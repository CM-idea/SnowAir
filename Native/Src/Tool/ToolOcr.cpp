#include "pch.h"
#include "../Win/WinCap.h"
#include "../Lang.h"
#include "../Tip.h"
#include "ToolOcr.h"
#include "IconCodes.h"
#include "ToolbarTheme.h"

namespace {
	// Microsoft YaHei 的 CJK 字形墨迹高 / 字号 ≈ 0.875（实测）。用它把"字号"和"文字实际占多高"互相换算。
	constexpr float kYaheiInkRatio = 0.875f;
	// 收框时文字纵向允许占到的框高比例。识别框本身就是贴着文字的，取 1.0 才等于"还原原来的大小"，
	// 留白（如 0.9）会把紧框的字又压小一圈。
	constexpr float kBoxHeightFill = 1.0f;

	// 量一段文本按指定字号排成一行（不折行）时的宽度，设备像素。
	// 字体取 TextBox 的默认族名，保证量出来的宽就是最终渲染的宽。
	float measureTextWidth(const std::wstring& s, float fontSizeLogical, float dpi)
	{
		auto* d2d = Ling::D2D::get();
		if (!d2d || !d2d->dwriteFactory || s.empty()) return 0.f;
		auto fmt = d2d->getTextFormat(L"");
		if (!fmt) return 0.f;
		Microsoft::WRL::ComPtr<IDWriteTextLayout> tl;
		if (FAILED(d2d->dwriteFactory->CreateTextLayout(s.c_str(), (UINT32)s.size(),
			fmt, FLT_MAX, FLT_MAX, tl.GetAddressOf())) || !tl)
			return 0.f;
		tl->SetFontSize(fontSizeLogical * dpi, { 0, INT_MAX });
		DWRITE_TEXT_METRICS m{};
		if (FAILED(tl->GetMetrics(&m))) return 0.f;
		return m.widthIncludingTrailingWhitespace > 0.f ? m.widthIncludingTrailingWhitespace : m.width;
	}

	// 逐通道中位数
	int medianOf(std::vector<int>& v)
	{
		const size_t mid = v.size() / 2;
		std::nth_element(v.begin(), v.begin() + mid, v.end());
		return v[mid];
	}

	// 单个识别框的背景色：只采「贴着框边的几圈」像素，再逐通道取中位数。
	// 框内是「文字 + 背景」混在一起，取平均会随文字覆盖率变化（纯色背景也会一块深一块浅）；
	// 边缘一圈基本是纯背景，中位数又对少数属于文字的像素天然免疫。
	bool regionBg(const std::vector<BYTE>& px, int W, int H,
		float fx, float fy, float fw, float fh, int& outR, int& outG, int& outB)
	{
		const int left = std::max(0, (int)std::floor(fx));
		const int top = std::max(0, (int)std::floor(fy));
		const int right = std::min(W, (int)std::ceil(fx + fw));
		const int bottom = std::min(H, (int)std::ceil(fy + fh));
		if (right - left < 1 || bottom - top < 1) return false;
		const int band = std::max(1, std::min(2, (bottom - top) / 4));
		std::vector<int> rs, gs, bs;
		auto push = [&](int x, int y) {
			if (x < 0 || y < 0 || x >= W || y >= H) return;
			const BYTE* p = px.data() + ((size_t)y * W + x) * 4;   // BGRA
			if (p[3] < 32) return;
			rs.push_back(p[2]); gs.push_back(p[1]); bs.push_back(p[0]);
		};
		for (int b = 0; b < band; ++b) {
			const int yT = top + b, yB = bottom - 1 - b;
			for (int x = left; x < right; ++x) { push(x, yT); push(x, yB); }
			const int xL = left + b, xR = right - 1 - b;
			for (int y = top; y < bottom; ++y) { push(xL, y); push(xR, y); }
		}
		if (rs.size() < 2) return false;
		outR = medianOf(rs); outG = medianOf(gs); outB = medianOf(bs);
		return true;
	}

