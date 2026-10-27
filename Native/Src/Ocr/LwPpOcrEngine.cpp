#include "pch.h"
#include "LwPpOcrEngine.h"
#include "lw_infer.h"          // lw.PPOCR.C 的公开 C ABI（头文件原样引入，见其 MIT 许可）
#include "OcrPackVariant.h"
#include "PluginPaths.h"
#include "../Util.h"

#include <mutex>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace {

// 识别置信度下限：低于它的行整行丢弃（对齐 PP-OCR 官方后处理的 drop_score=0.5）
constexpr float kMinScore = 0.5f;

// 引擎与 DLL 都是进程级单例：OCR 会在后台线程被调用，故全程加锁
std::mutex g_mtx;
HMODULE g_mod{ nullptr };
lw_ocr* g_ocr{ nullptr };

// 输出缓冲：容量按 lw_ocr_get_info 报的上限一次性分配，之后复用（不随识别次数增长）
std::vector<lw_ocr_line> g_lines;
std::vector<char> g_text;
std::vector<BYTE> g_bgr;   // BGRA → BGR 的转换缓冲

// lw.PPOCR.C 导出符号（C ABI v1）
struct Api {
	decltype(&lw_ocr_create) create{ nullptr };
	decltype(&lw_ocr_free) free_{ nullptr };
	decltype(&lw_ocr_get_info) getInfo{ nullptr };
	decltype(&lw_ocr_run_bgr_u8) run{ nullptr };
	decltype(&lw_ocr_options_init) optionsInit{ nullptr };
	decltype(&lw_ocr_info_init) infoInit{ nullptr };
	decltype(&lw_ocr_result_init) resultInit{ nullptr };
	decltype(&lw_error_init) errorInit{ nullptr };
	decltype(&lw_status_string) statusString{ nullptr };
};
Api g_api;

std::wstring utf8ToW(const char* s, size_t len)
{
	if (!s || len == 0) return {};
	const int n = MultiByteToWideChar(CP_UTF8, 0, s, (int)len, nullptr, 0);
	if (n <= 0) return {};
	std::wstring out((size_t)n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s, (int)len, out.data(), n);
	return out;
}

std::wstring errText(lw_status st, const lw_error& e)
{
	std::wstring out = g_api.statusString ? utf8ToW(g_api.statusString(st), strlen(g_api.statusString(st))) : L"";
	const std::wstring msg = utf8ToW(e.message, strnlen(e.message, LW_ERROR_MESSAGE_CAPACITY));
	if (!msg.empty()) out += out.empty() ? msg : (L"：" + msg);
	return out.empty() ? L"未知错误" : out;
}

// 当前生效的档位（设置里选的是哪个就用哪个；没装则退到另一个已装档位）
OcrPackVariant g_variant{ OcrPackVariant::Small };
// 引擎里实际加载的档位：与 g_variant 不一致时释放重建
OcrPackVariant g_loadedVariant{ OcrPackVariant::Small };

bool modelsReady(OcrPackVariant v)
{
	std::error_code ec;
	const auto dir = PluginPaths::ocrModels() / OcrPack::folderName(v);
	for (const wchar_t* name : { L"det.lwm", L"cls.lwm", L"rec.lwm", L"ppocr_keys.txt" }) {
		const auto p = dir / name;
		if (!std::filesystem::is_regular_file(p, ec)) return false;
		if (std::filesystem::file_size(p, ec) == 0) return false;
	}
	return true;
}

OcrPackVariant resolveVariant()
{
	auto* s = Setting::get();
	const OcrPackVariant want = OcrPack::clampVariant(s ? s->getOcrPackVariant() : 0);
	if (modelsReady(want)) return want;
	// 用户把选中的那档删了：退到另一档，别让整个 OCR 直接不可用
	const OcrPackVariant other = want == OcrPackVariant::Small ? OcrPackVariant::Tiny : OcrPackVariant::Small;
	if (modelsReady(other)) return other;
	return want;
}

std::filesystem::path modelsDir()
{
	return PluginPaths::ocrModels() / OcrPack::folderName(g_variant);
}

bool isCjkChar(wchar_t c)
{
	return (c >= 0x3000 && c <= 0x303F) || (c >= 0x3040 && c <= 0x30FF)
		|| (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF)
		|| (c >= 0xFF00 && c <= 0xFFEF);
}

