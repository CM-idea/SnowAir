#include "pch.h"
#include "LwPpOcrEngine.h"
#include "lw_infer.h"          // lw.PPOCR.C 的公开 C ABI（头文件原样引入，见其 MIT 许可）
#include "OcrPackVariant.h"
#include "PluginPaths.h"
#include "../Util.h"

#include <mutex>
#include <string>
#include <vector>

namespace {

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

std::filesystem::path modelsDir()
{
	return PluginPaths::ocrModels() / OcrPack::folderName(OcrPackVariant::Official);
}

// 加载 lw_ppocr_c.dll 并绑定符号。已加载则直接返回
bool loadModule(std::wstring& err)
{
	if (g_mod) return true;
	const auto dll = PluginPaths::ocrRuntime(OcrPackVariant::Official) / L"lw_ppocr_c.dll";
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
	return true;
}

} // namespace

bool LwPpOcrEngine::isInstalled()
{
	std::error_code ec;
	if (!std::filesystem::is_regular_file(
		PluginPaths::ocrRuntime(OcrPackVariant::Official) / L"lw_ppocr_c.dll", ec))
		return false;
	const auto dir = modelsDir();
	for (const wchar_t* name : { L"det.lwm", L"cls.lwm", L"rec.lwm", L"ppocr_keys.txt" }) {
		const auto p = dir / name;
		if (!std::filesystem::is_regular_file(p, ec)) return false;
		if (std::filesystem::file_size(p, ec) == 0) return false;
	}
	return true;
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

	// BGRA → BGR（该 ABI 只吃 BGR8；用显式转换而不是拿 stride 跳 alpha，语义更明确）
	const size_t pixels = (size_t)w * (size_t)h;
	g_bgr.resize(pixels * 3);
	for (size_t i = 0; i < pixels; ++i) {
		g_bgr[i * 3 + 0] = bgra[i * 4 + 0];
		g_bgr[i * 3 + 1] = bgra[i * 4 + 1];
		g_bgr[i * 3 + 2] = bgra[i * 4 + 2];
	}

	for (int attempt = 0; attempt < 2; ++attempt) {
		lw_ocr_result res;
		g_api.resultInit(&res);
		lw_error error;
		g_api.errorInit(&error);
		const lw_status st = g_api.run(g_ocr, g_bgr.data(), (uint64_t)g_bgr.size(),
			(uint32_t)w, (uint32_t)h, (uint32_t)w * 3u,
			g_lines.data(), (uint32_t)g_lines.size(), g_text.data(), (uint64_t)g_text.size(),
			&res, &error);
		if (st != LW_STATUS_OK) {
			r.error = L"PP-OCRv6 识别失败：" + errText(st, error);
			return r;
		}
		// 容量不够（比如行数比预估多）：按报出来的需求扩一次再跑
		if (res.required_line_capacity > g_lines.size()
			|| res.required_text_capacity > g_text.size()) {
			if (attempt == 0) {
				g_lines.resize(res.required_line_capacity);
				g_text.resize((size_t)res.required_text_capacity);
				continue;
			}
			r.error = L"PP-OCRv6 结果超出容量上限";
			return r;
		}

		std::wstring joined;
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
			OcrRegion reg;
			reg.text = text;
			reg.displayText = text;
			reg.x = std::max(0.f, minX);
			reg.y = std::max(0.f, minY);
			reg.w = std::max(2.f, std::min(maxX, (float)w) - reg.x);
			reg.h = std::max(2.f, std::min(maxY, (float)h) - reg.y);
			r.regions.push_back(reg);

			if (!joined.empty()) joined += L'\n';
			joined += text;
		}
		r.text = joined;
		r.ok = !r.regions.empty();
		if (!r.ok) r.error = L"PP-OCRv6 未识别到文字";
		return r;
	}
	r.error = L"PP-OCRv6 识别失败";
	return r;
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