	// 整层底色：对各框背景色再取中位数（纯色背景下每个框结果一致 → 整层同色，不会一块深一块浅）；
	// 文字色按底色亮度反相（亮底近黑 / 暗底近白），保证任何主题、任何截图上都读得清。
	void sampleLayerPalette(const std::vector<BYTE>& px, int W, int H,
		const std::vector<OcrRegion>& regions, uint32_t& outBg, uint32_t& outFg)
	{
		outBg = 0xFFFFFFF2;   // 兜底：浅底深字（采样不到时也不给空值）
		outFg = 0x14181CFF;
		if (px.empty() || W <= 0 || H <= 0 || regions.empty()) return;
		if ((size_t)W * (size_t)H * 4 > px.size()) return;
		std::vector<int> rs, gs, bs;
		for (const auto& reg : regions) {
			int r = 0, g = 0, b = 0;
			if (regionBg(px, W, H, reg.x, reg.y, reg.w, reg.h, r, g, b)) {
				rs.push_back(r); gs.push_back(g); bs.push_back(b);
			}
		}
		if (rs.empty()) return;
		const int r = medianOf(rs), g = medianOf(gs), b = medianOf(bs);
		// 底色不透明：盖死原字，不留半透明叠影
		outBg = ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | 0xFFu;
		const int lum = (r * 299 + g * 587 + b * 114) / 1000;
		outFg = (lum > 150) ? 0x14181CFF : 0xF8FBFBFF;
	}

	// 框内文字的"墨迹高度"（image 像素）：先取框最外一圈的中位亮度当背景，再逐行数前景像素，
	// 掐掉上下零星的抗锯齿/标点尾巴，得到真实字身高度。
	// 它与检测框松紧无关 —— 系统 OCR 的框紧贴字身、PP-OCR 的框带边距，同一个字量出的墨迹高一致，
	// 所以能拿它当跨引擎的统一标尺，避免"同一个字、不同引擎给出不同字号"。
	float estimateInkHeight(const std::vector<BYTE>& px, int W, int H,
		float fx, float fy, float fw, float fh)
	{
		if (px.empty() || W <= 0 || H <= 0 || (size_t)W * (size_t)H * 4 > px.size()) return 0.f;
		const int x0 = std::clamp((int)std::floor(fx), 0, W - 1);
		const int y0 = std::clamp((int)std::floor(fy), 0, H - 1);
		const int x1 = std::clamp((int)std::ceil(fx + fw), x0 + 1, W);
		const int y1 = std::clamp((int)std::ceil(fy + fh), y0 + 1, H);
		const int bw = x1 - x0, bh = y1 - y0;
		if (bw < 4 || bh < 4) return 0.f;

		auto gray = [&](int x, int y) -> int {
			const BYTE* p = px.data() + ((size_t)y * W + x) * 4;   // BGRA
			return (p[2] * 299 + p[1] * 587 + p[0] * 114) / 1000;
		};
		std::vector<int> border;
		border.reserve((size_t)(bw + bh) * 2);
		for (int x = x0; x < x1; ++x) { border.push_back(gray(x, y0)); border.push_back(gray(x, y1 - 1)); }
		for (int y = y0; y < y1; ++y) { border.push_back(gray(x0, y)); border.push_back(gray(x1 - 1, y)); }
		if (border.size() < 4) return 0.f;
		const int bgLum = medianOf(border);
		const bool darkText = bgLum > 128;   // 浅底深字 / 深底浅字

		std::vector<int> counts(bh, 0);
		int maxCount = 0;
		for (int y = 0; y < bh; ++y) {
			int n = 0;
			for (int x = 0; x < bw; ++x) {
				const int g = gray(x0 + x, y0 + y);
				if (darkText ? (g < bgLum - 40) : (g > bgLum + 40)) ++n;
			}
			counts[y] = n;
			maxCount = std::max(maxCount, n);
		}
		if (maxCount < std::max(2, bw / 40)) return 0.f;   // 几乎无前景：纯色块 / 对比度不足

		const int thr = std::max(1, (int)(maxCount * 0.10f));
		int first = -1, last = -1;
		for (int y = 0; y < bh; ++y) {
			if (counts[y] >= thr) { if (first < 0) first = y; last = y; }
		}
		if (first < 0 || last < first) return 0.f;
		return (float)(last - first + 1);
	}

