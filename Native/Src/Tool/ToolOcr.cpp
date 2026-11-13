#include "pch.h"
#include "../Win/WinCap.h"
#include "../Lang.h"
#include "../Setting.h"
#include "../Tip.h"
#include "ToolOcr.h"
#include "ToolOcrCompare.h"
#include "IconCodes.h"
#include "ToolbarTheme.h"

namespace {
	// 去掉段首圆点（• · ● …）及其后的空白：翻译前后都自己管圆点，左右才齐
	void stripLeadingBullet(std::wstring& t)
	{
		size_t k = 0;
		while (k < t.size() && (t[k] == L' ' || t[k] == L'\t')) ++k;
		if (k >= t.size()) return;
		switch (t[k]) {
		case L'•': case L'·': case L'●': case L'○': case L'▪': case L'◦':
		case L'※': case L'◆': case L'◇': case L'■': case L'□':
			t.erase(0, k + 1);
			while (!t.empty() && (t.front() == L' ' || t.front() == L'\t')) t.erase(0, 1);
			break;
		default:
			break;
		}
	}

	// 取出段首圆点字符（没有就返回 0）：文本重排时统计哪种圆点用得最多
	wchar_t leadingBulletChar(const std::wstring& t)
	{
		size_t k = 0;
		while (k < t.size() && (t[k] == L' ' || t[k] == L'\t')) ++k;
		if (k >= t.size()) return 0;
		switch (t[k]) {
		case L'•': case L'·': case L'●': case L'○': case L'▪': case L'◦':
		case L'※': case L'◆': case L'◇': case L'■': case L'□':
			return t[k];
		default:
			return 0;
		}
	}

	// Microsoft YaHei 的 CJK 字形墨迹高 / 字号 ≈ 0.875（实测）。用它把"字号"和"文字实际占多高"互相换算。
	constexpr float kYaheiInkRatio = 0.875f;
	// 收框时文字纵向允许占到的框高比例。识别框本身就是贴着文字的，取 1.0 才等于"还原原来的大小"，
	// 留白（如 0.9）会把紧框的字又压小一圈。
	constexpr float kBoxHeightFill = 1.0f;
	// 墨迹高 → 字号 的换算系数（见 kYaheiInkRatio）。
	constexpr float kInkToFont = 1.f / kYaheiInkRatio;
	// 方块字在 Microsoft YaHei 下实测墨迹高 / 字号 ≈ 0.96，并不占满整个 em。
	// 之前按 1.0 换算会把字号算小，中文重绘明显比原文小一圈。
	constexpr float kCjkInkRatio = 0.96f;
	// 识别框本身贴着文字，留 2% 余量防止贴边行被面板裁掉。
	constexpr float kCjkVFill = 0.98f;

	// 是否含"方块字"（汉字/假名/谚文/全角标点）——决定用哪个墨迹比
	bool hasWideGlyph(const std::wstring& s)
	{
		for (wchar_t c : s) {
			if ((c >= 0x3000 && c <= 0x303F) || (c >= 0x3040 && c <= 0x30FF)
				|| (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF)
				|| (c >= 0xFF00 && c <= 0xFFEF)) return true;
		}
		return false;
	}

	// 单个字符是不是方块字（合并折行时决定要不要补空格：中文之间不用，西文之间要）
	bool isCjkChar(wchar_t c)
	{
		return (c >= 0x3000 && c <= 0x303F) || (c >= 0x3040 && c <= 0x30FF)
			|| (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF)
			|| (c >= 0xFF00 && c <= 0xFFEF);
	}

	// 叠加用文字块：把文字**按真实排版尺寸**等比缩放并居中到识别框。
	// TextBox 没有字间距接口，用横向缩放代替（效果等同把单行文字左右拉满）。
	// 注意：缩放只改视觉，控件自身的命中矩形还是未缩放的位置 —— 这会让"点到的位置"和
	// 眼睛看到的文字对不上。所以缩放后把节点几何一并改写成**视觉矩形**，命中测试才落在
	// 真正看得见的文字上；ToolOcr 的跨行选区也直接读这个矩形做坐标映射。
	struct OcrText : Ling::TextBox
	{
		explicit OcrText(Ling::WinBase* win) : Ling::TextBox(win) {}
		float fitW{ 0.f };   // 识别框宽（物理像素）
		float fitH{ 0.f };   // 识别框高（物理像素）
		float boxL{ 0.f };   // 识别框左上角 X（父视觉坐标，物理像素）
		float boxT{ 0.f };   // 识别框左上角 Y
		float inkH{ 0.f };   // 当前字号下的字形高度（物理像素；竖排时是整列墨迹高）
		float vFill{ 1.f };  // 纵向允许占到的框高比例（CJK 留余量，防上下两行叠字）
		float scale_{ 1.f }; // 本次布局的视觉缩放比（命中测试用）
		int align{ 0 };      // 原文对齐：0=居左 1=居中 2=居右（译文比原文短时才有区别）
		// 竖排：控件里的文本是"每字一行"（用换行符堆叠），logical 才是真正要复制/选区的串。
		bool vertical{ false };
		std::wstring logical;
	protected:
		// layout() 里把节点几何改成了视觉矩形，命中测试要按同一比例换算回布局坐标
		float hitTestScale() const override { return scale_; }
		void layout() override
		{
			Ling::TextBox::layout();
			if (fitW <= 0.f || fitH <= 0.f || inkH <= 0.f || w <= 0.f || h <= 0.f) return;
			// 字号已经在建框时按识别框适配过（见 rebuildEditors 里的 measureTextSize）；
			// 这里只做最后兜底，并且**只缩不放**：放大等于把文字画布整体拉伸，边缘会发虚。
			// 竖排是"每字一行"的多行文本，行距由排版决定、不能再用 inkH 估算，直接按整列自然高度缩放。
			const float s = vertical
				? std::min(std::min(fitW / w, fitH * 0.98f / h), 1.f)
				// 横排：允许 6% 以内的横向溢出，不为了几像素再缩一次 ——
				// 逐行缩放会让同一段里的字（包括圆点）大小忽大忽小，看着不齐
				: std::min(std::min((w > fitW * 1.06f ? fitW / w : 1.f), fitH * vFill / inkH), 1.f);
			scale_ = s;
			// 水平按原文对齐摆放：译文比原文短、装不满框时，居左/居中/居右的差别才看得出来。
			// 居左 = 和原文一样从同一列起排；居中/居右照原样还原。
			const float visW = w * s;
			float visX = boxL;                                     // 居左
			if (align == 1) visX = boxL + (fitW - visW) * 0.5f;     // 居中
			else if (align == 2) visX = boxL + (fitW - visW);       // 居右
			// 位置对齐到物理像素：合成层按小数坐标摆放时会把文字画布重采样一遍，边缘也会发虚。
			const float d = (win && win->dpi > 0.f) ? win->dpi : 1.f;
			auto snap = [d](float v) { return std::round(v * d) / d; };
			// ox 是"未缩放节点"的偏移：缩放绕节点自身中心，故视觉左边缘 = ox + (w - visW)/2
			const float ox = snap(visX - (w - visW) * 0.5f);
			// 纵向仍居中：文字块的墨迹落在识别框中间，和原文的墨迹位置对齐。
			const float oy = snap(boxT + (fitH - h) * 0.5f);
			visual.CenterPoint({ w * 0.5f, h * 0.5f, 0.f });
			visual.Scale({ s, s, 1.f });
			visual.Offset({ ox, oy, 0.f });
			// 以中心为锚缩放后的可见矩形，回写成节点几何（命中测试/选区映射都用它）。
			x = ox + w * 0.5f - w * s * 0.5f;
			y = oy + h * 0.5f - h * s * 0.5f;
			w = w * s;
			h = h * s;
		}
	};

	// 量一段文本按指定宽度折行后的总高度，设备像素（折行设置与 TextBox 保持一致）。
	float measureTextHeight(const std::wstring& s, float fontSizeLogical, float maxWidthLogical, float dpi,
		bool bold = false)
	{
		auto* d2d = Ling::D2D::get();
		if (!d2d || !d2d->dwriteFactory || s.empty() || maxWidthLogical <= 0.f) return 0.f;
		auto fmt = d2d->getTextFormat(L"");
		if (!fmt) return 0.f;
		Microsoft::WRL::ComPtr<IDWriteTextLayout> tl;
		if (FAILED(d2d->dwriteFactory->CreateTextLayout(s.c_str(), (UINT32)s.size(),
			fmt, maxWidthLogical * dpi, FLT_MAX, tl.GetAddressOf())) || !tl)
			return 0.f;
		tl->SetFontSize(fontSizeLogical * dpi, { 0, INT_MAX });
		tl->SetFontWeight(bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL, { 0, INT_MAX });
		tl->SetWordWrapping(DWRITE_WORD_WRAPPING_EMERGENCY_BREAK);
		DWRITE_TEXT_METRICS m{};
		if (FAILED(tl->GetMetrics(&m))) return 0.f;
		return m.height;
	}