// 检测框把"图标 + 中文标签"并在了一行时（侧栏条目很常见），用像素列找图标与文字之间的空白。
// 做法：以框最外一圈的中位亮度当底色（深底浅字/浅底深字都适用），逐列数墨迹像素；
// 行首第一簇墨迹与其后内容之间若有一段明显空白，就把这段空白之后的起点当成真正的文字起点。
// 正常字间距只有 1~3px，而图标与标签之间通常是 0.5 行高以上。
// 返回 true 时 textStartX = 文字实际开始的列（绝对坐标）。
bool findLeadingIconGap(const BYTE* bgr, int imgW, int imgH,
	int x0, int y0, int x1, int y1, int& textStartX, int& gapW, int& clusterW)
{
	x0 = std::clamp(x0, 0, imgW - 1);
	y0 = std::clamp(y0, 0, imgH - 1);
	x1 = std::clamp(x1, x0 + 1, imgW);
	y1 = std::clamp(y1, y0 + 1, imgH);
	const int bw = x1 - x0, bh = y1 - y0;
	if (bw < 8 || bh < 6) return false;

	auto gray = [&](int x, int y) -> int {
		const BYTE* p = bgr + ((size_t)y * imgW + x) * 3;
		return (p[2] * 299 + p[1] * 587 + p[0] * 114) / 1000;
	};
	std::vector<int> border;
	border.reserve((size_t)(bw + bh) * 2);
	for (int x = x0; x < x1; ++x) { border.push_back(gray(x, y0)); border.push_back(gray(x, y1 - 1)); }
	for (int y = y0; y < y1; ++y) { border.push_back(gray(x0, y)); border.push_back(gray(x1 - 1, y)); }
	std::nth_element(border.begin(), border.begin() + border.size() / 2, border.end());
	const int bg = border[border.size() / 2];

	std::vector<int> cols((size_t)bw, 0);
	for (int y = y0; y < y1; ++y) {
		for (int x = x0; x < x1; ++x) {
			if (std::abs(gray(x, y) - bg) > 40) ++cols[(size_t)(x - x0)];
		}
	}

	int first = -1;
	for (int c = 0; c < bw; ++c) if (cols[(size_t)c] > 0) { first = c; break; }
	if (first < 0) return false;

	// 图标本身可能是两三笔（列方向上被 1~2px 的缝分开），先按"小缝"把簇连起来，
	// 再看这个簇后面有没有"大缝"。小缝阈值取 0.2 行高，大缝要求 0.45 行高以上。
	const int smallGap = std::max(2, (int)std::lround(bh * 0.2f));
	int c = first;
	int clusterEnd = -1;
	while (c < bw) {
		if (cols[(size_t)c] > 0) { ++c; continue; }
		const int s = c;
		while (c < bw && cols[(size_t)c] == 0) ++c;
		if (c - s >= smallGap) { clusterEnd = s; break; }
	}
	if (clusterEnd <= first) return false;
	int n = clusterEnd;
	while (n < bw && cols[(size_t)n] == 0) ++n;
	if (n >= bw) return false;   // 整个框只有一簇：没有可切的东西

	textStartX = x0 + n;
	gapW = n - clusterEnd;
	clusterW = clusterEnd - first;
	return true;
}


// 加载 lw_ppocr_c.dll 并绑定符号。已加载则直接返回
bool loadModule(std::wstring& err)
{
	if (g_mod) return true;
	const auto dll = PluginPaths::ocrRuntime(OcrPackVariant::Tiny) / L"lw_ppocr_c.dll";
	std::error_code ec;
	if (!std::filesystem::is_regular_file(dll, ec)) {
		err = L"PP-OCRv6 运行时未安装，请到「插件集成」下载";
		return false;
	}
	HMODULE mod = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!mod) {
		err = L"lw_ppocr_c.dll 加载失败（可能缺少同目录的运行库）";
		return false;
	}
	auto sym = [mod](const char* name) { return GetProcAddress(mod, name); };
	g_api.create = (decltype(g_api.create))sym("lw_ocr_create");
	g_api.free_ = (decltype(g_api.free_))sym("lw_ocr_free");
	g_api.getInfo = (decltype(g_api.getInfo))sym("lw_ocr_get_info");
	g_api.run = (decltype(g_api.run))sym("lw_ocr_run_bgr_u8");
	g_api.optionsInit = (decltype(g_api.optionsInit))sym("lw_ocr_options_init");
	g_api.infoInit = (decltype(g_api.infoInit))sym("lw_ocr_info_init");
	g_api.resultInit = (decltype(g_api.resultInit))sym("lw_ocr_result_init");
	g_api.errorInit = (decltype(g_api.errorInit))sym("lw_error_init");
	g_api.statusString = (decltype(g_api.statusString))sym("lw_status_string");
	if (!g_api.create || !g_api.free_ || !g_api.getInfo || !g_api.run || !g_api.optionsInit
		|| !g_api.infoInit || !g_api.resultInit || !g_api.errorInit) {
		FreeLibrary(mod);
		err = L"lw_ppocr_c.dll 导出符号不匹配（需要 lw.PPOCR.C 1.x 的 C ABI v1）";
		return false;
	}
	g_mod = mod;
	return true;
}