	// 逐框字号（单位：image 像素）。
	//   正文：取全局「中位框高」统一，再用墨迹高 × K 标定 —— 单个框的高度不可靠（带边距/行高拟合
	//   不准），逐框按各自框高算就会"一大一小"；而墨迹高跨引擎可比，能给出同一字号。
	//   标题（框高明显大于正文）保留自身字号，避免把大标题压成正文。
	std::vector<float> unifiedFontSizes(const std::vector<BYTE>& px, int W, int H,
		const std::vector<OcrRegion>& regions)
	{
		std::vector<float> out(regions.size(), 0.f);
		if (regions.empty()) return out;

		std::vector<int> h;
		h.reserve(regions.size());
		for (const auto& reg : regions)
			if (reg.h >= 2.f) h.push_back((int)std::lround(reg.h));
		if (h.empty()) return out;

		std::sort(h.begin(), h.end());
		// 正文基准用「偏低分位」而不是中位数：标题/大字的框会把中位抬高，阈值跟着抬高，
		// 标题反而判不出来（实测 V5 框高 [20,21,29,41] 中位 29 → 阈值 42 → 41 的标题被当成正文，
		// 于是全屏被统一成一个偏大的字号）。取 1/4 分位，对少数大框不敏感。
		const float bodyRef = (float)h[h.size() / 4];
		const float headingThresh = bodyRef * 1.45f;
		float bodySize = std::clamp(bodyRef * 0.88f, 8.f, 96.f);

		if (bodyRef > 0.f && !px.empty() && W > 0 && H > 0 && (size_t)W * (size_t)H * 4 <= px.size()) {
			std::vector<float> ink(regions.size(), -1.f);
			auto inkOf = [&](size_t i) -> float {
				if (ink[i] < 0.f)
					ink[i] = estimateInkHeight(px, W, H, regions[i].x, regions[i].y, regions[i].w, regions[i].h);
				return ink[i];
			};
			// 正文候选：先按框高排除明显更高的（标题类）
			std::vector<float> inks;
			for (size_t i = 0; i < regions.size(); ++i) {
				if (regions[i].h < 2.f || regions[i].h >= headingThresh) continue;
				const float ik = inkOf(i);
				if (ik >= 3.f) inks.push_back(ik);
			}
			if (inks.size() >= 2) {
				// 字号 ≈ 墨迹高 / 0.875：这样渲染出来的墨迹高才和原图一致。
				// 墨迹高是"与检测框松紧无关"的量（系统 OCR 框紧贴字身、PP-OCR 框带边距，同一行字量出来一样），
				// 拿它当尺子，两个引擎才会给出同一个字号。
				constexpr float kInkToFont = 1.f / kYaheiInkRatio;
				// 单框字号：优先用墨迹高标定（跨引擎可比）；量不到墨迹才退回按框高估
				auto fontOf = [&](size_t i) -> float {
					const float ik = inkOf(i);
					return std::clamp(ik >= 3.f ? ik * kInkToFont : regions[i].h * 0.88f, 8.f, 96.f);
				};
				// 正文里还会混着"比正文大一档"的行（比如正文下方的链接文字）。用墨迹中位数再分一次档：
				// 墨迹明显高于中位（≥1.15×）的保留自己的字号，不并进正文统一 ——
				// 否则会被压成正文大小，看起来"和正文一样大"。
				float inkMed = 0.f;
				{
					std::vector<float> s = inks;
					std::sort(s.begin(), s.end());
					inkMed = s[s.size() / 2];
				}
				auto isOwnSize = [&](size_t i) -> bool {
					if (regions[i].h > headingThresh) return true;
					const float ik = inkOf(i);
					return inkMed > 0.f && ik >= 3.f && ik > inkMed * 1.15f;
				};
				// 正文：逐框算完取偏低分位统一 —— 每个框的墨迹有 ±1px 误差，统一后才不会"一大一小"。
				std::vector<float> bodyFonts;
				for (size_t i = 0; i < regions.size(); ++i) {
					if (regions[i].h < 2.f || isOwnSize(i)) continue;
					bodyFonts.push_back(fontOf(i));
				}
				if (!bodyFonts.empty()) {
					std::sort(bodyFonts.begin(), bodyFonts.end());
					bodySize = bodyFonts[bodyFonts.size() / 4];
				}
				for (size_t i = 0; i < regions.size(); ++i) {
					if (regions[i].h < 2.f) out[i] = bodySize;
					else out[i] = isOwnSize(i) ? fontOf(i) : bodySize;
				}

				// 行距上限：检测框紧贴字身时"框 + 两侧留白"会比行距还高，相邻框互相压成一道遮挡带。
				// 只压正文 —— 标题/大一号的行本来就比行距高，压下去就跟正文一样大了。
				std::vector<float> centers;
				centers.reserve(regions.size());
				for (const auto& reg : regions)
					if (reg.h >= 2.f) centers.push_back(reg.y + reg.h * 0.5f);
				std::sort(centers.begin(), centers.end());
				float minGap = 0.f;
				for (size_t i = 1; i < centers.size(); ++i) {
					const float g = centers[i] - centers[i - 1];
					if (g > 3.f && (minGap <= 0.f || g < minGap)) minGap = g;
				}
				const float lineCap = minGap > 6.f ? std::clamp(minGap * 0.78f, 8.f, 96.f) : 0.f;
				if (lineCap > 0.f) {
					for (size_t i = 0; i < out.size(); ++i) {
						if (isOwnSize(i)) continue;
						out[i] = std::max(8.f, std::min(out[i], lineCap));
					}
				}
				return out;
			}
		}

		for (size_t i = 0; i < regions.size(); ++i) {
			const float hh = regions[i].h;
			if (hh < 2.f) { out[i] = bodySize; continue; }
			out[i] = (hh > headingThresh) ? std::clamp(hh * 0.88f, 8.f, 96.f) : bodySize;
		}
		return out;
	}
}

