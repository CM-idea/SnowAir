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
	// 墨迹高 → 字号 的换算系数（见 kYaheiInkRatio）。
	constexpr float kInkToFont = 1.f / kYaheiInkRatio;
	// 中日韩的方块字几乎占满整个 em，不像西文只占 ≈0.875。用同一个墨迹比换算，中文会被放大一档、
	// 溢出识别框，上下两行贴到一起（翻译成中文时尤其明显）。所以按脚本分别取墨迹比。
	// 方块字墨迹比取 1.0（保守：不同字体的字面还可能略超 em），再留 10% 纵向余量 ——
	// 余量给太少时，贴边的那几行会被面板窗口裁掉（用户反馈"译文还是可能被裁剪"）。
	constexpr float kCjkInkRatio = 1.0f;
	constexpr float kCjkVFill = 0.90f;

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
		float inkH{ 0.f };   // 当前字号下的字形高度（物理像素）
		float vFill{ 1.f };  // 纵向允许占到的框高比例（CJK 留余量，防上下两行叠字）
		int align{ 0 };      // 原文对齐：0=居左 1=居中 2=居右（译文比原文短时才有区别）
	protected:
		void layout() override
		{
			Ling::TextBox::layout();
			if (fitW <= 0.f || fitH <= 0.f || inkH <= 0.f || w <= 0.f || h <= 0.f) return;
			// 等比缩放（取"框宽/文字宽"与"框高/字形高"的较小者），横纵同比例，绝不单独拉伸某一轴。
			const float s = std::min(fitW / w, fitH * vFill / inkH);
			// 水平按原文对齐摆放：译文比原文短、装不满框时，居左/居中/居右的差别才看得出来。
			// 居左 = 和原文一样从同一列起排；居中/居右照原样还原。
			const float visW = w * s;
			float visX = boxL;                                     // 居左
			if (align == 1) visX = boxL + (fitW - visW) * 0.5f;     // 居中
			else if (align == 2) visX = boxL + (fitW - visW);       // 居右
			// ox 是"未缩放节点"的偏移：缩放绕节点自身中心，故视觉左边缘 = ox + (w - visW)/2
			const float ox = visX - (w - visW) * 0.5f;
			// 纵向仍居中：文字块的墨迹落在识别框中间，和原文的墨迹位置对齐。
			const float oy = boxT + (fitH - h) * 0.5f;
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
		const SelPoint sp = hitSelPoint(pos);
		if (sp.line < 0) return;
		selActive_ = true;
		selALine_ = selBLine_ = sp.line;
		selAIdx_ = selBIdx_ = sp.index;
		SetFocus(hwnd);
		SetCapture(hwnd);
		applySelection();
	});
	onMouseMove.add([this](POINT pos) {
		if (!selActive_) return;
		const SelPoint sp = hitSelPoint(pos);
		if (sp.line < 0) return;
		selBLine_ = sp.line;
		selBIdx_ = sp.index;
		applySelection();
	});
	onMouseUp.add([this](POINT, bool) {
		if (!selActive_) return;
		selActive_ = false;
		ReleaseCapture();
		applySelection();
	});
	onKeyDown.add([this](UINT key) {
		const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
		if (ctrl && key == 'A') {   // 全选（跨行）
			if (!editors.empty()) {
				selActive_ = true;
				selALine_ = 0; selAIdx_ = 0;
				selBLine_ = (int)editors.size() - 1;
				selBIdx_ = (int)editors.back()->getText().size();
				applySelection();
			}
			return;
		}
		if (ctrl && key == 'C') { copyAll(); return; }
		if (ctrl && key == 'X') return;   // 只读展示，剪切忽略
		if (this->host) this->host->onKeyDown(key);
	});
	// 拖选中保持 I 形光标：此时鼠标可能已经离开某些行，各行的 hover 态不生效。
	onCursor.add([this](bool* handled) {
		if (selActive_) { SetCursor(LoadCursor(nullptr, IDC_IBEAM)); *handled = true; }
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

	if (!pinOverlay_) {
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
	}

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
	show();
}

void ToolOcr::rebuildEditors()
{
	for (auto* e : editors) {
		if (e) body->removeChild(e);
	}
	editors.clear();
	for (auto* n : bgs_) {
		if (n) body->removeChild(n);
	}
	bgs_.clear();

	if (!result_.ok) {
		showBanner(result_.error.empty() ? Lang::get(L"ocr.fail") : result_.error, true);
		if (bannerNode) body->bringToFront(bannerNode);
		return;
	}
	// 有结果才收起提示条：面板还空着时（例如翻译模式刚建好、译文未到）保留提示，
	// 免得重建一次就把"正在翻译…"收掉、面板却什么都没显示。
	if (!result_.regions.empty()) clearBanner();

	const float scaleX = (imgW > 0) ? (w / (float)imgW) : 1.f;
	const float scaleY = (imgH > 0) ? (h / (float)imgH) : 1.f;

	// 正文统一字号：同一段正文里各框松紧不一（PP-OCR 的框带边距、混排时还会更高），逐框按自身高度
	// 算就会"一行大一行小"。做法：取偏低分位当**正文基准**（大标题会把均值/中位抬高，偏低分位对
	// 少数大框不敏感），正文行一律用正文框高的**平均值**定字号；明显更大的行（标题）保留自身高度。
	std::vector<float> hEff(result_.regions.size(), 0.f);
	{
		const size_t n = result_.regions.size();
		std::vector<float> hs(n);
		for (size_t i = 0; i < n; i++)
			hs[i] = std::max(1.f, result_.regions[i].h * scaleY);
		std::vector<float> sorted = hs;
		std::sort(sorted.begin(), sorted.end());
		const float bodyRef = sorted.empty() ? 0.f : sorted[sorted.size() / 4];
		const float headingThresh = bodyRef * 1.45f;
		float bodySum = 0.f;
		int bodyN = 0;
		for (size_t i = 0; i < n; i++) {
			if (hs[i] < headingThresh) { bodySum += hs[i]; ++bodyN; }
		}
		const float bodyAvg = (bodyN > 0) ? bodySum / (float)bodyN : bodyRef;
		for (size_t i = 0; i < n; i++)
			hEff[i] = (hs[i] < headingThresh) ? bodyAvg : hs[i];
	}

	// 原文对齐检测：译文比原文短、装不满框时才有观感差别 —— 居中的标题要跟着居中、
	// 右对齐的日期/页码要跟着靠右。判据基于"**行与行之间的共识**"（只看单行的左右留白会被
	// 整段缩进 / 选区没框正带偏）：
	//   ① 先定整块的主导对齐：比较左边界/中线/右边界各自"最多有几行共用"。若中线共用的行数
	//      **严格多于**左右、且至少 2 行，就判定整块居中，所有行一律居中 —— 避免同一段文字里
	//      出现"一行居左一行居中"（各行长度不同时，中线重合才是真的居中）。
	//   ② 否则逐行：贴"多行共用的左边界"→居左；贴多行共用的右边界、或顶到整块最右且明显短→居右；
	//      与"多行共用的中线"重合且两侧都明显内缩→居中；都不像→居左。
	std::vector<int> aligns(result_.regions.size(), 0);
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
			const float inset = std::max(4.f, blockW * 0.08f);    // 居中要求两侧都明显内缩

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
			const auto idC = cluster(cs, szC);
			int bestL = 0, bestR = 0, bestC = 0;
			for (int s : szL) bestL = std::max(bestL, s);
			for (int s : szR) bestR = std::max(bestR, s);
			for (int s : szC) bestC = std::max(bestC, s);
			// ① 整块居中：中线共用的行数严格多于左右，且至少两行
			const bool blockCentered = (bestC >= 2 && bestC > bestL && bestC > bestR);

			for (size_t i = 0; i < n; i++) {
				if (blockCentered) { aligns[i] = 1; continue; }
				const float lineW = rs[i] - ls[i];
				const bool multiLeft = idL[i] >= 0 && szL[idL[i]] >= 2;
				const bool multiRight = idR[i] >= 0 && szR[idR[i]] >= 2;
				const bool multiCenter = idC[i] >= 0 && szC[idC[i]] >= 2;
				if (multiLeft) {
					aligns[i] = 0;                                   // 贴着多行共用的左边界
				}
				else if (multiRight || (maxR - rs[i] <= tolEdge && lineW < blockW * 0.85f)) {
					aligns[i] = 2;                                   // 多行共用右边界 / 顶右且不是满行
				}
				else if (multiCenter && (ls[i] - minL) > inset && (maxR - rs[i]) > inset) {
					aligns[i] = 1;                                   // 与多行共用的中线重合，且两侧内缩
				}
				else {
					aligns[i] = 0;                                   // 都不像 → 仍按居左
				}
			}
		}
	}

	for (size_t i = 0; i < result_.regions.size(); ++i) {
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

		// 1) 背景块：**严格等于识别框**，整块盖住原文字 —— 这是"覆盖到位"的关键。
		auto* bgNode = body->makeChild<Ling::Node>();
		bgNode->setPositionType(Ling::Position::Absolute);
		bgNode->setPosition(Ling::Edge::Left, rx);
		bgNode->setPosition(Ling::Edge::Top, ry);
		bgNode->setSize(rw, rh);
		bgNode->setBg(bg);
		bgNode->setBorderRadius(2.f);
		bgs_.push_back(bgNode);

		// 2) 文字：字号按"字形高度 = 识别框高"取，再交给 OcrText 在 layout() 里
		//    按**真实排版尺寸**缩放并居中到识别框（字形与框上下齐平、左右拉满）。
		//    墨迹比按脚本分：方块字占 em 的比例比西文大，用同一个比会把中文算大、与上下行叠在一起。
		const std::wstring text = reg.displayText.empty() ? reg.text : reg.displayText;
		const float boxW = std::max(1.f, reg.w * scaleX);   // 面板物理像素
		const float boxH = (hEff[i] > 0.f) ? hEff[i] : std::max(1.f, reg.h * scaleY);
		const bool wide = hasWideGlyph(text);
		const float inkRatio = wide ? kCjkInkRatio : kYaheiInkRatio;
		const float vFill = wide ? kCjkVFill : 1.f;
		const float fLogical = std::clamp(boxH * vFill / (inkRatio * dpi), 7.f / dpi, 300.f / dpi);

		auto* box = body->makeChild<OcrText>();
		box->fitW = boxW;
		box->fitH = boxH;
		box->boxL = reg.x * scaleX;
		box->boxT = reg.y * scaleY;
		box->inkH = fLogical * dpi * inkRatio;
		box->vFill = vFill;
		box->align = (i < aligns.size()) ? aligns[i] : 0;
		box->setPositionType(Ling::Position::Absolute);
		// 节点自身几何按识别框摆放（文字命中选择用）；实际绘制位置由 layout() 精确定位。
		box->setPosition(Ling::Edge::Left, rx);
		box->setPosition(Ling::Edge::Top, ry);
		box->setFontSize(fLogical);
		box->setColor(fg);
		box->setAutoSize(true);
		box->setText(text);
		box->setMouseSelectionEnabled(false);   // 选区由 ToolOcr 统一接管
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
	// 选区状态跟着重建作废；镜像层保持在最上层
	selActive_ = false;
	selALine_ = selBLine_ = -1;
	selAIdx_ = selBIdx_ = 0;
	if (annotCanvas_) body->bringToFront(annotCanvas_);
	// 提示条：已经设过就重新顶一次（既压最上面，也保证每次重建后它还在 ——
	// 否则重建一次提示就没了，用户看到"字没了、结果也没来"）。
	if (!banner_.empty()) showBanner(banner_, bannerError_);
	// 不自动选中/聚焦：结果只是"叠在原图上供阅读"，一进来就整段高亮并不合适；点哪块再编辑哪块。
}