// 建引擎（已建则直接返回）。det/cls/rec 用官方的 .lwm 权重，字典是 ppocr_keys.txt
bool ensureEngine(std::wstring& err)
{
	// 档位跟着设置走（设置里切了档位 → 放掉旧引擎重建）
	g_variant = resolveVariant();
	if (g_ocr && g_variant != g_loadedVariant) {
		LwPpOcrEngine::release();
	}
	if (g_ocr) return true;
	if (!loadModule(err)) return false;

	const auto dir = modelsDir();
	const auto det = dir / L"det.lwm";
	const auto cls = dir / L"cls.lwm";
	const auto rec = dir / L"rec.lwm";
	const auto dict = dir / L"ppocr_keys.txt";
	std::error_code ec;
	for (const auto& p : { det, cls, rec, dict }) {
		if (!std::filesystem::is_regular_file(p, ec)) {
			err = L"PP-OCRv6 模型文件缺失（" + p.filename().wstring() + L"），请到「插件集成」重新下载";
			return false;
		}
	}

	lw_ocr_options options;
	g_api.optionsInit(&options);
	// 方向分类阈值：默认值偏高（约 0.9），"竖排但置信度中等"的整列会被判成 0°、不做 180° 翻转，
	// 识别结果直接变成乱码（实测 539×730 日文里有一列被认成"十1"）。0.7 能把这类列翻正，
	// 横排文本的分类置信度通常接近 1.0，不受影响。
	options.classifier_threshold = 0.7f;
	// REC 动态宽度上限。只限制最长行，短行仍按自身宽度算、不受影响；
	// 压到 640 会把长行水平压扁（867px 宽的英文行实测识别成乱码、置信度 0.39 被整行丢弃）。
	options.recognizer.target_width = 3200;
	// worker_count 留 0 = 平台默认（按 CPU 上限 8）：截图选区一般只有几行，够用；
	// 想更省内存可以在这里设 1（实测峰值 103MB → 约 84MB），代价是多行的识别慢一些。

	const std::string sDet = Ling::Util::convertToStr(det.wstring());
	const std::string sCls = Ling::Util::convertToStr(cls.wstring());
	const std::string sRec = Ling::Util::convertToStr(rec.wstring());
	const std::string sDict = Ling::Util::convertToStr(dict.wstring());

	lw_error error;
	g_api.errorInit(&error);
	const lw_status st = g_api.create(sDet.c_str(), sCls.c_str(), sRec.c_str(), sDict.c_str(),
		&options, &g_ocr, &error);
	if (st != LW_STATUS_OK || !g_ocr) {
		g_ocr = nullptr;
		err = L"PP-OCRv6 引擎初始化失败：" + errText(st, error);
		return false;
	}

	lw_ocr_info info;
	g_api.infoInit(&info);
	if (g_api.getInfo(g_ocr, &info) != LW_STATUS_OK || info.max_line_capacity == 0
		|| info.max_text_capacity == 0) {
		err = L"PP-OCRv6 无法获取输出容量";
		g_api.free_(g_ocr);
		g_ocr = nullptr;
		return false;
	}
	g_lines.assign(info.max_line_capacity, lw_ocr_line{});
	g_text.assign((size_t)info.max_text_capacity, 0);
	g_loadedVariant = g_variant;
	return true;
}

} // namespace

bool LwPpOcrEngine::isInstalled()
{
	std::error_code ec;
	if (!std::filesystem::is_regular_file(
		PluginPaths::ocrRuntime(OcrPackVariant::Tiny) / L"lw_ppocr_c.dll", ec))
		return false;
	return modelsReady(OcrPackVariant::Tiny) || modelsReady(OcrPackVariant::Small);
}