ToolOcr::ToolOcr(WinCap* win, OcrResult result, bool translateMode,
	std::vector<BYTE> image, int imageW, int imageH)
	: Ling::WinBase(), win(win), result_(std::move(result)), translateMode_(translateMode),
	  imgW(std::max(1, imageW)), imgH(std::max(1, imageH)), srcImage_(std::move(image))
{
	dpi = win->dpi;
	sampleLayerPalette(srcImage_, imgW, imgH, result_.regions, palBg_, palFg_);
	onKeyDown.add([this](UINT key) {
		if ((GetKeyState(VK_CONTROL) & 0x8000) && (key == 'A' || key == 'C' || key == 'X'))
			return;
		this->win->onKeyDown(key);
	});
}

ToolOcr::~ToolOcr()
{
	// WinBase 的析构不销毁 HWND：不先 close 就 reset，窗口会变成"幽灵窗"，
	// 其 WndProc 仍指向已释放的对象 —— 鼠标划过即重入已释放内存，反复开关几次就闪退。
	if (hwnd) { close(); hwnd = nullptr; }
}

void ToolOcr::onCreated()
{
	body->setBg(0);   // 透明面板：只盖住识别到的文字块，其余区域直接露出原图
	body->setBorderRadius(0.f);

	copyBtn = body->makeChild<Ling::Button>();
	copyBtn->setText(Icon::Copy);
	copyBtn->setSize(34.f, 34.f);
	copyBtn->setPositionType(Ling::Position::Absolute);
	copyBtn->setPosition(Ling::Edge::Right, 4.f);
	copyBtn->setPosition(Ling::Edge::Top, 4.f);
	copyBtn->setBg(0xFFFFFFB0);          // 半透明浅底：底图深浅不定，按钮自身要有一点底色才看得见
	copyBtn->setHoverBg(0xFFFFFFF2);
	copyBtn->setBorderRadius(ToolbarTheme::hoverRadius);
	copyBtn->setAlignItems(Ling::Align::Center);
	copyBtn->setJustifyContent(Ling::Justify::Center);
	copyBtn->setFontFamily(Icon::Family);
	copyBtn->setFontSize(Icon::Size);
	copyBtn->setColor(0x262626FF);
	copyBtn->setHoverColor(0x262626FF);
	copyBtn->onClick.add([this](Ling::Button*) { copyAll(); });

	// 悬停提示（与其他工具条按钮一致：停悬 1 秒后弹出）
	tip = std::make_unique<Tip>(this);
	tip->bind(copyBtn, Lang::get(L"ocr.copy"));

	bannerNode = body->makeChild<Ling::Node>();
	bannerNode->setPositionType(Ling::Position::Absolute);
	bannerNode->setPosition(Ling::Edge::Left, 8.f);
	bannerNode->setPosition(Ling::Edge::Top, 8.f);
	bannerNode->setPadding(8.f, 6.f, 8.f, 6.f);
	bannerNode->setBorderRadius(4.f);
	bannerNode->hide();
	bannerLabel = bannerNode->makeChild<Ling::Label>();
	bannerLabel->setFontSize(12.f);

	rebuildEditors();
	show();
}