void ToolOcr::applyTranslateLines(const std::vector<std::wstring>& lines)
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
	if (body) body->bringToFront(bannerNode);   // 提示条永远压在最上面
}

void ToolOcr::clearBanner()
{
	banner_.clear();
	if (bannerNode) bannerNode->hide();
}

void ToolOcr::copyAll()
{
	// 有选区就只复制选中的部分；没有就整段
	std::wstring text = selectedText();
	if (text.empty()) {
		for (size_t i = 0; i < editors.size(); i++) {
			if (i) text += L'\n';
			text += editors[i]->getText();
		}
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

// —— 跨行选区 ——
// 每行是一个独立控件，命中落点/选区范围都由本类统一计算（各行自身的鼠标选区已关闭），
// 因此"点到的位置"与"看到的文字"始终一致，且可以跨行拖选。

ToolOcr::SelPoint ToolOcr::hitSelPoint(POINT p) const
{
	SelPoint sp;
	if (editors.empty()) return sp;
	// 先按 Y 定行：落在某行内就用它，否则取纵向最近的一行
	int best = 0;
	float bestDist = FLT_MAX;
	for (size_t i = 0; i < editors.size(); ++i) {
		auto* e = editors[i];
		if (!e) continue;
		if ((float)p.y >= e->y && (float)p.y <= e->y + e->h) { best = (int)i; bestDist = 0.f; break; }
		const float d = std::fabs((float)p.y - (e->y + e->h * 0.5f));
		if (d < bestDist) { bestDist = d; best = (int)i; }
	}
	auto* e = editors[best];
	if (!e) return sp;
	sp.line = best;
	const int n = (int)e->getText().size();   // 命中矩形已是缩放后的视觉矩形（见 OcrText::layout）
	if (n > 0 && e->w > 1.f) {
		const float f = std::clamp(((float)p.x - e->x) / e->w, 0.f, 1.f);
		const float pos = f * (float)n;            // 0~n：落在第几个字符边界上
		// 首尾各放宽半格到 0.75 格：起手/收手常按在第一个字符上，
		// 按"最近边界"取整就会从第二个字开始选（用户反馈拖选老是从第二字起）。
		if (pos <= 0.75f) sp.index = 0;
		else if (pos >= (float)n - 0.75f) sp.index = n;
		else sp.index = std::clamp((int)std::lround(pos), 0, n);
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
		const int len = (int)e->getText().size();
		int a = 0, b = 0;
		if (sl == el) { a = si; b = ei; }
		else if (i == sl) { a = si; b = len; }
		else if (i == el) { a = 0; b = ei; }
		else { a = 0; b = len; }
		e->setSelectionRange(a, b);
	}
}

void ToolOcr::clearSelection()
{
	for (auto* e : editors) if (e) e->setSelectionRange(0, 0);
	selActive_ = false;
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
		const std::wstring t = e->getText();
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
	// 标注镜像：把宿主画在它自己窗口里的标注，按与选区一致的平移画到本窗口顶层 ——
	// 两个窗口无法互相穿插，这样"后画的标注"看起来就盖在识别文字上面。
	if (!annotCanvas_ || !annotPainter_ || !hasMask_) return;
	auto* ctx = annotCanvas_->startPaint();
	if (!ctx) return;
	ctx->Clear(0);
	ctx->SetTransform(D2D1::Matrix3x2F::Translation(-maskRect_.left, -maskRect_.top));
	annotPainter_(ctx);
	annotCanvas_->finishPaint();
}

void ToolOcr::syncToMask(const D2D1_RECT_F& maskRect, int hostX, int hostY, float hostDpi)
{
	dpi = hostDpi;
	maskRect_ = maskRect;
	hasMask_ = true;
	const int left = (int)std::lround(maskRect.left);
	const int top = (int)std::lround(maskRect.top);
	const int right = (int)std::lround(maskRect.right);
	const int bottom = (int)std::lround(maskRect.bottom);
	// 面板尺寸取「覆盖矩形」；识别源图尺寸 imgW/imgH 保持不变 ——
	// rebuildEditors 用 w/imgW 求缩放比，贴图缩放后识别框才会跟着一起放大。
	const int panelW = std::max(1, right - left);
	const int panelH = std::max(1, bottom - top);
	const bool sizeChanged = (panelW != panelW_ || panelH != panelH_);
	if (sizeChanged) {
		panelW_ = panelW;
		panelH_ = panelH;
		setSize((float)panelW / dpi, (float)panelH / dpi);
	}
	setPosition(hostX + left, hostY + top);
	// 只在尺寸变化时重建编辑框：拖动只是挪窗口，逐帧重建（还带整图字号分析）会明显卡。
	if (sizeChanged) rebuildEditors();
}

LRESULT ToolOcr::onHitTest(const POINT screenPos)
{
	// 只有识别文本框（及右上角复制按钮）吃鼠标：可选中/复制文字；其余区域一律穿透。
	//  - 贴图模式：穿透后贴图仍能右键弹菜单 / 滚轮缩放；
	//  - 截图模式：穿透后主画布照常收到鼠标 —— 识别面板开着也能继续画标注，不必先取消识别。
	POINT client = screenPos;
	ScreenToClient(hwnd, &client);
	for (auto* e : editors) {
		if (e && e->isPosIn(client)) return HTCLIENT;
	}
	if (copyBtn && copyBtn->isPosIn(client)) return HTCLIENT;
	return HTTRANSPARENT;
}

void ToolOcr::onMinMaxInfo(MINMAXINFO* mmi)
{
	mmi->ptMinTrackSize.x = 1;
	mmi->ptMinTrackSize.y = 1;
}