OcrResult LwPpOcrEngine::recognize(int w, int h, const BYTE* bgra)
{
	OcrResult r;
	r.engineUsed = OcrEngineKind::PpOcrV6;
	if (w <= 0 || h <= 0 || !bgra) {
		r.error = L"图片为空";
		return r;
	}

	std::lock_guard<std::mutex> lock(g_mtx);
	std::wstring err;
	if (!ensureEngine(err)) {
		r.error = err;
		return r;
	}

	// 原始识别行（原图坐标，尚未做置信度过滤）
	struct RawLine {
		std::wstring text;
		float score{ 0.f };
		float x{ 0.f }, y{ 0.f }, w{ 0.f }, h{ 0.f };
		bool vertical{ false };
	};

	// 把一张 BGRA（可能是放大后的图）跑一遍，坐标按 invScale 除回原图空间
	auto runOnce = [&](const BYTE* src, int sw, int sh, float invScale,
		std::wstring& error) -> std::vector<RawLine> {
		std::vector<RawLine> out;
		const size_t pixels = (size_t)sw * (size_t)sh;
		g_bgr.resize(pixels * 3);
		for (size_t i = 0; i < pixels; ++i) {
			g_bgr[i * 3 + 0] = src[i * 4 + 0];
			g_bgr[i * 3 + 1] = src[i * 4 + 1];
			g_bgr[i * 3 + 2] = src[i * 4 + 2];
		}

		for (int attempt = 0; attempt < 2; ++attempt) {
			lw_ocr_result res;
			g_api.resultInit(&res);
			lw_error e;
			g_api.errorInit(&e);
			const lw_status st = g_api.run(g_ocr, g_bgr.data(), (uint64_t)g_bgr.size(),
				(uint32_t)sw, (uint32_t)sh, (uint32_t)sw * 3u,
				g_lines.data(), (uint32_t)g_lines.size(), g_text.data(), (uint64_t)g_text.size(),
				&res, &e);
			if (st != LW_STATUS_OK) {
				error = L"PP-OCRv6 识别失败：" + errText(st, e);
				return {};
			}
			// 容量不够（比如行数比预估多）：按报出来的需求扩一次再跑
			if (res.required_line_capacity > g_lines.size()
				|| res.required_text_capacity > g_text.size()) {
				if (attempt == 0) {
					g_lines.resize(res.required_line_capacity);
					g_text.resize((size_t)res.required_text_capacity);
					continue;
				}
				error = L"PP-OCRv6 结果超出容量上限";
				return {};
			}

			for (uint32_t i = 0; i < res.line_count; ++i) {
				const lw_ocr_line& ln = g_lines[i];
				if (ln.text_offset >= g_text.size()) continue;
				const size_t len = (size_t)std::min<uint64_t>(ln.text_length, g_text.size() - ln.text_offset);
				std::wstring text = utf8ToW(g_text.data() + ln.text_offset, len);
				if (text.empty()) continue;

				// 四点四边形 → 包围盒（调用方按 x/y/w/h 排版）
				const float xs[4]{ ln.box.x1, ln.box.x2, ln.box.x3, ln.box.x4 };
				const float ys[4]{ ln.box.y1, ln.box.y2, ln.box.y3, ln.box.y4 };
				float minX = xs[0], maxX = xs[0], minY = ys[0], maxY = ys[0];
				for (int k = 1; k < 4; ++k) {
					minX = std::min(minX, xs[k]); maxX = std::max(maxX, xs[k]);
					minY = std::min(minY, ys[k]); maxY = std::max(maxY, ys[k]);
				}

				// 行首图标：检测框把图标和中文标签并在了一起（侧栏 "H系统按键"），识别结果会多出一个
				// 孤立字符，叠在原图上还会压住图标。三个条件同时成立才处理，避免误伤正常文本：
				//   ① 行首是 ASCII 字符、后面紧跟（可隔一个空格）方块字；
				//   ② 像素列上图标与文字之间有明显空白（≥0.45 行高，正常字间距只有 1~3px），
				//      且行首那一簇的宽度像图标（0.55~1.2 行高；"1"、"l" 这类细字够不到下限）；
				//   ③ 整行置信度不是满分（图标不是文字，会把这个值拉低）。
				if (text.size() >= 3 && ln.recognition_score < 0.99f && text[0] > 0x20 && text[0] < 0x80) {
					size_t k = 1;
					if (k < text.size() && text[k] == L' ') ++k;
					if (k < text.size() && isCjkChar(text[k])) {
						int textStartX = 0, gapW = 0, clusterW = 0;
						const float lineH = maxY - minY;
						if (findLeadingIconGap(g_bgr.data(), sw, sh,
								(int)std::floor(minX), (int)std::floor(minY),
								(int)std::ceil(maxX), (int)std::ceil(maxY),
								textStartX, gapW, clusterW)
							&& (float)gapW >= std::max(4.f, lineH * 0.45f)
							&& (float)clusterW >= lineH * 0.55f
							&& (float)clusterW <= lineH * 1.2f) {
							text.erase(0, k);
							minX = std::max(minX, (float)textStartX);
							maxX = std::max(maxX, minX + 2.f);
						}
					}
				}

				RawLine line;
				line.text = std::move(text);
				line.score = ln.recognition_score;
				line.vertical = (maxY - minY) >= (maxX - minX) * 1.5f;   // 高瘦框 = 竖排
				line.x = minX * invScale;
				line.y = minY * invScale;
				line.w = (maxX - minX) * invScale;
				line.h = (maxY - minY) * invScale;
				out.push_back(std::move(line));
			}
			return out;
		}
		return out;
	};

	// 过滤 + 打包成 OcrResult（放大图/原图两条路共用）
	auto Finish = [&](const std::vector<RawLine>& lines, int fw, int fh,
		const std::wstring& runErr) -> OcrResult {
		OcrResult out;
		out.engineUsed = OcrEngineKind::PpOcrV6;
		if (lines.empty()) {
			out.error = runErr.empty() ? L"PP-OCRv6 未识别到文字" : runErr;
			return out;
		}
		std::wstring joined;
		for (const auto& line : lines) {
			// 丢弃低置信度结果（PP-OCR 官方后处理的 drop_score 默认就是 0.5）。
			// 深色 UI 截图里的图标、分隔线常被误检成一两个字符，不滤掉会盖住原图非文字区域。
			if (line.score < kMinScore) continue;
			// 单字符 + 框近似正方形 + 置信度不高 → 基本是图标被认成了字
			if (line.text.size() == 1 && line.score < 0.95f
				&& std::fabs(line.w - line.h) <= 0.35f * std::max(line.w, line.h))
				continue;

			// 按短边向外补 0.15 的边距：横排短边就是行高（与原来一致）；竖排按行高取会把
			// 31px 宽的列撑到 240px、相邻列叠在一起。（背景取样与字号标定需要这圈边距）
			const float pad = std::min(line.w, line.h) * 0.15f;
			OcrRegion reg;
			reg.text = line.text;
			reg.displayText = line.text;
			reg.vertical = line.vertical;
			const float x0 = (std::max)(0.f, line.x - pad);
			const float y0 = (std::max)(0.f, line.y - pad);
			const float x1 = (std::min)(line.x + line.w + pad, (float)fw);
			const float y1 = (std::min)(line.y + line.h + pad, (float)fh);
			reg.x = x0;
			reg.y = y0;
			reg.w = std::max(2.f, x1 - x0);
			reg.h = std::max(2.f, y1 - y0);
			out.regions.push_back(reg);

			if (!joined.empty()) joined += L'\n';
			joined += line.text;
		}
		out.text = joined;
		out.ok = !out.regions.empty();
		if (!out.ok) out.error = L"PP-OCRv6 未识别到文字";
		return out;
	};

	// 单次推理。检测器长边上限通常取 960：小图直接送进去时检测器只按"最大边"缩小、
	// 不会放大，细窄的竖列容易漏检/漏字（实测 539×730 会丢掉最左一列）。这里先把长边补到
	// 960 再送，一次就够用；整屏大图不缩放。
	// 只对**竖长/方图**补：这个问题来自竖排整列，横向小窗（如 800×600）放大既没必要也变慢。
	std::vector<BYTE> scaled;
	const int longSide = (std::max)(w, h);
	if (longSide < 960 && h >= w) {
		const float f = (std::min)(960.f / (float)longSide, 3.f);
		const int nw = (int)std::lround((float)w * f);
		const int nh = (int)std::lround((float)h * f);
		if ((int64_t)nw * nh <= 4'000'000) {
			scaled = Util::scaleBGRA(bgra, w, h, nw, nh);
			if (!scaled.empty()) {
				std::vector<RawLine> lines = runOnce(scaled.data(), nw, nh, 1.f / f, err);
				return Finish(lines, w, h, err);
			}
		}
	}
	{
		std::vector<RawLine> lines = runOnce(bgra, w, h, 1.f, err);
		return Finish(lines, w, h, err);
	}
}

void LwPpOcrEngine::release()
{
	std::lock_guard<std::mutex> lock(g_mtx);
	if (g_ocr && g_api.free_) g_api.free_(g_ocr);
	g_ocr = nullptr;
	if (g_mod) {
		FreeLibrary(g_mod);
		g_mod = nullptr;
	}
	g_api = Api{};
	// 缓冲区也一起还掉（不 shrink 的话那几十 KB~百 KB 会挂到进程退出）
	std::vector<lw_ocr_line>().swap(g_lines);
	std::vector<char>().swap(g_text);
	std::vector<BYTE>().swap(g_bgr);
}