void ToolOcr::rebuildEditors()
{
	for (auto* e : editors) {
		if (e) body->removeChild(e);
	}
	editors.clear();

	if (!result_.ok) {
		showBanner(result_.error.empty() ? Lang::get(L"ocr.fail") : result_.error, true);
		return;
	}
	clearBanner();

	const float scaleX = (imgW > 0) ? (w / (float)imgW) : 1.f;
	const float scaleY = (imgH > 0) ? (h / (float)imgH) : 1.f;
	// 逐框字号统一标定（见 unifiedFontSizes）：正文一个字号、标题保留，避免"一大一小"
	const std::vector<float> fonts = unifiedFontSizes(srcImage_, imgW, imgH, result_.regions);
	constexpr float kPad = 2.f;

	for (size_t i = 0; i < result_.regions.size(); ++i) {
		auto& reg = result_.regions[i];
		auto* box = body->makeChild<Ling::TextBox>();
		box->setPositionType(Ling::Position::Absolute);
		box->setPosition(Ling::Edge::Left, reg.x * scaleX / dpi);
		box->setPosition(Ling::Edge::Top, reg.y * scaleY / dpi);
		box->setPadding(kPad);
		box->setBg(palBg_);
		box->setBorderRadius(2.f);
		const float fpx = (i < fonts.size() && fonts[i] > 0.f) ? fonts[i] : 16.f;
		const std::wstring text = reg.displayText.empty() ? reg.text : reg.displayText;
		// 字号单位是 image 像素，随缩放回到屏幕（逻辑像素）
		float fLogical = std::clamp(fpx * scaleY, 7.f, 96.f) / dpi;
		// 收进识别框：TextBox 开了 autoSize 会跟着文字长，不约束就会整段溢出框、盖到框外去。
		// 宽度和高度各给一条上限，取更紧的那个等比缩小（只缩不放）——
		// 宽度约束对长行是主导项，等于让"文字宽度 = 识别框宽度"。
		const float textW = measureTextWidth(text, fLogical, dpi);
		const float textH = fLogical * dpi * kYaheiInkRatio;
		const float boxW = reg.w * scaleX;
		const float boxH = reg.h * scaleY * kBoxHeightFill;
		float shrink = 1.f;
		if (textW > 0.f && boxW > 0.f) shrink = std::min(shrink, boxW / textW);
		if (textH > 0.f && boxH > 0.f) shrink = std::min(shrink, boxH / textH);
		if (shrink < 1.f) fLogical = std::max(7.f, fLogical * shrink);
		box->setFontSize(fLogical);
		box->setColor(palFg_);
		// 框体跟着文字走（不折行、永不出现滚动条）：识别框尺寸本身不准，硬按它定尺寸会让
		// 内容溢出、冒出滚动条；位置仍取识别框左上角，底色也就刚好吃住原来的那行字。
		box->setAutoSize(true);
		box->setText(text);
		editors.push_back(box);
	}
	if (editors.empty() && !result_.text.empty()) {
		auto* box = body->makeChild<Ling::TextBox>();
		box->setPositionType(Ling::Position::Absolute);
		box->setPosition(Ling::Edge::Left, 8.f);
		box->setPosition(Ling::Edge::Top, 40.f);
		box->setSize(std::max(80.f, w / dpi - 16.f), std::max(40.f, h / dpi - 48.f));
		box->setBg(palBg_);
		box->setColor(palFg_);
		box->setText(result_.text);
		editors.push_back(box);
	}
	// 不自动选中/聚焦：结果只是"叠在原图上供阅读"，一进来就整段高亮并不合适；点哪块再编辑哪块。
}