	// 不折行时文字的自然宽高（设备像素）：OCR 叠加层用它在建框前算字号，让文字按**字号**
	// 缩放到识别框，而不是绘完再拉伸位图（拉伸会糊，重排字号不会）。
	bool measureTextSize(const std::wstring& s, float fontSizeLogical, float dpi, bool bold,
		float& outW, float& outH)
	{
		auto* d2d = Ling::D2D::get();
		if (!d2d || !d2d->dwriteFactory || s.empty()) return false;
		auto fmt = d2d->getTextFormat(L"");
		if (!fmt) return false;
		Microsoft::WRL::ComPtr<IDWriteTextLayout> tl;
		if (FAILED(d2d->dwriteFactory->CreateTextLayout(s.c_str(), (UINT32)s.size(),
			fmt, FLT_MAX, FLT_MAX, tl.GetAddressOf())) || !tl)
			return false;
		tl->SetFontSize(fontSizeLogical * dpi, { 0, INT_MAX });
		tl->SetFontWeight(bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL, { 0, INT_MAX });
		tl->SetWordWrapping(DWRITE_WORD_WRAPPING_EMERGENCY_BREAK);
		DWRITE_TEXT_METRICS m{};
		if (FAILED(tl->GetMetrics(&m))) return false;
		outW = m.widthIncludingTrailingWhitespace;
		outH = m.height;
		return outW > 0.f && outH > 0.f;
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

	// 单行笔画宽度（image 像素）：框内按背景色二值化，再逐行取前景游程的 25 分位。
	// 取 25 分位是为了避开中文长横（长横的游程接近整字宽），留下竖笔/短笔的真实粗细。
	float estimateStrokeWidth(const std::vector<BYTE>& px, int W, int H, const OcrRegion& reg)
	{
		if (px.empty() || W <= 0 || H <= 0) return 0.f;
		const int x0 = std::clamp((int)std::floor(reg.x), 0, W - 1);
		const int y0 = std::clamp((int)std::floor(reg.y), 0, H - 1);
		const int x1 = std::clamp((int)std::ceil(reg.x + reg.w), x0 + 1, W);
		const int y1 = std::clamp((int)std::ceil(reg.y + reg.h), y0 + 1, H);
		if (x1 - x0 < 4 || y1 - y0 < 4) return 0.f;
		int br = 0, bg = 0, bb = 0;
		if (!regionBg(px, W, H, reg.x, reg.y, reg.w, reg.h, br, bg, bb)) return 0.f;

		std::vector<int> runs;
		runs.reserve((size_t)(y1 - y0) * 2);
		for (int y = y0; y < y1; ++y) {
			const BYTE* p = px.data() + ((size_t)y * W + x0) * 4;
			int run = 0;
			for (int x = x0; x < x1; ++x, p += 4) {
				const int dr = std::abs((int)p[2] - br);
				const int dg = std::abs((int)p[1] - bg);
				const int db = std::abs((int)p[0] - bb);
				const bool fg = (std::max)({ dr, dg, db }) > 48;
				if (fg) ++run;
				else if (run > 0) { runs.push_back(run); run = 0; }
			}
			if (run > 0) runs.push_back(run);
		}
		if (runs.size() < 8) return 0.f;
		std::sort(runs.begin(), runs.end());
		return (float)runs[runs.size() / 4];
	}

	// 大字号粗体估测：只对明显大于正文的框（>=24px × dpi）做，且必须比正文笔画中位数
	// 明显更粗（>=1.25 倍）。小字（1~2px 笔画）量化误差比粗体差异还大，一律不猜。
	// 默认 OCR 和翻译都会重绘文字，才需要判断；文本重排是整段一个框，不做逐行加粗。
	void estimateBoldRegions(const std::vector<BYTE>& px, int W, int H,
		std::vector<OcrRegion>& regions, float dpi)
	{
		if (px.empty() || W <= 0 || H <= 0 || regions.empty()) return;
		if ((size_t)W * (size_t)H * 4 > px.size()) return;
		const float large = 24.f * (dpi > 0.f ? dpi : 1.f);
		std::vector<float> ratio(regions.size(), 0.f);
		std::vector<float> body;
		for (size_t i = 0; i < regions.size(); ++i) {
			auto& reg = regions[i];
			reg.bold = false;
			if (reg.text.empty() || reg.h < 2.f) continue;
			const float sw = estimateStrokeWidth(px, W, H, reg);
			if (sw <= 0.f) continue;
			ratio[i] = sw / (std::max)(1.f, reg.h);
			if (reg.h < large) body.push_back(ratio[i]);
		}
		if (body.empty()) return;   // 没有正文基线：不猜
		std::sort(body.begin(), body.end());
		const float bodyMed = body[body.size() / 2];
		for (size_t i = 0; i < regions.size(); ++i) {
			auto& reg = regions[i];
			if (reg.text.empty() || reg.h < large || ratio[i] <= 0.f) continue;
			if (ratio[i] >= (std::max)(0.12f, bodyMed * 1.25f))
				reg.bold = true;
		}
	}


	// 逐框字号（单位：image 像素）。
	//   正文：取全局「中位框高」统一，再用墨迹高 × K 标定 —— 单个框的高度不可靠（带边距/行高拟合
	//   不准），逐框按各自框高算就会"一大一小"；而墨迹高跨引擎可比，能给出同一字号。
	//   标题（框高明显大于正文）保留自身字号，避免把大标题压成正文。
	[[maybe_unused]] std::vector<float> unifiedFontSizes(const std::vector<BYTE>& px, int W, int H,
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

ToolOcr::ToolOcr(Ling::WinBase* host, OcrResult result, bool translateMode,
	std::vector<BYTE> image, int imageW, int imageH)
	: Ling::WinBase(), host(host), result_(std::move(result)), translateMode_(translateMode),
	  imgW(std::max(1, imageW)), imgH(std::max(1, imageH)), srcImage_(std::move(image))
{
	dpi = host ? host->dpi : 1.f;
	sampleLayerPalette(srcImage_, imgW, imgH, result_.regions, palBg_, palFg_);
	// 每行是独立控件，跨行选中由本类统一接管（见 hitSelPoint/applySelection）：禁用各行
	// 自身的鼠标选区，避免"点到的位置"和"眼睛看到的文字"各算各的。
	onMouseDown.add([this](POINT pos, bool isRight) {
		if (isRight) return;
		// 贴图上方的宿主控件（锁定/编辑/关闭）：压在浮层最上面，点击要先交给它
		if (pinOverlay_ && chromeDown_ && chromeDown_(pos)) return;
		// 点在上方悬浮按钮上：交给按钮自己处理，不参与"双击复制结果"，
		// 否则双击「对比」会直接复制并退出截图，而不是切换原图浮窗。
		// 必须放在最前面：文本重排分支里也有双击判定，会把它盖掉。
		if (buttonsShown_ && ((copyBtn && copyBtn->isPosIn(pos)) || (compareBtn && compareBtn->isPosIn(pos))))
			return;
		// 文本重排：单击/拖选交给普通 TextBox；双击仍然是「完成」。
		if (reflow_) {
			if (!pinOverlay_ && onConfirm_) {
				const auto now = GetTickCount64();
				const bool dbl = (now - lastDownTick_ <= GetDoubleClickTime())
					&& std::abs(pos.x - lastDownPt_.x) <= GetSystemMetrics(SM_CXDOUBLECLK)
					&& std::abs(pos.y - lastDownPt_.y) <= GetSystemMetrics(SM_CYDOUBLECLK);
				lastDownTick_ = now;
				lastDownPt_ = pos;
				if (dbl) { onConfirm_(L""); return; }
			}
			return;
		}
		// 截图宿主：双击译文/识别文字 = 「完成」（复制整段并退出）。
		// 必须抢在选区逻辑之前返回 —— 否则第二下会被当成又一次落点，
		// 双击就变成了"选中一个字符/整块"，而不是完成。
		if (!pinOverlay_ && onConfirm_) {
			const auto now = GetTickCount64();
			const bool dbl = (now - lastDownTick_ <= GetDoubleClickTime())
				&& std::abs(pos.x - lastDownPt_.x) <= GetSystemMetrics(SM_CXDOUBLECLK)
				&& std::abs(pos.y - lastDownPt_.y) <= GetSystemMetrics(SM_CYDOUBLECLK);
			lastDownTick_ = now;
			lastDownPt_ = pos;
			if (dbl) { onConfirm_(L""); return; }
		}
		const SelPoint sp = hitSelPoint(pos);
		if (sp.line < 0) return;
		selDragging_ = true;
		selALine_ = selBLine_ = sp.line;
		selAIdx_ = selBIdx_ = sp.index;
		SetFocus(hwnd);
		SetCapture(hwnd);
		applySelection();
	});
	onMouseMove.add([this](POINT pos) {
		if (!selDragging_) return;
		const SelPoint sp = hitSelPoint(pos);
		if (sp.line < 0) return;
		selBLine_ = sp.line;
		selBIdx_ = sp.index;
		applySelection();
	});
	onMouseUp.add([this](POINT, bool) {
		if (!selDragging_) return;
		selDragging_ = false;
		ReleaseCapture();
		applySelection();
	});
	onKeyDown.add([this](UINT key) {
		const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
		// 文本重排且文本框已聚焦：编辑键、Ctrl+A/C/V 全归 TextBox，只把 Esc 透给宿主。
		// Ctrl+Z / Ctrl+Y 例外：TextBox 没有 undo，交给本类的编辑历史 —— 否则会被当成"撤销标注"。
		if (reflow_ && !editors.empty() && editors[0] && editors[0]->isFocused()) {
			if (ctrl && (key == 'Z' || key == 'Y')) {
				undoRedoReflow(key == 'Y');
				return;
			}
			if (key == VK_ESCAPE && this->host) this->host->onKeyDown(key);
			return;
		}
		if (ctrl && key == 'A') {   // 全选（跨行）
			if (!editors.empty()) {
				// 全选不是"正在拖选"：这里只设端点。之前把 selActive_ 也置真，
				// 于是全选后鼠标只是划过文字，onMouseMove 就把终点改到光标处。
				selDragging_ = false;
				selALine_ = 0; selAIdx_ = 0;
				selBLine_ = (int)editors.size() - 1;
				selBIdx_ = (int)editors.back()->getText().size();
				applySelection();
			}
			return;
		}
		if (ctrl && key == 'C') {
			// 贴图宿主：鼠标在贴图范围内才接管 —— 拖选过只复制选区，没拖选复制整段；
			// 鼠标不在贴图上时不拦 Ctrl+C，留给系统。
			if (pinOverlay_) {
				POINT pt{};
				GetCursorPos(&pt);
				const int r = x + (int)std::lround(w), b = y + (int)std::lround(h);
				if (pt.x < x || pt.x >= r || pt.y < y || pt.y >= b) return;
				copyAll();
				return;
			}
			// 截图宿主：拖选后 Ctrl+C = 复制选中片段并退出截图（等同「完成」，但只复制这段）。
			// 没有选区时保持原行为：复制整段、留在原地面板里。
			const std::wstring sel = onConfirm_ ? selectedText() : std::wstring{};
			if (!sel.empty()) { onConfirm_(sel); return; }
			copyAll();
			return;
		}
		if (ctrl && key == 'X') return;   // 只读展示，剪切忽略
		if (this->host) this->host->onKeyDown(key);
	});
	// 拖选中保持 I 形光标：此时鼠标可能已经离开某些行，各行的 hover 态不生效。
	onCursor.add([this](bool* handled) {
		// 拖选/全选后都保持 I 形光标；全选没有拖选状态，按端点是否有效判断。
		if (selDragging_ || (selALine_ >= 0 && selBLine_ >= 0)) {
			SetCursor(LoadCursor(nullptr, IDC_IBEAM));
			*handled = true;
		}
	});
	// 翻译：译文折行后整段可能超出选区，滚轮上下滚动（不画滚动条）。
	// 贴图宿主不接管 —— 滚轮要留给贴图缩放。
	onMouseWheel.add([this](POINT, float space) {
		if (!translateMode_ || pinOverlay_ || trMaxScroll_ <= 0.f) return;
		trScrollY_ = std::clamp(trScrollY_ - space, 0.f, trMaxScroll_);
		for (size_t i = 0; i < trBoxes_.size() && i < trTops_.size(); ++i)
			trBoxes_[i]->setPosition(Ling::Edge::Top, (trTops_[i] - trScrollY_) / dpi);
		refresh();
	});
	// 跨屏缩放不同：DPI 变了按选区物理尺寸重贴并重排。窗口自己管几何（见 keepGeometryOnDpiChange），
	// 系统不会再套「建议矩形」把它改小/改偏。
	onDpiChanged.add([this]() {
		if (!hasMask_) return;
		applyMaskGeometry();
		rebuildEditors();
		refresh();
	});
	// 右上角按钮的显示/隐藏靠轮询：面板除文字块外全穿透，空白处的悬停事件收不到
	onTimer.add([this](UINT id) {
		if (id == kHoverTimerId) updateButtonsHover();
		if (id == kTypingTimerId) {
			// 点动画：0 → 1 → 2 → 3 循环，让提示条一直"在动"，不打字
			busyDots_ = (busyDots_ + 1) % 4;
			if (bannerLabel) bannerLabel->setText(busyBase_ + std::wstring((size_t)busyDots_, L'.'));
			repaint();
		}
	});
}

ToolOcr::~ToolOcr()
{
	// WinBase 的析构不销毁 HWND：不先 close 就 reset，窗口会变成"幽灵窗"，
	// 其 WndProc 仍指向已释放的对象 —— 鼠标划过即重入已释放内存，反复开关几次就闪退。
	if (hwnd) killTimer(kHoverTimerId);
	if (hwnd) { close(); hwnd = nullptr; }
}

void ToolOcr::onCreated()
{
	body->setBg(0);   // 透明面板：只盖住识别到的文字块，其余区域直接露出原图
	body->setBorderRadius(0.f);

	if (!pinOverlay_) {
		// 与工具栏主题联动：底色 / 悬停底 / 描边 / 图标色都取 ToolbarTheme
		ToolbarTheme::refresh();
		// 透明度对齐贴图右上角悬浮按钮：常态 45%、悬停 60%（色相仍跟主题走）
		const uint32_t btnBg = (ToolbarTheme::background & 0xFFFFFF00u) | 0x73u;
		const uint32_t btnHoverBg = (ToolbarTheme::hoverBg & 0xFFFFFF00u) | 0x99u;
		auto styleTopBtn = [btnBg, btnHoverBg](Ling::Button* b) {
			b->setSize(34.f, 34.f);
			b->setPositionType(Ling::Position::Absolute);
			b->setPosition(Ling::Edge::Top, 10.f);
			b->setBg(btnBg);
			b->setHoverBg(btnHoverBg);
			b->setBorder(ToolbarTheme::borderWidth, ToolbarTheme::border);
			b->setBorderRadius(ToolbarTheme::hoverRadius);
			b->setAlignItems(Ling::Align::Center);
			b->setJustifyContent(Ling::Justify::Center);
			b->setFontFamily(Icon::Family);
			b->setFontSize(18.f);
			b->setColor(ToolbarTheme::iconNormal);
			b->setHoverColor(ToolbarTheme::iconNormal);
			};

		copyBtn = body->makeChild<Ling::Button>();
		copyBtn->setText(Icon::Copy);
		copyBtn->setPosition(Ling::Edge::Right, 10.f);
		styleTopBtn(copyBtn);
		copyBtn->onClick.add([this](Ling::Button*) {
			// 截图宿主：右上角复制 = 「完成」（复制整段文字结果并退出截图）；
			// 贴图宿主没有 onConfirm，维持原来的"只复制、不关窗"。
			if (!pinOverlay_ && onConfirm_) { onConfirm_(L""); return; }
			copyAll();
		});

		// 「对比」：与复制同款，排在它左边
		compareBtn = body->makeChild<Ling::Button>();
		compareBtn->setText(Icon::Compare);
		compareBtn->setPosition(Ling::Edge::Right, 52.f);
		styleTopBtn(compareBtn);
		compareBtn->onClick.add([this](Ling::Button*) { toggleCompare(); });

		// 两个按钮平时隐藏：鼠标悬停到选区范围内才出现
		copyBtn->hide();
		compareBtn->hide();

		// 悬停提示（与其他工具条按钮一致：停悬 1 秒后弹出）
		tip = std::make_unique<Tip>(this);
		tip->bind(copyBtn, Lang::get(L"ocr.copy"));
		tip->bind(compareBtn, Lang::get(L"ocr.compare"));
	}
	// 两种宿主都用同一个轮询：贴图模式在浮层里收不到贴图的鼠标事件，
	// 靠它把光标位置转给宿主更新控件悬停态（见 updateButtonsHover）。
	setTimer(kHoverPollMs, kHoverTimerId);

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
	// 标注镜像层：盖在识别文字之上的透明画布（宿主注入 annotPainter_ 后才有内容）
	annotCanvas_ = body->makeChild<Ling::Canvas>();
	annotCanvas_->enableSwapChain();
	annotCanvas_->setSizePercent(100.f, 100.f);
	// 提示条（"翻译失败：…"之类）必须压在最上面：它建得比识别结果早，不显式提到最前
	// 就会被后建的背景块/文字块盖住。
	if (bannerNode) body->bringToFront(bannerNode);
	// 右上角按钮压在文字块与标注镜像层之上（先建的后移，所以放在最后提）
	if (compareBtn) body->bringToFront(compareBtn);
	if (copyBtn) body->bringToFront(copyBtn);
	show();
}

void ToolOcr::updateButtonsHover()
{
	if (pinOverlay_) {
		// 贴图模式：把光标（换算成宿主客户区坐标）交给宿主，由宿主决定控件显隐与高亮
		if (!hwnd || !chromeHover_) return;
		POINT pt{};
		GetCursorPos(&pt);
		ScreenToClient(hwnd, &pt);
		chromeHover_(pt);
		return;
	}
	if (!hwnd || !copyBtn) return;
	POINT pt{};
	GetCursorPos(&pt);
	// 面板窗口 == 选区矩形：光标落在窗口内就算"悬停在框选范围内"
	const bool in = pt.x >= x && pt.x < x + (int)w && pt.y >= y && pt.y < y + (int)h;
	if (in == buttonsShown_) return;
	buttonsShown_ = in;
	if (in) {
		copyBtn->show();
		if (compareBtn) compareBtn->show();
	}
	else {
		if (tip) tip->hide();
		copyBtn->hide();
		if (compareBtn) compareBtn->hide();
	}
}

void ToolOcr::toggleCompare()
{
	if (compare_) { compare_.reset(); refreshCompareBtnState(); return; }
	if (!hwnd || srcImage_.empty()) return;
	compare_ = std::make_unique<ToolOcrCompare>(srcImage_, imgW, imgH, dpi);
	compare_->createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, WS_POPUP);
	if (compare_->hwnd)
		SetWindowLongPtr(compare_->hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(hwnd));
	compare_->placeNear(RECT{ x, y, x + (int)w, y + (int)h });
	compare_->show();
	refreshCompareBtnState();
}

void ToolOcr::refreshCompareBtnState()
{
	if (!compareBtn) return;
	// 选中态与工具栏开关按钮一致：图标用主题强调色
	const uint32_t color = compare_ ? ToolbarTheme::selection : ToolbarTheme::iconNormal;
	compareBtn->setColor(color);
	compareBtn->setHoverColor(color);
}

// 把一段译文按"原文各行宽度"逐行塞满：方块字逐字、西文按词，塞不下（行数不够）返回 false。
// 成功时 out 的每一行正好对应原文的一行，可以直接贴回原位。
bool fitTextToSlots(const std::wstring& text, float fontLogical, float dpi, bool bold,
	const std::vector<float>& slotWidths, std::vector<std::wstring>& out)
{
	out.clear();
	if (slotWidths.empty() || text.empty()) return false;
	size_t pos = 0;
	for (size_t k = 0; k < slotWidths.size() && pos < text.size(); ++k) {
		const float lim = std::max(8.f, slotWidths[k]);
		std::wstring line;
		while (pos < text.size()) {
			// 取一个排版单元：方块字一个字；西文一个词（连同后面的空格）
			size_t next = pos + 1;
			if (!isCjkChar(text[pos])) {
				while (next < text.size() && !isCjkChar(text[next]) && text[next] != L' ') ++next;
				while (next < text.size() && text[next] == L' ') ++next;
			}
			const std::wstring cand = text.substr(pos, next - pos);
			float w = 0.f, h = 0.f;
			if (!measureTextSize(line + cand, fontLogical, dpi, bold, w, h)) { out.clear(); return false; }
			if (!line.empty() && w > lim) break;
			if (line.empty() && w > lim) {
				// 单元本身比整行还宽：退回按字硬断
				std::wstring cut;
				for (size_t t = pos; t < next; ++t) {
					float cw = 0.f, ch = 0.f;
					if (!measureTextSize(cut + text[t], fontLogical, dpi, bold, cw, ch)) break;
					if (cw > lim && !cut.empty()) break;
					cut += text[t];
				}
				if (cut.empty()) return false;
				line = cut;
				pos += cut.size();
				break;
			}
			line += cand;
			pos = next;
		}
		out.push_back(line);
	}
	if (pos < text.size()) { out.clear(); return false; }   // 行数不够，交给整段排版
	return !out.empty();
}

void ToolOcr::rebuildEditors()
{
	builtDpi_ = dpi;   // 这一版编辑框是用当前 dpi 排的（换屏后 syncToMask 据此判断要不要重排）
	for (auto* e : editors) {
		if (e) body->removeChild(e);
	}
	editors.clear();
	for (auto* n : bgs_) {
		if (n) body->removeChild(n);
	}
	bgs_.clear();
	trBoxes_.clear();
	trTops_.clear();
	trScrollY_ = 0.f;
	trMaxScroll_ = 0.f;

	if (!result_.ok) {
		// 正在翻译时依然顶着忙碌提示（识别失败也不能把"正在翻译…"换成红条）
		showBanner(!busyMsg_.empty() ? busyMsg_
			: (result_.error.empty() ? Lang::get(L"ocr.fail") : result_.error), busyMsg_.empty());
		if (bannerNode) body->bringToFront(bannerNode);
		if (compareBtn) body->bringToFront(compareBtn);
		if (copyBtn) body->bringToFront(copyBtn);
		return;
	}

	// 文本重排只作用于 OCR 结果；翻译仍要保留"译文对齐原文位置"的语义。
	reflow_ = !translateMode_ && Setting::get()->getOcrResultMode() == 1;
	// OCR 结果要重绘识别文字：否则用户看不到识别结果，没法核对对错。
	// 字号按识别框适配（见下面的 measureTextSize），不再用 visual.Scale 拉伸位图。
	// 加粗只对大字标题估，小字不猜；文本重排是整段一个框，不做逐行加粗。
	if (!reflow_) estimateBoldRegions(srcImage_, imgW, imgH, result_.regions, dpi);

	const float scaleX = (imgW > 0) ? (w / (float)imgW) : 1.f;
	const float scaleY = (imgH > 0) ? (h / (float)imgH) : 1.f;

	// 正文统一字号：同一段正文里各框松紧不一（PP-OCR 的框带边距、混排时还会更高），逐框按自身高度
	// 算就会"一行大一行小"。做法：取偏低分位当**正文基准**（大标题会把均值/中位抬高，偏低分位对
	// 少数大框不敏感），正文行一律用正文框高的**平均值**定字号；明显更大的行（标题）保留自身高度。
	std::vector<float> hEff(result_.regions.size(), 0.f);
	// 译文定字号用的「墨迹高」（真实字身高度，物理像素）。
	std::vector<float> inkEff(result_.regions.size(), 0.f);
	{
		const size_t n = result_.regions.size();
		// 一律按墨迹高度定字号，不再用检测框高度：表格单元的框含内边距，框高参差不齐，
		// 用框高会把普通单元格误判成"标题"放大。
		std::vector<float> ik(n, 0.f);
		for (size_t i = 0; i < n; i++) {
			const auto& r = result_.regions[i];
			ik[i] = estimateInkHeight(srcImage_, imgW, imgH, r.x, r.y, r.w, r.h) * scaleY;
			if (ik[i] < 3.f) ik[i] = std::max(1.f, r.h * scaleY) * 0.72f;
		}
		std::vector<float> sorted = ik;
		std::sort(sorted.begin(), sorted.end());
		const float bodyRef = sorted.empty() ? 0.f : sorted[sorted.size() / 2];
		const float headingThresh = bodyRef * 1.45f;
		float bodySum = 0.f;
		int bodyN = 0;
		for (size_t i = 0; i < n; i++) {
			if (ik[i] < headingThresh) { bodySum += ik[i]; ++bodyN; }
		}
		float bodyInk = (bodyN > 0) ? bodySum / (float)bodyN : bodyRef;
		// 可读性下限：正文换算成字号后如果太小，按方块字 13px / 西文 12px 兜底。
		// 例外：整块**确实全是小字**（八成以上的正文行都在下限以下）时就按实际来 ——
		// 那种截图（整页密集小字）强行放大会全部溢出重叠。
		{
			const bool hasCjk = [&] {
				for (const auto& r : result_.regions)
					for (wchar_t c : r.text)
						if (c >= 0x3400 && c <= 0x9FFF) return true;
				return false;
			}();
			const float minReadPx = hasCjk ? 13.f : 12.f;
			const float bodyPx = bodyInk / (kCjkInkRatio * dpi);
			if (bodyPx > 0.f && bodyPx < minReadPx && bodyN > 0) {
				int tiny = 0;
				for (size_t i = 0; i < n; i++) {
					if (ik[i] >= headingThresh) continue;
					if (ik[i] / (kCjkInkRatio * dpi) < minReadPx) ++tiny;
				}
				// 不到八成的小字 → 说明是"个别行偏小"，抬到可读下限
				if (tiny * 100 < bodyN * 80) bodyInk = minReadPx * kCjkInkRatio * dpi;
			}
		}
		for (size_t i = 0; i < n; i++) {
			hEff[i] = (ik[i] < headingThresh) ? bodyInk : ik[i];
			inkEff[i] = hEff[i];
		}
		// 表格里的"高个子"不是标题：标题独占一行，表格一行里有好几个格子。
		// 带图标的格子墨迹会被图标撑高，不压回正文就会比同排其他格子大一圈。
		for (size_t i = 0; i < n; i++) {
			if (ik[i] < headingThresh) continue;
			const auto& a = result_.regions[i];
			for (size_t j = 0; j < n; j++) {
				if (j == i) continue;
				const auto& b = result_.regions[j];
				const float hh = std::max(1.f, std::min(a.h, b.h));
				if (std::fabs(b.y - a.y) <= hh * 0.5f) {
					hEff[i] = bodyInk;
					inkEff[i] = bodyInk;
					break;
				}
			}
		}
	}
	// 原文对齐检测：译文比原文短、装不满框时才有观感差别 —— 居中的标题要跟着居中、
	// 右对齐的日期/页码要跟着靠右。判据基于"**行与行之间的共识**"（只看单行的左右留白会被
	// 整段缩进 / 选区没框正带偏）：
	//   ① 整块是否居中：比较左边界/中线/右边界各自"最多有几行共用"，且左右两侧都明显内缩、
	//      内缩量接近 —— 三条都满足才整块居中。
	//   ② 否则逐行：贴"多行共用的左边界"→居左；贴多行共用的右边界、或顶到整块最右且明显短→居右；
	//      **其余一律居左**（不再逐行猜居中：那样一段左对齐的列表很容易被误判成居中）。
	std::vector<int> aligns(result_.regions.size(), 0);
	float blockLeftPx = 0.f;   // 整块的左边界（译文行首齐头用，见下面的翻译排版）
	{
		const size_t n = result_.regions.size();
		if (n > 0) {
			std::vector<float> ls(n), rs(n), cs(n);
			float minL = 1e9f, maxR = -1e9f;
			for (size_t i = 0; i < n; i++) {
				const auto& reg = result_.regions[i];
				ls[i] = reg.x * scaleX;
				rs[i] = (reg.x + reg.w) * scaleX;
				cs[i] = (ls[i] + rs[i]) * 0.5f;
				minL = std::min(minL, ls[i]);
				maxR = std::max(maxR, rs[i]);
			}
			const float blockW = std::max(1.f, maxR - minL);
			const float tol = std::max(2.f, blockW * 0.03f);      // 聚簇容差
			const float tolEdge = std::max(3.f, blockW * 0.05f);  // "同一列"的容差（OCR 框本就有几像素抖动）

			// 聚簇：返回每行所属簇 id；sizes 记录各簇大小（= 有多少行共用这条边）
			auto cluster = [tol](const std::vector<float>& v, std::vector<int>& sizes) {
				std::vector<int> id(v.size(), -1);
				sizes.clear();
				for (size_t i = 0; i < v.size(); i++) {
					if (id[i] >= 0) continue;
					const int cid = (int)sizes.size();
					int cnt = 0;
					for (size_t j = i; j < v.size(); j++) {
						if (id[j] < 0 && v[j] >= v[i] - tol && v[j] <= v[i] + tol) { id[j] = cid; ++cnt; }
					}
					sizes.push_back(cnt);
				}
				return id;
			};
			std::vector<int> szL, szR, szC;
			const auto idL = cluster(ls, szL);
			const auto idR = cluster(rs, szR);
			cluster(cs, szC);   // 中线簇只用于"整块居中"判定
			int bestL = 0, bestR = 0, bestC = 0;
			for (int s : szL) bestL = std::max(bestL, s);
			for (int s : szR) bestR = std::max(bestR, s);
			for (int s : szC) bestC = std::max(bestC, s);
			// ① 整块居中：中线共用的行数严格多于左右、至少两行，**再**加一道几何闸 ——
			//    左右两侧都要明显内缩、且内缩量接近。只靠中线聚簇不够：每行宽度差不多、
			//    左边界又被 OCR 抖成两个簇时，中线会挤成一簇，一整段左对齐的列表就被判成居中，
			//    译文每行都跟着缩进，看着就是"没对齐"（用户反馈的正是这个）。
			std::vector<float> lin(n), rin(n);
			for (size_t i = 0; i < n; i++) { lin[i] = ls[i] - minL; rin[i] = maxR - rs[i]; }
			std::sort(lin.begin(), lin.end());
			std::sort(rin.begin(), rin.end());
			const float medL = lin[n / 2], medR = rin[n / 2];
			const float insetMin = std::max(3.f, blockW * 0.06f);
			const bool symmetric = std::fabs(medL - medR) <= std::max(blockW * 0.10f, 6.f);
			const bool blockCentered = (bestC >= 2 && bestC > bestL && bestC > bestR)
				&& medL > insetMin && medR > insetMin && symmetric;
			// 整块左边界取**偏左分位**：多数行贴在这里；个别行因为圆点没被并进文字会偏右
			// 一两个字符宽，取中位数会被这种行带偏（一半行偏右时基准也跟着偏右）。
			{
				std::vector<float> lsorted = ls;
				std::sort(lsorted.begin(), lsorted.end());
				blockLeftPx = lsorted[n / 4];
			}

			for (size_t i = 0; i < n; i++) {
				if (blockCentered) { aligns[i] = 1; continue; }
				const float lineW = rs[i] - ls[i];
				const bool multiLeft = idL[i] >= 0 && szL[idL[i]] >= 2;
				const bool multiRight = idR[i] >= 0 && szR[idR[i]] >= 2;
				if (multiLeft) {
					aligns[i] = 0;                                   // 贴着多行共用的左边界
				}
				else if (multiRight || (maxR - rs[i] <= tolEdge && lineW < blockW * 0.85f)) {
					aligns[i] = 2;                                   // 多行共用右边界 / 顶右且不是满行
				}
				else {
					aligns[i] = 0;                                   // 其余一律居左，不再逐行猜居中
				}
			}
		}
	}

	// 翻译：译文按**段**排版 —— 同一段的折行（OCR 的视觉行、续行还带缩进）先并成一个框，
	// 再按原段宽折行、字号取原文行高。整段超出选区时滚轮上下滚动（不画滚动条）。
	if (translateMode_ && !result_.regions.empty()) {
		// 译文不再原位覆盖原文，行被推开后空档会露出原字 —— 整块用采样底色盖死
		body->setBg(palBg_);
		float cursorY = 0.f;   // 物理像素：下一段允许的最上端
		const size_t n = result_.regions.size();
		// 全块字身中位：判断"这段是不是大字标题"用
		float bodyInkAll = 0.f;
		{
			std::vector<float> vs;
			for (float v : inkEff) if (v >= 3.f) vs.push_back(v);
			std::sort(vs.begin(), vs.end());
			if (!vs.empty()) bodyInkAll = vs[vs.size() / 2];
		}
		std::vector<size_t> paras;   // 每段的段首 region 下标
		for (size_t k = 0; k < n; ++k)
			if (k == 0 || result_.regions[k].para != result_.regions[k - 1].para) paras.push_back(k);
		// 整块是不是"圆点列表"：过半左对齐段落带圆点。是的话，中段被 OCR 漏掉圆点的条目
		// 也按条目补一个 —— 否则它比上下条目左移一个"• "的宽度，看着就是"中间不齐"。
		int bulletParas = 0, leftParas = 0;
		for (size_t k = 0; k < paras.size(); ++k) {
			if (aligns[paras[k]] != 0) continue;
			++leftParas;
			if (result_.regions[paras[k]].bullet) ++bulletParas;
		}
		const bool isBulletList = (bulletParas > 0 && bulletParas * 2 >= leftParas);
		for (size_t p = 0; p < paras.size(); ++p) {
			// 本段范围 [i, j)：同 para 的行算一段（段落归并在 ocrResultMergedToLines 里做）
			const size_t i = paras[p];
			const size_t j = (p + 1 < paras.size()) ? paras[p + 1] : n;
			const auto& reg = result_.regions[i];
			const bool betweenBullets = p > 0 && p + 1 < paras.size()
				&& result_.regions[paras[p - 1]].bullet && result_.regions[paras[p + 1]].bullet;
			const bool bullet = reg.bullet || (isBulletList && betweenBullets);
			std::wstring text = reg.displayText;
			if (text.empty()) {
				// 没有译文（例如目标语言与原文相同）：退回原文，按行拼起来（原文自带圆点）
				for (size_t k = i; k < j; ++k) {
					if (!text.empty()) text += L' ';
					text += result_.regions[k].text;
				}
			}
			else if (bullet) {
				// 圆点自己补：引擎对"•"保留/丢弃不一致，补上后所有条目左端才在同一列
				stripLeadingBullet(text);
				text = std::wstring(L"\u2022  ") + text;   // 圆点后留两个空格：和正文拉开一点
			}
			if (text.empty()) continue;
			const float py = reg.y * scaleY;
			const float ph = (hEff[i] > 0.f) ? hEff[i] : std::max(1.f, reg.h * scaleY);
			float rightMost = (reg.x + reg.w) * scaleX;   // 段内最右（段宽按它算）
			for (size_t k = i + 1; k < j; ++k) {
				const auto& rk = result_.regions[k];
				rightMost = std::max(rightMost, (rk.x + rk.w) * scaleX);
			}
			const bool wide = hasWideGlyph(text);
			const bool srcWide = hasWideGlyph(reg.text);
			// 字号完全沿用 OCR 原位的口径：先按行框定一个字号，再像 OcrText 那样按真实排版尺寸缩进框里。
			// 这样翻译出来的大小和 OCR 结果一一对应（标题大、正文小），只是换了语言。
			const float srcBoxH = (hEff[i] > 0.f) ? hEff[i] : std::max(1.f, reg.h * scaleY);
			const float srcBoxW = std::max(1.f, reg.w * scaleX);
			const float srcInkRatio = srcWide ? kCjkInkRatio : kYaheiInkRatio;
			const float srcVFill = srcWide ? kCjkVFill : 1.f;
			float f = std::clamp(srcBoxH * srcVFill / (srcInkRatio * dpi), 7.f / dpi, 300.f / dpi);
			{
				float nw = 0.f, nh = 0.f;
				if (measureTextSize(reg.text, f, dpi, reg.bold, nw, nh)) {
					const float inkH = f * dpi * srcInkRatio;
					const float s = std::min(nw > 0.f ? srcBoxW / nw : 1.f,
						inkH > 0.f ? srcBoxH * srcVFill / inkH : 1.f);
					f *= std::clamp(s, 0.5f, 1.f);
				}
			}
			// 目标语言的字形大小补偿：英→中放一点、中→英收一点
			const float scriptAdj = (wide && !srcWide) ? 1.0f : (!wide && srcWide) ? 0.95f : 1.f;
			const float fLogical = std::max(7.f / dpi, f * scriptAdj);
			// 方案一：把译文按原文各行的槽位贴回去 —— 每行贴在自己的原位置、原宽度里，
			// 行数够且每行都塞得下才成功；否则落到下面的整段排版。
			if (aligns[i] == 0 && j > i) {
				std::vector<float> slotW;
				slotW.reserve(j - i);
				for (size_t k = i; k < j; ++k) {
				// 元素查找到的选区：宽度按**元素外框**满排（右边到面板右边界），
				// 译文正好铺满元素，和原布局严丝合缝；手拖的选区仍按每行框宽。
				const float lineW = elementBlock_
					? std::max(8.f, w * dpi - result_.regions[k].x * scaleX - 4.f * dpi)
					: std::max(8.f, result_.regions[k].w * scaleX);
				slotW.push_back(lineW);
				}
				std::vector<std::wstring> slotLines;
				// 字号不再为塞进槽位而缩小：排不下就退回整段排版，字号保持一致
				const float slotFont = fitTextToSlots(text, fLogical, dpi, reg.bold, slotW, slotLines) ? fLogical : 0.f;
				if (slotFont > 0.f) {
					const float lineH = slotFont * dpi * 1.2f;
					// 行距沿原文的行距；译文行数比原文少时**往上收**，不留空洞
					// （原文那一行由面板底色盖住，不会露出残影）。
					float pitch = lineH;
					if (j - i > 1) {
						const float y0 = result_.regions[i].y * scaleY;
						const float y1 = result_.regions[j - 1].y * scaleY;
						pitch = std::max(lineH, (y1 - y0) / (float)(j - i - 1));
					}
					// 段落起点：原文位置和"紧接上一段"取更靠上的那个 —— 译文比原文短时
					// 后面不会空出一大块；比原文长时也不会压到上一段。
					const float want = result_.regions[i].y * scaleY;
					float paraGap = lineH * 0.45f;
					if (i > 0) {
						const auto& prev = result_.regions[i - 1];
						const float g = std::max(0.f, want - (prev.y + prev.h) * scaleY);
						paraGap = std::clamp(g, lineH * 0.25f, lineH * 0.8f);
					}
					// 段落起点：只有"接在上一段下面"（同一列往下走）才做上下压缩；
					// 并排的表格单元 want 和上一段几乎相同，必须保持原位 —— 否则每格都被
					// "紧接上一段"往下推一格，整行表格就挤成一团。
					float top0 = want;
					if (cursorY > 1.f && want >= cursorY - lineH * 0.6f) {
						top0 = std::min(want, cursorY + paraGap);
					}
					for (size_t k = i; k < j && (k - i) < slotLines.size(); ++k) {
						const auto& rk = result_.regions[k];
						const float sx = rk.x * scaleX;
						const float sy = top0 + (float)(k - i) * pitch;
						auto* box = body->makeChild<Ling::TextBox>();
						box->setPositionType(Ling::Position::Absolute);
						box->setPosition(Ling::Edge::Left, sx / dpi);
						box->setPosition(Ling::Edge::Top, (sy + (std::max(rk.h * scaleY, lineH) - lineH) * 0.5f) / dpi);
						box->setSize(std::max(slotW[k - i], 1.f) / dpi, lineH / dpi);
						box->setColor(palFg_);
						box->setPadding(0.f);
						box->setFontSize(slotFont);
						if (reg.bold) box->setBold(true);
						box->setScrollBarVisible(false);
						box->setSelectionBgColor(0x1890FFFF);
						box->setSelectionTextColor(0xFFFFFFFF);
						box->setMouseSelectionEnabled(false);
						box->setText(slotLines[k - i]);
						editors.push_back(box);
						trBoxes_.push_back(box);
						trTops_.push_back(sy);
					}
					cursorY = top0 + (float)(slotLines.size() - 1) * pitch + lineH;
					continue;
				}
			}
			// 段首对齐：整段用**第一行**的起点（续行的缩进是同一段的折行，不能再带出来）；
			// 圆点没被并进文字的行会偏出一两个字符宽，按整块左边界吸一下。
			float px = reg.x * scaleX;
			if (aligns[i] == 0 && px > blockLeftPx && px - blockLeftPx <= 3.0f * fLogical * dpi)
				px = blockLeftPx;
			const float pw = std::max(1.f, rightMost - px);
			// 折行宽度：右边有空间就伸到选区右边界，只被同一段高度上的右侧文本挡住
			// （左右分栏时才保留原列宽，免得跟邻栏打架）；居中/右对齐保持原段宽，避免对齐跑偏。
			float rightLimit = w;
			if (aligns[i] == 0 && !elementBlock_) {
				const float iy0 = reg.y * scaleY;
				const auto& last = result_.regions[j - 1];
				const float iy1 = (last.y + last.h) * scaleY;
				for (size_t k = 0; k < n; ++k) {
					if (k >= i && k < j) continue;
					const auto& o = result_.regions[k];
					const float ox = o.x * scaleX;
					if (ox <= px + 0.5f) continue;   // 只受右侧文本限制
					const float oy0 = o.y * scaleY, oy1 = (o.y + o.h) * scaleY;
					if (std::min(oy1, iy1) - std::max(oy0, iy0) <= 0.f) continue;
					rightLimit = std::min(rightLimit, ox);
				}
			}
			else {
				rightLimit = px + pw;
			}
			const float boxW = std::max(pw, std::min(rightLimit, w) - px);
			// 右/居中段落：原框可能比译文窄（一行日期被折成两行）。按译文实际宽度朝外侧撑开，
			// 右对齐的右边界、居中的中线保持不动。
			float boxX = px;
			float boxWidth = boxW;
			if (aligns[i] != 0) {
				float nw = 0.f, nh = 0.f;
				if (measureTextSize(text, fLogical, dpi, reg.bold, nw, nh) && nw + 2.f > boxW) {
					const float extra = nw + 2.f - boxW;
					boxWidth = nw + 2.f;
					boxX = (aligns[i] == 2) ? px - extra : px - extra * 0.5f;
					if (boxX < 0.f) { boxWidth += boxX; boxX = 0.f; }
				}
			}
			const float measured = measureTextHeight(text, fLogical, boxWidth / dpi, dpi, reg.bold);
			const float boxH = std::max(1.f, measured > 0.f ? measured + 1.f : ph);
			// 段与段之间的空隙：英文比中文长，一段常折成多行，把下一段顶下来后两段就贴死了。
			// 被顶下来时按原文行距补一点空隙（至少 0.3 行、最多 0.8 行）。
			float origGap = 0.f;
			if (i > 0) {
				const auto& prev = result_.regions[i - 1];
				origGap = std::max(0.f, py - (prev.y + prev.h) * scaleY);
			}
			const float lineH = fLogical * dpi;
			const float paraGap = std::clamp(origGap, lineH * 0.30f, lineH * 0.80f);
			const float top = (cursorY > py) ? cursorY + paraGap : py;
			auto* box = body->makeChild<Ling::TextBox>();
			box->setPositionType(Ling::Position::Absolute);
			box->setPosition(Ling::Edge::Left, boxX / dpi);
			box->setPosition(Ling::Edge::Top, top / dpi);
			box->setSize(boxWidth / dpi, boxH / dpi);
			box->setColor(palFg_);
			box->setPadding(0.f);
			box->setFontSize(fLogical);
			// 加粗只给整段都是大字的段落：OCR 把标题和正文并成一段时，不能因为首行是标题
			// 就把整段都加粗（否则正文看着比标题还重）
			bool paraBold = false;
			if (reg.bold) {
				if (j - i == 1) paraBold = true;
				else {
					std::vector<float> pis;
					for (size_t t = i; t < j; ++t) if (inkEff[t] >= 3.f) pis.push_back(inkEff[t]);
					std::sort(pis.begin(), pis.end());
					const float pi = pis.empty() ? 0.f : pis[pis.size() / 2];
					paraBold = pi > 3.f && bodyInkAll > 3.f && pi > bodyInkAll * 1.2f;
				}
			}
			if (paraBold) box->setBold(true);
			box->setScrollBarVisible(false);   // 单个框不滚（高度已按内容给足），滚动只发生在整层
			box->setTextAlignCenter(aligns[i] == 1);
			box->setSelectionBgColor(0x1890FFFF);
			box->setSelectionTextColor(0xFFFFFFFF);
			box->setMouseSelectionEnabled(false);   // 选区由 ToolOcr 统一接管
			box->setText(text);
			editors.push_back(box);
			trBoxes_.push_back(box);
			trTops_.push_back(top);
			cursorY = top + boxH;
		}
		trMaxScroll_ = std::max(0.f, cursorY - h);
	}

	// 文本重排：整段落进一个可编辑文本框（见下面的智能排版分支）
	const size_t regionCount = (reflow_ || translateMode_) ? 0 : result_.regions.size();
	for (size_t i = 0; i < regionCount; ++i) {
		auto& reg = result_.regions[i];
		const float rx = reg.x * scaleX / dpi;
		const float ry = reg.y * scaleY / dpi;
		const float rw = std::max(1.f, reg.w * scaleX) / dpi;
		const float rh = std::max(1.f, reg.h * scaleY) / dpi;

		// 该框自己的底色（按框外圈中位色采样）+ 按底色亮度反相的文字色。
		// 逐框取色而不是整层一个色 —— 底色有渐变/多块时才能"贴"住原来那块背景。
		uint32_t bg = palBg_, fg = palFg_;
		int r = 0, g = 0, b = 0;
		if (regionBg(srcImage_, imgW, imgH, reg.x, reg.y, reg.w, reg.h, r, g, b)) {
			bg = ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | 0xFFu;
			const int lum = (r * 299 + g * 587 + b * 114) / 1000;
			fg = (lum > 150) ? 0x14181CFF : 0xF8FBFBFF;
		}

		// 1) 背景块：盖住识别框，外扩 1 个物理像素 —— 识别框通常不含文字抗锯齿边缘，
		//    严格贴框时边缘会漏出一点原文字。
		{
			const float bgPad = 1.f / dpi;
			auto* bgNode = body->makeChild<Ling::Node>();
			bgNode->setPositionType(Ling::Position::Absolute);
			bgNode->setPosition(Ling::Edge::Left, rx - bgPad);
			bgNode->setPosition(Ling::Edge::Top, ry - bgPad);
			bgNode->setSize(rw + bgPad * 2.f, rh + bgPad * 2.f);
			bgNode->setBg(bg);
			bgNode->setBorderRadius(2.f);
			bgs_.push_back(bgNode);
		}

		// 2) 文字：字号按"字形高度 = 识别框高"取，再交给 OcrText 在 layout() 里
		//    按**真实排版尺寸**缩放并居中到识别框（字形与框上下齐平、左右拉满）。
		//    墨迹比按脚本分：方块字占 em 的比例比西文大，用同一个比会把中文算大、与上下行叠在一起。
		std::wstring text = reg.displayText.empty() ? reg.text : reg.displayText;
		// 圆点统一成「• 」：引擎有时给 ·（小中点）、有时给 •、有时干脆不认，
		// 直接画出来大小不一，看着就像"没带圆点"。这里统一成一个字形。
		if (reg.bullet && !text.empty()) {
			stripLeadingBullet(text);
			text = std::wstring(L"\u2022  ") + text;   // 圆点后留两个空格：和正文拉开一点
		}
		const float boxW = std::max(1.f, reg.w * scaleX);   // 面板物理像素
		// 竖排列高各不相同（长列 679、短列 111…），不能用横排那套"统一正文行高"，
		// 否则长列被缩短上移、短列被撑高、字距拉大。竖排一律用它自己的框高。
		const float boxH = reg.vertical
			? std::max(1.f, reg.h * scaleY)
			: ((hEff[i] > 0.f) ? hEff[i] : std::max(1.f, reg.h * scaleY));

		auto* box = body->makeChild<OcrText>();
		box->fitW = boxW;
		box->fitH = boxH;
		box->boxL = reg.x * scaleX;
		box->boxT = reg.y * scaleY;
		box->setPositionType(Ling::Position::Absolute);
		// 节点自身几何按识别框摆放（文字命中选择用）；实际绘制位置由 layout() 精确定位。
		box->setPosition(Ling::Edge::Left, rx);
		box->setPosition(Ling::Edge::Top, ry);
		// 重绘识别文字：用户要能看到识别结果才能核对；字号已在建框时按识别框适配过。
		box->setColor(fg);
		// 拖选文字的背景色统一 #1890FF（原位覆盖 / 文本重排一致）
		box->setSelectionBgColor(0x1890FFFF);
		box->setSelectionTextColor(0xFFFFFFFF);   // 拖选反白
		// 大字标题估出的加粗；小字不猜
		if (reg.bold) box->setBold(true);
		// 不能留 TextBox 默认的 6px 内边距：OcrText 的缩放比是按「识别框宽 / 控件宽」算的，
		// 控件宽含内边距就会被算成"文字比框宽 12px"，文字被整体压小一圈（短行尤其明显），
		// 纵向也会因为多出的上下内边距而偏移。识别文字是原位覆盖，不需要内边距。
		box->setPadding(0.f);
		box->setAutoSize(true);
		box->setMouseSelectionEnabled(false);   // 选区由 ToolOcr 统一接管
		box->align = (i < aligns.size()) ? aligns[i] : 0;

		if (reg.vertical && !text.empty()) {
			// 竖排：一个字一行（换行符让 TextBox 自然堆叠）。
			// 字号按"格的短边"定，整列再由 OcrText 按框高缩放 —— 之前按横排塞进
			// 54px 宽的窄框会把字缩到十来个像素，看起来就是一片空白。
			std::wstring stacked;
			int cells = 0;
			for (size_t k = 0; k < text.size(); ++k) {
				const wchar_t c = text[k];
				if (c == L'\r' || c == L'\n') continue;
				if (!stacked.empty()) stacked += L'\n';
				stacked += c;
				if (c >= 0xD800 && c <= 0xDBFF && k + 1 < text.size()) stacked += text[++k];   // 代理对
				++cells;
			}
			// 字号按**列宽**定（字大小跟着线宽走，不跟列高走），
			// 再用 boxH/n 兜底防止字多放不下。系数取 0.70：识别框比字形宽 0.15×列宽
			// （见 OcrEngine 的外扩），0.75 算出来会比原图略大一圈。
			const float cell = std::min(boxW * 0.70f, boxH / (float)std::max(1, cells));
			const float fLogical = std::clamp(cell / (kCjkInkRatio * dpi), 7.f / dpi, 300.f / dpi);
			box->inkH = (float)cells * fLogical * dpi * kCjkInkRatio;
			box->vFill = 0.98f;
			box->align = 1;                        // 竖排一律在列内水平居中
			box->vertical = true;
			box->logical = text;
			box->setFontSize(fLogical);
			box->setText(stacked);
		}
		else {
			const bool wide = hasWideGlyph(text);
			const float inkRatio = wide ? kCjkInkRatio : kYaheiInkRatio;
			const float vFill = wide ? kCjkVFill : 1.f;
			float fLogical = std::clamp(boxH * vFill / (inkRatio * dpi), 7.f / dpi, 300.f / dpi);
			// 用**字号**把文字适配到识别框：先量当前字号的自然宽高，再把字号乘上
			// "宽/高哪个先顶满"的倍率。以前是绘完再用 visual.Scale 拉伸位图，短行放大后
			// 边缘会糊；改字号是重新排版，字形始终按最终尺寸栅格化，边缘是清楚的。
			float naturalW = 0.f, naturalH = 0.f;
			if (measureTextSize(text, fLogical, dpi, reg.bold, naturalW, naturalH)) {
				const float inkH = fLogical * dpi * inkRatio;
				const float fitS = std::min(
					naturalW > 0.f ? boxW / naturalW : 1.f,
					inkH > 0.f ? boxH * vFill / inkH : 1.f);
				fLogical = std::clamp(fLogical * fitS, 7.f / dpi, 300.f / dpi);
			}
			box->inkH = fLogical * dpi * inkRatio;
			box->vFill = vFill;
			box->setFontSize(fLogical);
			box->setText(text);
		}
		editors.push_back(box);
	}
	// 文本重排（或没有逐框结果时）落成一个整段文本框：可编辑、Enter 换行，
	// 和普通多行输入框一致。文本优先用整段识别结果，缺失时按识别框顺序拼。
	std::wstring fullText = result_.text;
	if (reflow_ && fullText.empty()) {
		for (const auto& reg : result_.regions) {
			const std::wstring& t = reg.displayText.empty() ? reg.text : reg.displayText;
			if (t.empty()) continue;
			if (!fullText.empty()) fullText += L'\n';
			fullText += t;
		}
	}
	if ((reflow_ || editors.empty()) && !fullText.empty()) {
		if (reflow_) {
			// 整块铺满选区：完全不露出原画面（文字框四周也不透出来）
			auto* bgNode = body->makeChild<Ling::Node>();
			bgNode->setPositionType(Ling::Position::Absolute);
			bgNode->setPosition(Ling::Edge::Left, 0.f);
			bgNode->setPosition(Ling::Edge::Top, 0.f);
			bgNode->setSize(w / dpi, h / dpi);
			bgNode->setBg(palBg_);
			bgs_.push_back(bgNode);
		}
		{
			// 文本重排：一行对一行，保留识别出的换行；不合并折行、不额外插空行。
			// 全篇一个字号（带可读下限），圆点统一成识别结果里出现最多的那种。
			constexpr float kMinFontCjk = 13.f;    // 中文最小字号（逻辑像素）
			constexpr float kMinFontLatin = 12.f;  // 西文最小字号
			std::vector<float> inkOf(result_.regions.size(), 0.f);
			for (size_t i = 0; i < result_.regions.size(); ++i) {
				const auto& r = result_.regions[i];
				inkOf[i] = estimateInkHeight(srcImage_, imgW, imgH, r.x, r.y, r.w, r.h) * scaleY;
			}
			std::vector<float> bodyInks;
			for (float v : inkOf) if (v >= 3.f) bodyInks.push_back(v);
			std::sort(bodyInks.begin(), bodyInks.end());
			const float bodyInk = bodyInks.empty() ? 0.f : bodyInks[bodyInks.size() / 2];

			// 圆点多数表决：OCR 可能混着 • · ●，取出现最多的；票数相同用先出现的
			wchar_t bulletChar = 0;
			{
				std::vector<wchar_t> order;
				std::vector<int> counts;
				for (const auto& r : result_.regions) {
					const std::wstring& t = r.displayText.empty() ? r.text : r.displayText;
					const wchar_t c = leadingBulletChar(t);
					if (!c) continue;
					auto it = std::find(order.begin(), order.end(), c);
					if (it == order.end()) { order.push_back(c); counts.push_back(1); }
					else ++counts[(size_t)(it - order.begin())];
				}
				if (!order.empty()) {
					size_t best = 0;
					for (size_t k = 1; k < order.size(); ++k) if (counts[k] > counts[best]) best = k;
					bulletChar = order[best];
				}
			}

			std::wstring text;
			float bx0 = 1e9f, by0 = 1e9f;
			for (const auto& r : result_.regions) {
				std::wstring t = r.displayText.empty() ? r.text : r.displayText;
				if (t.empty()) continue;
				const wchar_t lead = leadingBulletChar(t);
				if (lead) {
					// 圆点后留两个空格：和正文拉开一点，读起来不黏
					stripLeadingBullet(t);
					t = std::wstring(1, bulletChar ? bulletChar : lead) + L"  " + t;
				}
				if (!text.empty()) text += L'\n';
				text += t;
				bx0 = std::min(bx0, r.x);
				by0 = std::min(by0, r.y);
			}
			if (!text.empty()) {
				// 统一字号：识别字身（墨迹高中位）换算后再 +1 —— 同字号下重排出来的观感偏小，
				// 抬 1 号更好读；再夹在 13/12 下限与 21 上限之间。
				const bool cjk = hasWideGlyph(text);
				const float ratio = cjk ? kCjkInkRatio : kYaheiInkRatio;
				const float minFont = cjk ? kMinFontCjk : kMinFontLatin;
				const float bodyFont = std::clamp(bodyInk / (ratio * dpi) + 1.f, minFont, 21.f);
				const float pad = 2.f * dpi;
				const float left = std::max(0.f, bx0 * scaleX - pad);
				const float top = std::max(0.f, by0 * scaleY - pad);
				auto* box = body->makeChild<Ling::TextBox>();
				box->setPositionType(Ling::Position::Absolute);
				box->setPosition(Ling::Edge::Left, left / dpi);
				box->setPosition(Ling::Edge::Top, top / dpi);
				box->setSize(std::max(80.f, w - left / dpi - 2.f), std::max(40.f, h - top / dpi - 2.f));
				box->setPadding(0.f);
				box->setFontSize(bodyFont);
				box->setLineSpacing(bodyFont * 1.75f);
				box->setBg(palBg_);
				box->setColor(palFg_);
				box->setSelectionBgColor(0x1890FFFF);
				box->setSelectionTextColor(0xFFFFFFFF);
				box->setText(text);
				reflowHistory_.assign(1, text);
				reflowHistPos_ = 0;
				box->onTextChanged.add([this](Ling::TextBox*, const std::wstring& t) {
					if (reflowHistGuard_) return;
					reflowHistory_.resize((size_t)reflowHistPos_ + 1);
					reflowHistory_.push_back(t);
					reflowHistPos_ = (int)reflowHistory_.size() - 1;
					if (reflowHistory_.size() > 200) {
						reflowHistory_.erase(reflowHistory_.begin());
						reflowHistPos_ = (int)reflowHistory_.size() - 1;
					}
				});
				editors.push_back(box);
			}
		}
		if (editors.empty()) {
			// 兜底（没有区域信息）：整段一个框
			auto* box = body->makeChild<Ling::TextBox>();
			box->setPositionType(Ling::Position::Absolute);
			box->setPosition(Ling::Edge::Left, 8.f);
			box->setPosition(Ling::Edge::Top, 40.f);
			box->setSize(std::max(80.f, w / dpi - 16.f), std::max(40.f, h / dpi - 48.f));
			box->setPadding(0.f);
			box->setBg(palBg_);
			box->setColor(palFg_);
			box->setSelectionBgColor(0x1890FFFF);
			box->setSelectionTextColor(0xFFFFFFFF);
			box->setText(fullText);
			editors.push_back(box);
		}
	}
	// 选区状态跟着重建作废；镜像层保持在最上层
	selDragging_ = false;
	selALine_ = selBLine_ = -1;
	selAIdx_ = selBIdx_ = 0;
	if (annotCanvas_) body->bringToFront(annotCanvas_);
	// 提示条的收/顶一律放在**编辑框都建好之后**：先收条、再建框，中间那段（建框 + 首帧排版）
	// 就是用户看到的"正在翻译…没了、结果还没出来"。顺序换过来，收条的同一帧结果已在场。
	// 翻译进行中（busyMsg_ 非空）则无论如何都顶着，只有 finishOcrTranslate 落地结果时才清。
	if (!busyMsg_.empty()) showBanner(busyMsg_, false);
	else if (!result_.regions.empty()) clearBanner();
	else if (!banner_.empty()) showBanner(banner_, bannerError_);
	// 右上角按钮始终在识别结果、标注镜像层与提示条之上（重建文字块会重新排到前面）
	if (compareBtn) body->bringToFront(compareBtn);
	if (copyBtn) body->bringToFront(copyBtn);
	// 不自动选中/聚焦：结果只是"叠在原图上供阅读"，一进来就整段高亮并不合适；点哪块再编辑哪块。
}

void ToolOcr::applyTranslateLines(const std::vector<std::wstring>& lines)
{
	// lines 是**按段**来的（WinCap 已把同段的折行拼好）：回填到每段的首行，其余行清空；
	// 排版按段取首行的 displayText（见 rebuildEditors 的翻译分支）。
	size_t seg = 0;
	for (size_t i = 0; i < result_.regions.size(); i++) {
		const bool newPara = (i == 0) || result_.regions[i].para != result_.regions[i - 1].para;
		if (newPara) {
			result_.regions[i].displayText = (seg < lines.size()) ? lines[seg] : std::wstring();
			++seg;
		}
		else {
			result_.regions[i].displayText.clear();
		}
	}
	// 兜底：只回来一条译文（引擎按整段返回）时落到第一段上
	if (lines.size() == 1 && !result_.regions.empty() && result_.regions[0].displayText.empty())
		result_.regions[0].displayText = lines[0];

	result_.text.clear();
	for (const auto& r : result_.regions) {
		if (r.displayText.empty()) continue;
		if (!result_.text.empty()) result_.text += L'\n';
		result_.text += r.displayText;
	}
}

void ToolOcr::applyTranslate(const std::vector<std::wstring>& lines)
{
	applyTranslateLines(lines);
	rebuildEditors();
}

void ToolOcr::setTranslatedResult(OcrResult result, const std::vector<std::wstring>& lines)
{
	result_ = std::move(result);
	applyTranslateLines(lines);
	// 翻译面板是先用**空结果**建的（识别在后台跑），那一步采样不到区域，底色只能是默认白。
	// 结果一到就按真实区域重采一次底色/文字色 —— 否则深色、彩色底图上会糊一块白板，
	// 和 OCR 识别模式的观感完全对不上。
	if (!srcImage_.empty())
		sampleLayerPalette(srcImage_, imgW, imgH, result_.regions, palBg_, palFg_);
	rebuildEditors();
}

void ToolOcr::setResult(OcrResult result)
{
	result_ = std::move(result);
	// 同上：结果后到的路径也要重采底色（面板先建、识别后填）
	if (!srcImage_.empty())
		sampleLayerPalette(srcImage_, imgW, imgH, result_.regions, palBg_, palFg_);
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
	if (body) body->bringToFront(bannerNode);   // 提示条永远压在最上面
}

void ToolOcr::clearBanner()
{
	banner_.clear();
	busyBase_.clear();
	busyDots_ = 0;
	killTimer(kTypingTimerId);
	if (bannerNode) bannerNode->hide();
}

void ToolOcr::setBusy(const std::wstring& msg)
{
	// 识别/翻译可能几秒才出结果：提示条末尾循环点点（正在识别 → . → .. → ...），
	// 让人一眼看出还在跑，而不是一条静止的文字。
	busyMsg_ = msg;
	busyBase_ = msg;
	while (!busyBase_.empty() && (busyBase_.back() == L'.' || busyBase_.back() == L'\u2026' ||
		busyBase_.back() == L' '))
		busyBase_.pop_back();
	busyDots_ = 0;
	showBanner(busyBase_, false);
	setTimer(kTypingTickMs, kTypingTimerId);
}

void ToolOcr::clearBusy()
{
	busyMsg_.clear();
}

void ToolOcr::undoRedoReflow(bool redo)
{
	if (reflowHistory_.empty() || editors.empty() || !editors[0]) return;
	const int next = reflowHistPos_ + (redo ? 1 : -1);
	if (next < 0 || next >= (int)reflowHistory_.size()) return;
	reflowHistPos_ = next;
	reflowHistGuard_ = true;
	editors[0]->setText(reflowHistory_[(size_t)next]);
	reflowHistGuard_ = false;
	repaint();
}

void ToolOcr::copyAll()
{
	// 有选区就只复制选中的部分；没有就整段
	std::wstring text = selectedText();
	if (text.empty()) {
		if (reflow_ && !result_.text.empty()) {
			// 文本重排：复制**原始识别文本**（不补圆点、不改字符），出去就是纯文本
			text = result_.text;
		}
		else {
			for (size_t i = 0; i < editors.size(); i++) {
				if (i) text += L'\n';
				auto* ot = dynamic_cast<OcrText*>(editors[i]);
				text += (ot && ot->vertical) ? ot->logical : editors[i]->getText();
			}
		}
	}
	if (text.empty()) text = result_.text;
	// 只写 CF_UNICODETEXT：不带 RTF/HTML，粘贴出去没有任何样式
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

// —— 跨行选区 ——
// 每行是一个独立控件，命中落点/选区范围都由本类统一计算（各行自身的鼠标选区已关闭），
// 因此"点到的位置"与"看到的文字"始终一致，且可以跨行拖选。

ToolOcr::SelPoint ToolOcr::hitSelPoint(POINT p) const
{
	SelPoint sp;
	if (editors.empty()) return sp;
	// 先定"行"。竖排整版时各列的 Y 区间几乎完全重合，按 Y 定会永远命中第一列
	// （表现为拖选拉不到别的列），所以竖排为主时必须按 X 定列；横排仍按 Y 定行。
	int vertCount = 0, totalCount = 0;
	for (auto* e : editors) {
		if (!e) continue;
		++totalCount;
		auto* t = dynamic_cast<OcrText*>(e);
		if (t && t->vertical) ++vertCount;
	}
	const bool byX = totalCount > 0 && vertCount * 2 >= totalCount;

	int best = 0;
	float bestDist = FLT_MAX;
	for (size_t i = 0; i < editors.size(); ++i) {
		auto* e = editors[i];
		if (!e) continue;
		const float primary = byX ? (float)p.x : (float)p.y;
		const float lo = byX ? e->x : e->y;
		const float hi = lo + (byX ? e->w : e->h);
		if (primary >= lo && primary <= hi) { best = (int)i; bestDist = 0.f; break; }
		const float d = std::fabs(primary - (lo + hi) * 0.5f);
		if (d < bestDist) { bestDist = d; best = (int)i; }
	}
	auto* e = editors[best];
	if (!e) return sp;
	sp.line = best;
	auto* ot = dynamic_cast<OcrText*>(e);
	if (ot && ot->vertical) {
		// 竖排是"每字一行"的堆叠文本，按 Y 轴均分到字符格
		const int n = (int)ot->logical.size();
		const float extent = e->h;
		const float offset = (float)p.y - e->y;
		if (n > 0 && extent > 1.f) {
			const float f = std::clamp(offset / extent, 0.f, 1.f);
			const float pos = f * (float)n;            // 0~n：落在第几个字符边界上
			// 首尾各放宽半格到 0.75 格：起手/收手常按在第一个字符上，
			// 按"最近边界"取整就会从第二个字开始选（用户反馈拖选老是从第二字起）。
			if (pos <= 0.75f) sp.index = 0;
			else if (pos >= (float)n - 0.75f) sp.index = n;
			else sp.index = std::clamp((int)std::lround(pos), 0, n);
		}
	}
	else {
		// 横排（含翻译框）：按真实字形边界命中；点在框外时 DWrite 夹到首/尾，正是边缘起手/收手要的
		sp.index = e->hitTextIndex(p);
	}
	return sp;
}

void ToolOcr::applySelection()
{
	const int n = (int)editors.size();
	if (n == 0) return;
	if (selALine_ < 0 || selBLine_ < 0) { clearSelection(); return; }
	// 阅读顺序（行优先）归一化
	int sl = selALine_, si = selAIdx_, el = selBLine_, ei = selBIdx_;
	if (sl > el || (sl == el && si > ei)) { std::swap(sl, el); std::swap(si, ei); }
	for (int i = 0; i < n; ++i) {
		auto* e = editors[i];
		if (!e) continue;
		if (i < sl || i > el) { e->setSelectionRange(0, 0); continue; }
		auto* ot = dynamic_cast<OcrText*>(e);
		const bool vert = ot && ot->vertical;
		const int len = vert ? (int)ot->logical.size() : (int)e->getText().size();
		int a = 0, b = 0;
		if (sl == el) { a = si; b = ei; }
		else if (i == sl) { a = si; b = len; }
		else if (i == el) { a = 0; b = ei; }
		else { a = 0; b = len; }
		if (vert) {
			// 竖排控件的文本是"字\n字\n字"：逻辑下标换算回控件下标再做高亮
			const int rawLen = (int)e->getText().size();
			const int rawA = std::clamp(a * 2, 0, rawLen);
			const int rawB = (b > a) ? std::clamp(b * 2 - 1, 0, rawLen) : rawA;
			e->setSelectionRange(rawA, rawB);
		}
		else {
			e->setSelectionRange(a, b);
		}
	}
}

void ToolOcr::clearSelection()
{
	for (auto* e : editors) if (e) e->setSelectionRange(0, 0);
	selDragging_ = false;
	selALine_ = selBLine_ = -1;
	selAIdx_ = selBIdx_ = 0;
}

std::wstring ToolOcr::selectedText() const
{
	const int n = (int)editors.size();
	if (n == 0 || selALine_ < 0 || selBLine_ < 0) return L"";
	int sl = selALine_, si = selAIdx_, el = selBLine_, ei = selBIdx_;
	if (sl > el || (sl == el && si > ei)) { std::swap(sl, el); std::swap(si, ei); }
	if (sl < 0 || el >= n) return L"";
	std::wstring out;
	for (int i = sl; i <= el; ++i) {
		auto* e = editors[i];
		if (!e) continue;
		auto* ot = dynamic_cast<OcrText*>(e);
		const std::wstring t = (ot && ot->vertical) ? ot->logical : e->getText();
		const int len = (int)t.size();
		int a = (i == sl) ? si : 0;
		int b = (i == el) ? ei : len;
		a = std::clamp(a, 0, len);
		b = std::clamp(b, 0, len);
		if (b <= a) continue;
		if (!out.empty()) out += L'\n';
		out += t.substr((size_t)a, (size_t)(b - a));
	}
	return out;
}

void ToolOcr::layout()
{
	Ling::WinBase::layout();
	// 标注/控件镜像层：两个窗口无法互相穿插，把宿主的内容镜像到本窗口顶层。
	// 控件画在标注之后（同一层里最后画），保证锁定/编辑/关闭永远在文字与标注之上。
	if (annotCanvas_ && hasMask_ && (annotPainter_ || chromePainter_)) {
		auto* ctx = annotCanvas_->startPaint();
		if (ctx) {
			ctx->Clear(0);
			if (annotPainter_) {
				ctx->SetTransform(D2D1::Matrix3x2F::Translation(-maskRect_.left, -maskRect_.top));
				annotPainter_(ctx);
			}
			if (chromePainter_) {
				ctx->SetTransform(D2D1::Matrix3x2F::Identity());
				chromePainter_(ctx);
			}
			annotCanvas_->finishPaint();
		}
	}
}

void ToolOcr::syncToMask(const D2D1_RECT_F& maskRect, int hostX, int hostY)
{
	maskRect_ = maskRect;
	hasMask_ = true;
	hostX_ = hostX;
	hostY_ = hostY;
	const int left = (int)std::lround(maskRect.left);
	const int top = (int)std::lround(maskRect.top);
	const int right = (int)std::lround(maskRect.right);
	const int bottom = (int)std::lround(maskRect.bottom);
	// 面板尺寸取「覆盖矩形」；识别源图尺寸 imgW/imgH 保持不变 ——
	// rebuildEditors 用 w/imgW 求缩放比，贴图缩放后识别框才会跟着一起放大。
	const int panelW = std::max(1, right - left);
	const int panelH = std::max(1, bottom - top);
	const bool sizeChanged = (panelW != panelW_ || panelH != panelH_);
	panelW_ = panelW;
	panelH_ = panelH;
	applyMaskGeometry();
	// 字号和坐标都是按 dpi 换算的：dpi 变了（换屏 / 改缩放）必须重排。
	// dpi 的变化就发生在上面的 setPosition 里，所以放在这里判断。
	if (sizeChanged || std::fabs(dpi - builtDpi_) > 0.001f) rebuildEditors();
}

// 把面板重新贴回选区。跨屏缩放不同时，系统会在 WM_DPICHANGED 后按「建议矩形」改窗口尺寸，
// 所以 DPI 稳定后再按选区物理尺寸重算一次；顺序仍是先定位、后定尺寸。
void ToolOcr::applyMaskGeometry()
{
	if (!hwnd || !hasMask_ || panelW_ <= 0 || panelH_ <= 0) return;
	setPosition(hostX_ + (int)std::lround(maskRect_.left),
		hostY_ + (int)std::lround(maskRect_.top));
	setSize((float)panelW_ / dpi, (float)panelH_ / dpi);
}

bool ToolOcr::hitText(POINT client)
{
	if (!hwnd || !IsWindowVisible(hwnd)) return false;
	for (auto* e : editors) {
		if (e && e->isPosIn(client)) return true;
	}
	// 背景块也归"文字区域"：框内留白同样是文字的地盘，只有框间空档才让给窗口拖动。
	// 文本重排的背景块铺满整块，不能算（否则整张贴图都没法拖了）。
	if (!reflow_) {
		for (auto* bg : bgs_) {
			if (bg && bg->isPosIn(client)) return true;
		}
	}
	return false;
}

bool ToolOcr::nearText(POINT client) const
{
	// 文本框外扩一圈：边缘起手与工字光标共用
	const float pad = 4.f * (dpi > 0.f ? dpi : 1.f);
	for (auto* e : editors) {
		if (!e) continue;
		if ((float)client.x >= e->x - pad && (float)client.x <= e->x + e->w + pad
			&& (float)client.y >= e->y - pad && (float)client.y <= e->y + e->h + pad)
			return true;
	}
	// 贴图：识别框自身的留白也算文字区域；截图模式不加，留白继续让给画布
	if (pinOverlay_ && !reflow_) {
		for (auto* bg : bgs_) {
			if (bg && bg->isPosIn(client)) return true;
		}
	}
	return false;
}

BOOL ToolOcr::setCursor()
{
	if (hwnd) {
		POINT p{};
		GetCursorPos(&p);
		ScreenToClient(hwnd, &p);
		if (nearText(p)) {
			SetCursor(LoadCursor(nullptr, IDC_IBEAM));
			return TRUE;
		}
	}
	return Ling::WinBase::setCursor();
}

LRESULT ToolOcr::onHitTest(const POINT screenPos)
{
	// 只有识别文本框附近吃鼠标：可选中/复制文字；其余区域一律穿透。
	//  - 贴图模式：穿透后贴图仍能右键弹菜单 / 滚轮缩放；
	//  - 截图模式：穿透后主画布照常收到鼠标 —— 识别面板开着也能继续画标注，不必先取消识别。
	POINT client = screenPos;
	ScreenToClient(hwnd, &client);
	if (nearText(client)) return HTCLIENT;
	// 贴图宿主控件在浮层最上层：命中就吃鼠标（点击由 chromeDown_ 转给宿主）
	if (pinOverlay_ && chromeHit_ && chromeHit_(client)) return HTCLIENT;
	// 右上角按钮平时隐藏，只有悬停在选区内才吃鼠标（隐藏时点击继续落到宿主画布上）
	if (buttonsShown_) {
		if (copyBtn && copyBtn->isPosIn(client)) return HTCLIENT;
		if (compareBtn && compareBtn->isPosIn(client)) return HTCLIENT;
	}
	return HTTRANSPARENT;
}

void ToolOcr::onMinMaxInfo(MINMAXINFO* mmi)
{
	mmi->ptMinTrackSize.x = 1;
	mmi->ptMinTrackSize.y = 1;
}