void ToolOcr::applyTranslate(const std::vector<std::wstring>& lines)
{
	for (size_t i = 0; i < result_.regions.size(); i++) {
		if (i < lines.size() && !lines[i].empty())
			result_.regions[i].displayText = lines[i];
		else if (result_.regions.size() == 1 && !lines.empty())
			result_.regions[i].displayText = lines[0];
	}
	result_.text.clear();
	for (size_t i = 0; i < result_.regions.size(); i++) {
		if (i) result_.text += L'\n';
		result_.text += result_.regions[i].displayText.empty()
			? result_.regions[i].text : result_.regions[i].displayText;
	}
	rebuildEditors();
}

void ToolOcr::setResult(OcrResult result)
{
	result_ = std::move(result);
	rebuildEditors();
}

void ToolOcr::showBanner(const std::wstring& msg, bool error)
{
	banner_ = msg;
	bannerError_ = error;
	if (!bannerNode || !bannerLabel) return;
	bannerNode->setBg(error ? 0xE81123CC : 0x333333CC);
	bannerLabel->setColor(0xFFFFFFFF);
	bannerLabel->setText(msg);
	bannerNode->show();
}

void ToolOcr::clearBanner()
{
	banner_.clear();
	if (bannerNode) bannerNode->hide();
}

void ToolOcr::copyAll()
{
	std::wstring text;
	for (size_t i = 0; i < editors.size(); i++) {
		if (i) text += L'\n';
		text += editors[i]->getText();
	}
	if (text.empty()) text = result_.text;
	if (OpenClipboard(hwnd)) {
		EmptyClipboard();
		size_t bytes = (text.size() + 1) * sizeof(wchar_t);
		HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
		if (h) {
			memcpy(GlobalLock(h), text.c_str(), bytes);
			GlobalUnlock(h);
			SetClipboardData(CF_UNICODETEXT, h);
		}
		CloseClipboard();
	}
}

void ToolOcr::syncToMask(const D2D1_RECT_F& maskRect, int hostX, int hostY, float hostDpi)
{
	dpi = hostDpi;
	const int left = (int)std::lround(maskRect.left);
	const int top = (int)std::lround(maskRect.top);
	const int right = (int)std::lround(maskRect.right);
	const int bottom = (int)std::lround(maskRect.bottom);
	imgW = std::max(1, right - left);
	imgH = std::max(1, bottom - top);
	setSize((float)imgW / dpi, (float)imgH / dpi);
	setPosition(hostX + left, hostY + top);
	rebuildEditors();
}

LRESULT ToolOcr::onHitTest(const POINT screenPos)
{
	if (win && win->hwnd && win->cutMask && win->cutMask->hasRect()) {
		POINT client = screenPos;
		ScreenToClient(win->hwnd, &client);
		const auto& r = win->cutMask->maskRect;
		if (client.x < r.left || client.x >= r.right || client.y < r.top || client.y >= r.bottom)
			return HTTRANSPARENT;
	}
	return HTCLIENT;
}

void ToolOcr::onMinMaxInfo(MINMAXINFO* mmi)
{
	mmi->ptMinTrackSize.x = 1;
	mmi->ptMinTrackSize.y = 1;
}
