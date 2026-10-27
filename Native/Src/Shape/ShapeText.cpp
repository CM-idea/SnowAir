#include "pch.h"
#include "App.h"
#include "Util.h"
#include "History.h"
#include "Win/AnnotHost.h"
#include "Tool/ToolSub.h"
#include "ShapeText.h"
#include "RoundBox.h"
#include "LinkGeom.h"
#include "../Lang.h"
#include <random>

using Microsoft::WRL::ComPtr;

ShapeText::ShapeText(AnnotHost* win, bool bubble, bool burst) : ShapeBase(win),
	borderPadding{ 6.f * win->dpi },
	bubble_{ bubble && !burst },
	burst_{ burst },
	draggers(4, D2D1::RectF(0, 0, 0, 0))
{
	setAttr();
	float dashes[] = { 2.f, 2.f };
	Ling::D2D::get()->d2dFactory->CreateStrokeStyle(
		D2D1::StrokeStyleProperties(
			D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_ROUND,
			D2D1_LINE_JOIN_MITER, 10.f, D2D1_DASH_STYLE_CUSTOM, 0.f),
		dashes, ARRAYSIZE(dashes), dashedStrokeStyle.GetAddressOf());
	if (isFrame()) {
		// 气泡 / 爆裂：字号与文字工具默认字号一致（标注滑条是线宽，不是字号），
		// 描边用标注的线宽。默认不再塞「请输入文字」：空框就是空框，创建后直接进编辑显示光标
		fontSize = 24.f * win->dpi;
		bubbleRadius_ = 14.f * win->dpi;
		bubbleStroke_ = std::max(1.f, win->toolSub->getSliderVal());
		updateFrameStyle();
		if (burst_) initBurstJitter();
		makeTextLayout();
		syncRectFromLayout();
		if (bubble_) updateBubbleTail();
	}
}

ShapeText::~ShapeText() {}

void ShapeText::updateDraggers()
{
	const float half = draggerSize * 0.5f;
	const D2D1_RECT_F sr = selectionRect(rect);
	draggers[0] = D2D1::RectF(sr.left - half, sr.top - half, sr.left + half, sr.top + half);
	draggers[1] = D2D1::RectF(sr.right - half, sr.top - half, sr.right + half, sr.top + half);
	draggers[2] = D2D1::RectF(sr.left - half, sr.bottom - half, sr.left + half, sr.bottom + half);
	draggers[3] = D2D1::RectF(sr.right - half, sr.bottom - half, sr.right + half, sr.bottom + half);
}

void ShapeText::syncRectFromLayout()
{
	float textW = 0.f, textH = 0.f;
	if (textLayout && !text.empty()) {
		DWRITE_TEXT_METRICS m{};
		textLayout->GetMetrics(&m);
		textW = m.widthIncludingTrailingWhitespace;
		textH = m.height;
	}
	if (isFrame()) {
		// 气泡/爆裂：只保证装得下文字（四周留白 + 最小尺寸），不把用户拖大的框缩回去
		const float pad = 16.f * win->dpi;
		const float minW = std::max(textW + pad * 2.f, 120.f * win->dpi);
		const float minH = std::max(textH + pad * 2.f, 52.f * win->dpi);
		if (rect.right - rect.left < minW) rect.right = rect.left + minW;
		if (rect.bottom - rect.top < minH) rect.bottom = rect.top + minH;
		if (bubble_) updateBubbleTail();
		updateDraggers();
		return;
	}
	if (!textLayout) return;
	rect.right = rect.left + textW + borderPadding * 2.f;
	// 线上文字（行距被压过）要把最后一行下伸的那点余量补进高度，否则洞/编辑框比字矮
	rect.bottom = rect.top + textH + (attachTo ? attachLineSlack : 0.f) + borderPadding * 2.f;
	updateDraggers();
}

// 气泡外框：圆角矩形 + 尾巴，**一条闭合路径**画出。
// 不用"圆角矩形 ∪ 三角形"：并集在圆角处会留接缝（底边落在直边上就悬空、落在弧上就露弦）。
// 现在沿圆角轮廓按角度走一圈，走到"嘴"（尾巴根部）时跳过那一小段，直接连到尾巴尖 ——
// 轮廓与尾巴天然是同一条线，任何圆角大小、任何尾巴位置都接得上。
ComPtr<ID2D1PathGeometry> ShapeText::buildBubbleGeometry() const
{
	constexpr float kPi = 3.14159265358979323846f;
	auto factory = Ling::D2D::get()->d2dFactory.Get();
	const float w = rect.right - rect.left;
	const float h = rect.bottom - rect.top;
	if (!factory || w < 1.f || h < 1.f) return nullptr;
	const float rr = std::min(bubbleRadius_, std::min(w, h) * 0.5f);
	const float radii[4]{ rr, rr, rr, rr };
	const float hw = w * 0.5f, hh = h * 0.5f;
	const float cx = (rect.left + rect.right) * 0.5f;
	const float cy = (rect.top + rect.bottom) * 0.5f;
	// 尾巴尖在框内：不画尾巴
	const bool tailInside = bubbleTail_.x > rect.left && bubbleTail_.x < rect.right
		&& bubbleTail_.y > rect.top && bubbleTail_.y < rect.bottom;
	if (tailInside) return RoundBox::build(factory, rect, radii);

	const float hb = std::min(std::max(8.f * win->dpi, std::min(w, h) * 0.09f), std::min(w, h) * 0.3f);
	float dx = bubbleTail_.x - cx, dy = bubbleTail_.y - cy;
	if (std::fabs(dx) < 1e-4f && std::fabs(dy) < 1e-4f) dy = 1.f;
	const float th = std::atan2(dy, dx);
	const float radiiO[4]{ rr, rr, rr, rr };
	// 轮廓上按方向角取点（直边/圆角统一由 LinkGeom 算，和采样用同一套数学）
	auto edgeAt = [&](float a, D2D1_POINT_2F& out) {
		D2D1_POINT_2F p{};
		if (!LinkGeom::edgeOnRect(hw, hh, radiiO, a, p)) return false;
		out = D2D1_POINT_2F{ cx + p.x, cy + p.y };
		return true;
	};
	D2D1_POINT_2F pMid{};
	if (!edgeAt(th, pMid)) return RoundBox::build(factory, rect, radii);
	// 嘴的半角宽：轮廓上的弦长 ≈ 2R·sin(delta)
	const float R = std::max(1.f, std::hypot(pMid.x - cx, pMid.y - cy));
	const float delta = std::atan2(hb, R);
	const float mouthA = th + delta;              // 嘴的一侧
	const float span = 2.f * kPi - 2.f * delta;   // 绕轮廓一圈（跳过嘴）

	ComPtr<ID2D1PathGeometry> path;
	if (FAILED(factory->CreatePathGeometry(path.GetAddressOf())) || !path) return nullptr;
	ComPtr<ID2D1GeometrySink> sink;
	if (FAILED(path->Open(sink.GetAddressOf())) || !sink) return nullptr;
	sink->SetFillMode(D2D1_FILL_MODE_WINDING);
	D2D1_POINT_2F start{};
	if (!edgeAt(mouthA, start)) return RoundBox::build(factory, rect, radii);
	sink->BeginFigure(start, D2D1_FIGURE_BEGIN_FILLED);
	const int steps = 480;   // 约 0.75° 一段：圆弧看不出棱角
	for (int i = 1; i <= steps; i++) {
		D2D1_POINT_2F p{};
		if (!edgeAt(mouthA + span * (float)i / (float)steps, p)) continue;
		sink->AddLine(p);
	}
	sink->AddLine(bubbleTail_);   // 嘴的另一侧直接连到尾巴尖
	sink->EndFigure(D2D1_FIGURE_END_CLOSED);
	sink->Close();
	return path;
}

// 每个爆裂一份固定随机参数：每段 2 个系数 —— 弧度伸缩 + 沿弦法线的小偏移。
// 顶点/端点保持模板原样，所以抖动只改变"每条弧鼓多少"，不会把尖角拉出毛刺。
// 只在创建时生成一次，之后 resize/重绘都用同一份。
void ShapeText::initBurstJitter()
{
	std::random_device rd;
	std::mt19937 gen(rd());
	std::uniform_real_distribution<float> dist(-1.f, 1.f);
	burstJitter_.assign(16 * 2, 0.f);
	for (auto& v : burstJitter_) v = dist(gen);
}

// 爆裂外框：直接采用产品给的 Vector 74.svg 路径（637×521 的 16 段三次贝塞尔），
// 按当前 rect 等比铺满；每个实例只对"弧的鼓度"做固定随机化，顶点位置不动。
ComPtr<ID2D1PathGeometry> ShapeText::buildBurstGeometry() const
{
	auto factory = Ling::D2D::get()->d2dFactory.Get();
	const float w = rect.right - rect.left;
	const float h = rect.bottom - rect.top;
	if (!factory || w < 1.f || h < 1.f) return nullptr;
	constexpr float kSvgW = 637.f, kSvgH = 521.f;
	const float sx = w / kSvgW, sy = h / kSvgH;
	struct Cubic {
		float c1x, c1y, c2x, c2y, x, y;
	};
	// Vector 74.svg：M 182.424 1.334 + 16×C … Z
	static const Cubic segs[] = {
		{ 182.424f,   1.334f, 181.681f,  71.072f, 152.281f, 123.086f },
		{ 122.880f, 175.100f,   1.567f, 163.536f,   1.567f, 163.536f },
		{   1.567f, 163.536f,  66.912f, 196.215f,  70.179f, 251.479f },
		{  73.445f, 306.744f,  15.953f, 346.667f,  15.953f, 346.667f },
		{  15.953f, 346.667f, 109.368f, 322.828f, 142.035f, 371.591f },
		{ 174.702f, 420.354f, 152.281f, 504.534f, 152.281f, 504.534f },
		{ 152.281f, 504.534f, 223.332f, 424.424f, 318.067f, 437.428f },
		{ 412.801f, 450.431f, 438.638f, 519.334f, 438.638f, 519.334f },
		{ 438.638f, 519.334f, 425.335f, 429.836f, 477.423f, 399.796f },
		{ 529.512f, 369.755f, 605.794f, 389.834f, 605.794f, 389.834f },
		{ 605.794f, 389.834f, 549.408f, 321.063f, 552.675f, 262.548f },
		{ 555.942f, 204.032f, 634.567f, 163.536f, 634.567f, 163.536f },
		{ 634.567f, 163.536f, 556.908f, 179.790f, 501.374f, 134.278f },
		{ 445.840f,  88.766f, 468.781f,   1.334f, 468.781f,   1.334f },
		{ 468.781f,   1.334f, 441.361f,  68.162f, 333.560f,  87.667f },
		{ 225.758f, 107.172f, 182.424f,   1.334f, 182.424f,   1.334f },
	};
	auto X = [&](float v) { return rect.left + v * sx; };
	auto Y = [&](float v) { return rect.top + v * sy; };
	const bool jittered = burstJitter_.size() >= 16 * 2;
	// 八个尖角统一修成 45° 尖角（和星形一样），而不是让两条曲线在尖点处几乎相切：
	// 取尖点两侧的谷点，算"向内的角平分线"，把两条切向各偏 22.5°。
	// 这样 miter 比例只有约 2.6，尖得干净，不会像原路径的 8° cusp 那样甩长刺。
	struct V2 { float x, y; };
	auto vnorm = [](V2 v) { const float l = std::hypot(v.x, v.y); return l > 1e-5f ? V2{ v.x / l, v.y / l } : V2{ 1.f, 0.f }; };
	auto vrot = [](V2 v, float a) {
		const float c = std::cos(a), s = std::sin(a);
		return V2{ v.x * c - v.y * s, v.x * s + v.y * c };
	};
	auto vdot = [](V2 a, V2 b) { return a.x * b.x + a.y * b.y; };
	D2D1_POINT_2F c1Sharp[16]{}, c2Sharp[16]{};
	bool hasC1[16]{}, hasC2[16]{};
	static const int tipSegs[] = { 1, 3, 5, 7, 9, 11, 13, 15 };
	for (int k : tipSegs) {
		D2D1_POINT_2F tip, prev, next;
		if (k == 15) {
			tip = D2D1::Point2F(182.424f, 1.334f);
			prev = D2D1::Point2F(segs[14].x, segs[14].y);
			next = D2D1::Point2F(segs[0].x, segs[0].y);
		}
		else {
			tip = D2D1::Point2F(segs[k].x, segs[k].y);
			prev = D2D1::Point2F(segs[k - 1].x, segs[k - 1].y);
			next = D2D1::Point2F(segs[k + 1].x, segs[k + 1].y);
		}
		const V2 up{ prev.x - tip.x, prev.y - tip.y };
		const V2 un{ next.x - tip.x, next.y - tip.y };
		const float lp = std::hypot(up.x, up.y), ln = std::hypot(un.x, un.y);
		if (lp < 1e-3f || ln < 1e-3f) continue;
		const V2 b = vnorm(V2{ up.x + un.x, up.y + un.y });
		const V2 a = vrot(b, 0.39269908f);    // +22.5°
		const V2 c = vrot(b, -0.39269908f);   // -22.5°
		const bool aIsPrev = vdot(a, up) > vdot(c, up);
		const V2 ePrev = aIsPrev ? a : c;
		const V2 eNext = aIsPrev ? c : a;
		const float L = 0.26f * std::min(lp, ln);
		c2Sharp[k] = D2D1::Point2F(tip.x + ePrev.x * L, tip.y + ePrev.y * L);
		hasC2[k] = true;
		const int outSeg = (k + 1) % 16;
		c1Sharp[outSeg] = D2D1::Point2F(tip.x + eNext.x * L, tip.y + eNext.y * L);
		hasC1[outSeg] = true;
	}
	ComPtr<ID2D1PathGeometry> path;
	if (FAILED(factory->CreatePathGeometry(path.GetAddressOf())) || !path) return nullptr;
	ComPtr<ID2D1GeometrySink> sink;
	if (FAILED(path->Open(sink.GetAddressOf())) || !sink) return nullptr;
	sink->SetFillMode(D2D1_FILL_MODE_WINDING);
	sink->BeginFigure(D2D1::Point2F(X(182.424f), Y(1.334f)), D2D1_FIGURE_BEGIN_FILLED);
	for (size_t i = 0; i < ARRAYSIZE(segs); ++i) {
		const auto& s = segs[i];
		const float startX = (i == 0) ? 182.424f : segs[i - 1].x;
		const float startY = (i == 0) ? 1.334f : segs[i - 1].y;
		float c1x = s.c1x, c1y = s.c1y, c2x = s.c2x, c2y = s.c2y;
		if (jittered) {
			// 每个弧：控制点绕"弦中点"伸缩 ±12%，再沿弦法线整体平移 ±3 SVG 单位。
			// 端点不参与，所以弧怎么变都还是接在原来的尖角上，不会多出毛刺。
			const float* j = &burstJitter_[i * 2];
			const float mx = (startX + s.x) * 0.5f, my = (startY + s.y) * 0.5f;
			const float k = 1.f + j[0] * 0.12f;
			c1x = mx + (c1x - mx) * k;
			c1y = my + (c1y - my) * k;
			c2x = mx + (c2x - mx) * k;
			c2y = my + (c2y - my) * k;
			const float dx = s.x - startX, dy = s.y - startY;
			const float len = std::hypot(dx, dy);
			if (len > 1e-3f) {
				const float t = j[1] * 3.f;
				const float nx = -dy / len * t, ny = dx / len * t;
				c1x += nx; c1y += ny;
				c2x += nx; c2y += ny;
			}
		}
		// 尖角处的切向覆盖抖动结果：保证每个爆裂的 8 个尖都是 45°，但弧身仍各不相同
		if (hasC1[i]) { c1x = c1Sharp[i].x; c1y = c1Sharp[i].y; }
		if (hasC2[i]) { c2x = c2Sharp[i].x; c2y = c2Sharp[i].y; }
		sink->AddBezier(D2D1::BezierSegment(
			D2D1::Point2F(X(c1x), Y(c1y)),
			D2D1::Point2F(X(c2x), Y(c2y)),
			D2D1::Point2F(X(s.x), Y(s.y))));
	}
	sink->EndFigure(D2D1_FIGURE_END_CLOSED);
	sink->Close();
	return path;
}

ComPtr<ID2D1PathGeometry> ShapeText::buildFrameGeometry() const
{
	if (burst_) return buildBurstGeometry();
	if (bubble_) return buildBubbleGeometry();
	return nullptr;
}

void ShapeText::updateFrameStyle()
{
	frameStyle_.Reset();
	if (!isFrame() || !win || !win->toolSub) return;
	auto factory = Ling::D2D::get()->d2dFactory.Get();
	if (!factory) return;
	const bool dashed = win->toolSub->isArrowDash;
	const bool roundCap = win->toolSub->isArrowRound;
	frameDash_ = dashed;
	frameRound_ = roundCap;
	const auto cap = roundCap ? D2D1_CAP_STYLE_ROUND : D2D1_CAP_STYLE_FLAT;
	// 气泡圆角连接；爆裂保留尖角所需的 miter
	const auto join = burst_ ? D2D1_LINE_JOIN_MITER : D2D1_LINE_JOIN_ROUND;
	const float sw = std::max(bubbleStroke_, 1.f);
	if (dashed) {
		const float gap = 6.f + (roundCap ? sw : 0.f);
		float dashes[] = { 8.f / sw, gap / sw };
		factory->CreateStrokeStyle(
			D2D1::StrokeStyleProperties(cap, cap, cap, join, 4.f,
				D2D1_DASH_STYLE_CUSTOM, 0.f),
			dashes, ARRAYSIZE(dashes), frameStyle_.GetAddressOf());
	}
	else {
		factory->CreateStrokeStyle(
			D2D1::StrokeStyleProperties(cap, cap, cap, join, 4.f),
			nullptr, 0, frameStyle_.GetAddressOf());
	}
}

// 爆裂命中：轮廓附近或内部都算（空框也要能单击选中 / 双击进编辑）
bool ShapeText::hitBurstShape(float x, float y, float band) const
{
	auto geo = buildBurstGeometry();
	if (!geo) return false;
	const D2D1_POINT_2F p{ x, y };
	BOOL inside = FALSE;
	if (SUCCEEDED(geo->FillContainsPoint(p, nullptr, &inside)) && inside) return true;
	inside = FALSE;
	return SUCCEEDED(geo->StrokeContainsPoint(p, band, nullptr, nullptr, &inside)) && inside;
}

// 气泡命中（描边）：点到圆角轮廓附近才算。用"从中心沿该方向到轮廓的距离"比较，
// 圆角和直边都统一由 LinkGeom 算，不会因为圆角大就把命中区算到框内空白里。
bool ShapeText::hitBubbleStroke(float x, float y, float band) const
{
	// 空气泡没有文字可命中，整块框都要能点选，否则删掉提示语以后就"点不中"了
	if (text.empty()
		&& x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom)
		return true;
	const float w = rect.right - rect.left;
	const float h = rect.bottom - rect.top;
	if (w < 1.f || h < 1.f) return false;
	const float hw = w * 0.5f, hh = h * 0.5f;
	const float cx = (rect.left + rect.right) * 0.5f;
	const float cy = (rect.top + rect.bottom) * 0.5f;
	const float rr = std::min(bubbleRadius_, std::min(hw, hh));
	const float radii[4]{ rr, rr, rr, rr };
	const float dx = x - cx, dy = y - cy;
	const float len = std::hypot(dx, dy);
	if (len < 1e-3f) return false;
	D2D1_POINT_2F p{};
	if (!LinkGeom::edgeOnRect(hw, hh, radii, std::atan2(dy, dx), p)) return false;
	return std::fabs(len - std::hypot(p.x, p.y)) <= band;
}

// 气泡命中（文字）：按文字**实际排版尺寸**判（不是整个气泡框），四周留一点余量
bool ShapeText::hitBubbleText(float x, float y) const
{
	if (!textLayout || text.empty()) return false;
	DWRITE_TEXT_METRICS m{};
	textLayout->GetMetrics(&m);
	if (m.widthIncludingTrailingWhitespace < 1.f || m.height < 1.f) return false;
	const float cx = (rect.left + rect.right) * 0.5f;
	const float cy = (rect.top + rect.bottom) * 0.5f;
	const float hw = m.widthIncludingTrailingWhitespace * 0.5f + 4.f * win->dpi;
	const float hh = m.height * 0.5f + 4.f * win->dpi;
	return std::fabs(x - cx) <= hw && std::fabs(y - cy) <= hh;
}

// 默认尾巴尖：底边偏右、朝下；用户拖过之后保持手动位置
void ShapeText::updateBubbleTail()
{
	if (!bubble_ || tailCustom_) return;
	const float w = rect.right - rect.left;
	const float h = rect.bottom - rect.top;
	const float hw = w * 0.5f, hh = h * 0.5f;
	const float rr = std::min(bubbleRadius_, std::min(hw, hh));
	const float cx = (rect.left + rect.right) * 0.5f;
	// 默认**朝下**：嘴落在底边的直段上（按"直段半长 hw-rr"取位，圆角再大也不会滑到右下角），
	// 尖端就在嘴的下方偏右一点点 —— 方向是往下，不是往右下角
	const float flat = std::max(0.f, hw - rr);
	bubbleTail_ = {
		cx + flat * 0.55f + 4.f * win->dpi,
		rect.bottom + 50.f * win->dpi
	};
}

void ShapeText::paint(ID2D1DeviceContext* ctx)
{
	if (isFrame() && win && win->toolSub
		&& (frameDash_ != win->toolSub->isArrowDash
			|| frameRound_ != win->toolSub->isArrowRound)) {
		updateFrameStyle();
	}
	if (isEditing) {
		// 编辑中的文字和插入符由形状直接绘制，键盘输入由宿主转发
		updateDraggers();
		paintPunch(ctx);   // 编辑中也要开洞，不然文字框里还压着线
		D2D1_MATRIX_3X2_F old{};
		ctx->GetTransform(&old);
		if (angle != 0.f) {
			const float cx = (rect.left + rect.right) * 0.5f;
			const float cy = (rect.top + rect.bottom) * 0.5f;
			ctx->SetTransform(D2D1::Matrix3x2F::Rotation(angle * 180.f / 3.14159265f, { cx, cy }) * old);
		}
		if (isFrame()) {
			auto geo = buildFrameGeometry();
			if (geo) ctx->DrawGeometry(geo.Get(), textBrush.Get(), bubbleStroke_, frameStyle_.Get());
		}
		if (textLayout) {
			DWRITE_TEXT_METRICS m{};
			textLayout->GetMetrics(&m);
			float tx = rect.left + borderPadding;
			float ty = rect.top + borderPadding;
			if (isFrame()) {
				tx = rect.left + ((rect.right - rect.left) - m.widthIncludingTrailingWhitespace) * 0.5f;
				ty = rect.top + ((rect.bottom - rect.top) - m.height) * 0.5f;
			}
			if (selectAll_ && !text.empty()) {
				ComPtr<ID2D1SolidColorBrush> selBrush;
				if (SUCCEEDED(ctx->CreateSolidColorBrush(color, selBrush.GetAddressOf())) && selBrush) {
					selBrush->SetOpacity(0.24f);
					const float pad = 1.f * win->dpi;
					ctx->FillRectangle(
						D2D1::RectF(tx - pad, ty, tx + m.widthIncludingTrailingWhitespace + pad, ty + m.height),
						selBrush.Get());
				}
			}
			if (!text.empty()) {
				ctx->DrawTextLayout({ tx, ty }, textLayout.Get(), textBrush.Get(),
					D2D1_DRAW_TEXT_OPTIONS_NONE);
			}
		}
		// 插入符与编辑命中共用 editCaretPos；编辑定时器每 500ms 触发重绘
		if (((GetTickCount64() / 500) & 1) == 0) {
			float lineH = 0.f;
			const auto p = editCaretPos(lineH);
			ctx->DrawLine(D2D1::Point2F(p.x, p.y), D2D1::Point2F(p.x, p.y + lineH),
				textBrush.Get(), std::max(1.f, 1.5f * win->dpi));
		}
		ctx->SetTransform(old);
		return;
	}
	if (!textLayout && !isFrame()) return;
	paintPunch(ctx);
	D2D1_MATRIX_3X2_F old{};
	ctx->GetTransform(&old);
	// 普通文字可以自己拖角度（四角外一圈旋转）；线上文字角度恒为 0（一律横排）
	if (angle != 0.f) {
		const float cx = (rect.left + rect.right) * 0.5f;
		const float cy = (rect.top + rect.bottom) * 0.5f;
		ctx->SetTransform(D2D1::Matrix3x2F::Rotation(angle * 180.f / 3.14159265f, { cx, cy }) * old);
	}
	if (isFrame()) {
		// 气泡/爆裂外框 + 文字居中
		auto geo = buildFrameGeometry();
		if (geo) ctx->DrawGeometry(geo.Get(), textBrush.Get(), bubbleStroke_, frameStyle_.Get());
		if (textLayout && !text.empty()) {
			DWRITE_TEXT_METRICS m{};
			textLayout->GetMetrics(&m);
			const float tx = rect.left + ((rect.right - rect.left) - m.widthIncludingTrailingWhitespace) * 0.5f;
			const float ty = rect.top + ((rect.bottom - rect.top) - m.height) * 0.5f;
			ctx->DrawTextLayout({ tx, ty }, textLayout.Get(), textBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
		}
		ctx->SetTransform(old);
		return;
	}
	if (!textLayout) { ctx->SetTransform(old); return; }
	ctx->DrawTextLayout({ rect.left + borderPadding, rect.top + borderPadding },
		textLayout.Get(), textBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
	ctx->SetTransform(old);
}

// 挂到箭头/线段上：只把文字**中心**摆在线段中点，**不跟线转**。
// 「线上文字一律横排」是用户定的：不管箭头是左→右、右→左还是垂直，文字方向都一样（永远
// 正着读，不会镜像/倒置）；要竖着排就自己在编辑框里按回车换行（一个字符一行）。
// 所以这里把 angle 归零 —— 不这么做的话，右→左的箭头会把文字转 180° 倒过来、垂直的箭头
// 会转 90° 变成"躺着读"
void ShapeText::setAttachPose(float cx, float cy)
{
	if (!win) return;
	angle = 0.f;
	attachCx = cx;
	attachCy = cy;
	float w = 0.f, h = 0.f;
	if (textLayout) {
		DWRITE_TEXT_METRICS m{};
		textLayout->GetMetrics(&m);
		w = m.widthIncludingTrailingWhitespace + borderPadding * 2.f;
		// 行距压掉的那点余量要补回来（最后一行的下伸在排版高度之外），
		// 不然洞会比字矮一截、线从那行的尾巴上压过去
		h = m.height + attachLineSlack + borderPadding * 2.f;
	}
	if (w < 1.f || h < 1.f) {   // 还没内容：洞 = 一个字 + 两侧间距，高度就是一行
		// （这里曾经借"一个字"跑一次真排版来量尺寸，还带上下限兜底 —— 现在直接按排版
		//  测量结果算洞的尺寸，插入符位置由排版自身决定）
		w = fontSize + borderPadding * 2.f;
		h = fontSize * 1.35f + borderPadding * 2.f;
	}
	rect = D2D1::RectF(cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f, cy + h * 0.5f);
	// 统一吸附到整像素：文字与洞在提交前后使用同一套浮点几何，避免亚像素偏移
	rect.left = std::floor(rect.left);
	rect.top = std::floor(rect.top);
	rect.right = std::floor(rect.right);
	rect.bottom = std::floor(rect.bottom);
	updateDraggers();
}

void ShapeText::paintPunch(ID2D1DeviceContext* ctx){
	// 「线段中间打字」：用底图把文字这一块抠回来 —— 线段/箭头从文字下穿过就会断成两截，
	// 正好是尺寸标注那种"文字压在线上但线不穿字"的样子。没开这个标记的文字不受影响。
	// 编辑中也要挖洞：现在编辑与提交共用同一套几何（rect = setAttachPose 的锚点居中几何），
	// 洞在编辑中就是提交后那个洞，不会再出现"编辑时挖歪"
	if (!punchBg || !win || !win->screenImg || !ctx) return;
	if (rect.right - rect.left < 1.f || rect.bottom - rect.top < 1.f) return;
	// 洞贴着字就行：别用 borderPadding（那是编辑框的留白），不然洞比字宽一圈
	const float pad = 2.f * (win ? win->dpi : 1.f);
	const D2D1_RECT_F r = D2D1::RectF(rect.left - pad, rect.top - pad, rect.right + pad, rect.bottom + pad);
	// 线上文字一律横排（angle==0），洞就是轴对齐的一块：多行文字（用户自己回车的竖列）
	// 也整块抠掉，线在整列文字的上下/左右各留 pad 的间距
	ctx->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);
	// 底图按自身像素尺寸铺（贴图缩放时客户区尺寸 ≠ 图像尺寸，用 w/h 会抠错位置）；
	// 尺寸对不上时仍按窗口尺寸铺，保持白板/演示的原行为
	const D2D1_SIZE_U srcPx = win->screenImg->GetPixelSize();
	const float sc = win->scale > 0.001f ? win->scale : 1.f;
	const bool sameSpace = std::fabs((float)srcPx.width - (float)win->w / sc) <= 2.f
		&& std::fabs((float)srcPx.height - (float)win->h / sc) <= 2.f;
	ctx->DrawBitmap(win->screenImg.Get(), sameSpace
		? D2D1::RectF(0.f, 0.f, (float)srcPx.width, (float)srcPx.height)
		: D2D1::RectF(0.f, 0.f, (float)win->w, (float)win->h));
	ctx->PopAxisAlignedClip();
}

void ShapeText::paintDragger(ID2D1DeviceContext* ctx)
{
	D2D1_MATRIX_3X2_F old{};
	ctx->GetTransform(&old);
	if (angle != 0.f) {
		const float cx = (rect.left + rect.right) * 0.5f;
		const float cy = (rect.top + rect.bottom) * 0.5f;
		ctx->SetTransform(D2D1::Matrix3x2F::Rotation(angle * 180.f / 3.14159265f, { cx, cy }) * old);
	}
	// 所有文字/气泡/爆裂都统一虚线选框（相对本体外扩 2px）
	ctx->DrawRectangle(selectionRect(rect), brushHandleBorder.Get(), win->dpi, selectionStroke());
	for (int i = 0; i < 4; i++) paintHandle(ctx, draggers[i]);
	if (bubble_) {
		// 气泡：和矩形一样 —— 只有四角 4 个控点 + 尾巴尖控点 + 四角内侧的圆角点；
		// 不再画四条边中点的控点（尺寸直接用角拖）
		const float hs = draggerSize * 0.5f;
		paintHandle(ctx, D2D1::RectF(bubbleTail_.x - hs, bubbleTail_.y - hs,
			bubbleTail_.x + hs, bubbleTail_.y + hs));
		// 圆角点：四角内侧各一个（拖动 = 统一调整四角圆角）
		const float inset = 14.f * win->dpi;
		for (int i = 10; i <= 13; i++)
			paintCornerDot(ctx, cornerDotPos(i, rect, inset), false);
	}
	ctx->SetTransform(old);
}

std::vector<float> ShapeText::editSnapshot() const
{
	return { rect.left, rect.top, rect.right, rect.bottom, fontSize, angle,
		bubbleRadius_, bubbleTail_.x, bubbleTail_.y };
}

void ShapeText::applyEditSnapshot(const std::vector<float>& s)
{
	if (s.size() < 9) return;
	rect = D2D1::RectF(s[0], s[1], s[2], s[3]);
	fontSize = s[4];
	angle = s[5];
	bubbleRadius_ = s[6];
	bubbleTail_ = D2D1::Point2F(s[7], s[8]);
	updateDraggers();
}

bool ShapeText::translateBy(float dx, float dy)
{
	// 挂在箭头/线段上的文字跟着线走，不单独平移
	if (attachTo) return false;
	rect = offsetRect(rect, dx, dy);
	if (tailCustom_) {
		bubbleTail_.x += dx;
		bubbleTail_.y += dy;
	}
	else {
		updateBubbleTail();
	}
	updateDraggers();
	return true;
}

void ShapeText::mouseDrag(const float x, const float y)
{
	if (bubble_ && hoverDraggerIndex >= 10 && hoverDraggerIndex <= 13) {
		// 圆角点：向内拖 = 加大圆角（四角统一）。位移先转回局部坐标，再投影到对角线方向
		float dx = x - dotPressX_, dy = y - dotPressY_;
		if (std::fabs(angle) > 1e-6f) {
			const float ca = std::cos(-angle), sa = std::sin(-angle);
			const float nx = dx * ca - dy * sa, ny = dx * sa + dy * ca;
			dx = nx;
			dy = ny;
		}
		const int slot = dotSlot(hoverDraggerIndex);
		const float sx = (slot == 0 || slot == 3) ? 1.f : -1.f;
		const float sy = (slot == 0 || slot == 1) ? 1.f : -1.f;
		const float inward = (dx * sx + dy * sy) * 0.70710678f;
		bubbleRadius_ = std::clamp(dotStartRadius_ + inward, 0.f, RoundBox::maxRadius(rect));
		win->refresh();
		return;
	}
	if (isFrame() && hoverDraggerIndex == 9) {
		// 生成中：拖出气泡/爆裂框。Shift = 标准比例（气泡 4:3，爆裂用 Vector 74 的 637:521）
		if (GetKeyState(VK_SHIFT) & 0x8000) {
			// 气泡标准比例横向 4:3（宽:高）；爆裂用 Vector 74 的 637:521
			const float aspect = burst_ ? (637.f / 521.f) : (4.f / 3.f);
			const float dx = x - pressX, dy = y - pressY;
			float w = std::fabs(dx), h = std::fabs(dy);
			if (w / aspect >= h) h = w / aspect;
			else w = h * aspect;
			const float x2 = pressX + (dx >= 0.f ? w : -w);
			const float y2 = pressY + (dy >= 0.f ? h : -h);
			rect = D2D1::RectF(
				std::min(pressX, x2), std::min(pressY, y2),
				std::max(pressX, x2), std::max(pressY, y2));
		}
		else {
			rect = D2D1::RectF(
				std::min(pressX, x), std::min(pressY, y),
				std::max(pressX, x), std::max(pressY, y));
		}
		if (bubble_) updateBubbleTail();
		updateDraggers();
		return;
	}
	if (bubble_ && hoverDraggerIndex == 5) {
		// 尾巴尖：拖到哪尖就到哪，自动贴最近的一条边
		bubbleTail_ = D2D1::Point2F(x, y);
		tailCustom_ = true;
		updateDraggers();
		return;
	}
	if (isFrame() && hoverDraggerIndex >= 0 && hoverDraggerIndex <= 3) {
		// 旋转后按世界坐标的固定对角/固定边缩放，指针投影到形状自身两条轴上；
		// 直接改 rect.left/right 会忽略角度，导致旋转后拖角整体漂移。
		rect = dragScale(x, y, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
		syncRectFromLayout();
		if (bubble_) updateBubbleTail();
		updateDraggers();
		return;
	}
	if (hoverDraggerIndex == 8) {
		const float dx = x - pressX, dy = y - pressY;
		rect.left += dx;
		rect.right += dx;
		rect.top += dy;
		rect.bottom += dy;
		bubbleTail_.x += dx;
		bubbleTail_.y += dy;
		pressX = x;
		pressY = y;
		updateDraggers();
		return;
	}
	if (hoverDraggerIndex >= 0 && hoverDraggerIndex <= 3) {
		const float cx = (resizeStartRect.left + resizeStartRect.right) * 0.5f;
		const float cy = (resizeStartRect.top + resizeStartRect.bottom) * 0.5f;
		const float dx0 = std::max(1.f, std::hypot(pressX - cx, pressY - cy));
		const float dx1 = std::max(1.f, std::hypot(x - cx, y - cy));
		const float minPx = 8.f * win->dpi;
		const float maxPx = 200.f * win->dpi;
		fontSize = std::clamp(resizeStartFont * (dx1 / dx0), minPx, maxPx);
		makeTextLayout();
		syncRectFromLayout();
		const float w = rect.right - rect.left;
		const float h = rect.bottom - rect.top;
		rect.left = cx - w * 0.5f;
		rect.right = cx + w * 0.5f;
		rect.top = cy - h * 0.5f;
		rect.bottom = cy + h * 0.5f;
		updateDraggers();
		// 拖角缩放时实时同步属性条数值
		if (win->toolSub) win->toolSub->setShapeSliderVal(L"text", fontSize);
		return;
	}
	if (hoverDraggerIndex == 4) {
		const float cx = (rect.left + rect.right) * 0.5f;
		const float cy = (rect.top + rect.bottom) * 0.5f;
		float next = rotateStartAngle + (std::atan2(y - cy, x - cx) - rotateStartAtan);
		if (GetKeyState(VK_SHIFT) & 0x8000) next = snapRotation8(next);
		angle = next;
		updateDraggers();
	}
}

void ShapeText::mouseDown(const float x, const float y)
{
	scaling_ = false;
	if (bubble_ && hoverDraggerIndex == 5) {
		// 尾巴尖控点
		pressX = x;
		pressY = y;
		return;
	}
	if (bubble_ && hoverDraggerIndex >= 10 && hoverDraggerIndex <= 13) {
		// 圆角点：记下起始半径/按下点，拖动时按增量算（按下不改变选中态）
		dotPressIndex_ = hoverDraggerIndex;
		dotStartRadius_ = bubbleRadius_;
		dotPressX_ = x;
		dotPressY_ = y;
		return;
	}
	if (hoverDraggerIndex == -1) {
		if (attachTo) {
			// 线上文字：点一下就是编辑（文字已经存在），不能按"落笔新文字"重置 rect
			startEdit();
			return;
		}
		if (isFrame()) {
			// 气泡/爆裂：和矩形一样拖拽生成（按下到松开就是框）
			pressX = x;
			pressY = y;
			rect = D2D1::RectF(x, y, x, y);
			updateDraggers();
			hoverDraggerIndex = 9;
			return;
		}
		rect = D2D1::RectF(x - borderPadding, y - borderPadding, x + borderPadding, y + borderPadding);
		hoverDraggerIndex = 9;
		startEdit();
		return;
	}
	if (hoverDraggerIndex == 9) {
		pressX = x;
		pressY = y;
		return;
	}
	if (hoverDraggerIndex == 8) {
		pressX = x;
		pressY = y;
		if (isEditing) finishEdit();
		return;
	}
	if (hoverDraggerIndex >= 0 && hoverDraggerIndex <= 3) {
		if (isEditing) finishEdit();
		pressX = x;
		pressY = y;
		resizeStartFont = fontSize;
		resizeStartRect = rect;
		if (isFrame()) {
			// 旋转后的缩放必须用世界锚点；scaleHandle_ 是 hitBoxEdgeOrCorner 的原始角序号
			beginScale(rect, angle, scaleHandle_, x, y);
			scaling_ = true;
		}
		return;
	}
	if (hoverDraggerIndex == 4) {
		if (isEditing) finishEdit();
		const float cx = (rect.left + rect.right) * 0.5f;
		const float cy = (rect.top + rect.bottom) * 0.5f;
		rotateStartAngle = angle;
		rotateStartAtan = std::atan2(y - cy, x - cx);
	}
}

void ShapeText::mouseUp(const float x, const float y)
{
	(void)x; (void)y;
	scaling_ = false;
	if (isFrame() && hoverDraggerIndex == 9) {
		// 生成结束：文字装不下时撑到最小尺寸；进入编辑延后到 annotUp 之后，
		// 保证新建形状先完成本帧绘制
		hoverDraggerIndex = -1;
		syncRectFromLayout();
		if (bubble_) updateBubbleTail();
		updateDraggers();
		win->requestEditText(this);
		return;
	}
	dotPressIndex_ = -1;
	updateDraggers();
}

void ShapeText::mouseWheel(const float /*x*/, const float /*y*/, const short delta)
{
	if (!isFrame()) return;
	// 二次选中气泡/爆裂：滚轮改描边粗细，并且同步回标注属性栏的线宽滑条
	auto next = bubbleStroke_ + (delta < 0 ? -win->dpi : win->dpi);
	auto applied = win->toolSub->setShapeSliderVal(L"arrow", next);
	if (applied == bubbleStroke_) return;
	bubbleStroke_ = applied;
	updateFrameStyle();
	win->refresh();
}

void ShapeText::mouseMove(const float x, const float y)
{
	hoverDraggerIndex = -1;
	updateDraggers();
	// 气泡尾巴尖：选中时优先命中（它在框外，比角控点更远）
	if (bubble_ && isSelected() && std::hypot(x - bubbleTail_.x, y - bubbleTail_.y) <= 8.f * win->dpi) {
		hoverDraggerIndex = 5;
		return;
	}
	// 四角外侧一圈 = 旋转（角上仍是缩放）；不再有单独旋转柄
	float lx = x, ly = y;
	if (std::fabs(angle) > 1e-6f) {
		const float cx = (rect.left + rect.right) * 0.5f;
		const float cy = (rect.top + rect.bottom) * 0.5f;
		const float dx = x - cx, dy = y - cy;
		const float ca = std::cos(-angle), sa = std::sin(-angle);
		lx = cx + dx * ca - dy * sa;
		ly = cy + dx * sa + dy * ca;
	}
	// 气泡的圆角点最优先（在四角内侧，不能被动角缩放/框内移动抢走）
	if (bubble_ && isSelected()) {
		const int dot = hitCornerDot(lx, ly, rect);
		if (dot >= 0) {
			hoverDraggerIndex = dot;
			return;
		}
	}
	// 四角优先；四边也可调（映射到对应角，缩放逻辑相同）。
	// 气泡只保留四角 4 个控点：边中点不缩放，落到下面的"框内移动"
	const float band = std::max(8.f * win->dpi, borderPadding);
	const int z = hitBoxEdgeOrCorner(lx, ly, rect, band);
	if (z >= 0 && !(isFrame() && (z & 1))) {
		scaleHandle_ = z;
		// Area 索引 → Text 四角：0=TL 2=TR 4=BR 6=BL → 0,1,3,2
		if (z == 0 || z == 1 || z == 7) hoverDraggerIndex = 0;
		else if (z == 2 || z == 3) hoverDraggerIndex = 1;
		else if (z == 4 || z == 5) hoverDraggerIndex = 3;
		else hoverDraggerIndex = 2;
		return;
	}
	// 控件命中之后再判旋转：角柄外半圈仍算缩放，旋转只在更外圈生效
	const int rotCorner = isSelected() ? hitRotateRing(lx, ly, rect) : -1;
	if (rotCorner >= 0) {
		hoverDraggerIndex = 4;
		rotateCorner = rotCorner;
		return;
	}
	// 气泡：只有描边和文字算命中 —— 中间空白不抢点击（能点到气泡下面的东西）
	if (isFrame()) {
		const float strokeBand = std::max(6.f * win->dpi, bubbleStroke_ * 0.5f + 4.f * win->dpi);
		const bool onFrame = hitBubbleText(lx, ly)
			|| (bubble_ ? hitBubbleStroke(lx, ly, strokeBand) : hitBurstShape(lx, ly, strokeBand));
		if (onFrame) {
			hoverDraggerIndex = isEditing ? 9 : 8;
			return;
		}
		if (isEditing) hoverDraggerIndex = 0;
		return;
	}
	const float half = borderPadding / 2.f + win->dpi;
	if (x >= rect.left - half && x <= rect.right + half && y >= rect.top - half && y <= rect.bottom + half) {
		// 内部：编辑中为输入，已选中为平移
		hoverDraggerIndex = isEditing ? 9 : 8;
		return;
	}
	if (isEditing) hoverDraggerIndex = 0;
}

void ShapeText::setCursor()
{
	// 文本工具在任何文字上（尤其是挂在线上那种）：始终工字型 ——
	// 否则已选中的文字会走移动/缩放光标（↔），二次编辑时鼠标就变了样
	if (win && win->getCurToolId() == L"text") {
		SetCursor(LoadCursor(nullptr, IDC_IBEAM));
		return;
	}
	if (applyUnselectedHoverCursor()) return;
	if (hoverDraggerIndex == 0 || hoverDraggerIndex == 3)
		SetCursor(LoadCursor(nullptr, IDC_SIZENWSE));
	else if (hoverDraggerIndex == 1 || hoverDraggerIndex == 2)
		SetCursor(LoadCursor(nullptr, IDC_SIZENESW));
	else if (hoverDraggerIndex >= 10 && hoverDraggerIndex <= 13) {
		const int slot = dotSlot(hoverDraggerIndex);
		SetCursor(LoadCursor(nullptr, (slot == 0 || slot == 2) ? IDC_SIZENWSE : IDC_SIZENESW));
	}
	else if (hoverDraggerIndex == 4)
		setRotateCursor(rotateCorner, angle);
	else if (hoverDraggerIndex == 8)
		SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
	else if (hoverDraggerIndex == 9)
		SetCursor(LoadCursor(nullptr, IDC_IBEAM));
	else
		SetCursor(LoadCursor(nullptr, IDC_ARROW));
}

bool ShapeText::hitErase(const float x, const float y)
{
	if (isFrame()) {
		// 气泡：描边和文字；爆裂：轮廓或内部
		float lx = x, ly = y;
		if (std::fabs(angle) > 1e-6f) {
			const float cx = (rect.left + rect.right) * 0.5f;
			const float cy = (rect.top + rect.bottom) * 0.5f;
			const float dx = x - cx, dy = y - cy;
			const float c = std::cos(-angle), s = std::sin(-angle);
			lx = cx + dx * c - dy * s;
			ly = cy + dx * s + dy * c;
		}
		const float band = std::max(6.f * win->dpi, bubbleStroke_ * 0.5f + 4.f * win->dpi);
		return hitBubbleText(lx, ly)
			|| (bubble_ ? hitBubbleStroke(lx, ly, band) : hitBurstShape(lx, ly, band));
	}
	return hitLabelBox(x, y);
}

D2D1_POINT_2F ShapeText::editCaretPos(float& lineH) const
{
	lineH = fontSize * 1.1f;
	float cx = 0.f, cy = 0.f;
	if (textLayout) {
		DWRITE_HIT_TEST_METRICS hm{};
		if (SUCCEEDED(textLayout->HitTestTextPosition(
			(UINT32)std::clamp(caretIndex_, 0, (int)text.size()), FALSE, &cx, &cy, &hm))) {
			if (hm.height > 0.5f) lineH = hm.height;
		}
	}
	if (isFrame()) {
		DWRITE_TEXT_METRICS m{};
		if (textLayout) textLayout->GetMetrics(&m);
		const float rw = rect.right - rect.left, rh = rect.bottom - rect.top;
		return D2D1::Point2F(
			rect.left + (rw - m.widthIncludingTrailingWhitespace) * 0.5f + cx,
			rect.top + (rh - m.height) * 0.5f + cy);
	}
	return D2D1::Point2F(rect.left + borderPadding + cx, rect.top + borderPadding + cy);
}

bool ShapeText::hitCaretArea(float x, float y) const
{
	float lx = x, ly = y;
	if (std::fabs(angle) > 1e-6f) {
		const float cx = (rect.left + rect.right) * 0.5f;
		const float cy = (rect.top + rect.bottom) * 0.5f;
		const float c = std::cos(-angle), s = std::sin(-angle);
		const float dx = x - cx, dy = y - cy;
		lx = cx + dx * c - dy * s;
		ly = cy + dx * s + dy * c;
	}
	float lineH = 0.f;
	const auto p = editCaretPos(lineH);
	const float pad = 4.f * win->dpi;
	return lx >= p.x - pad && lx <= p.x + pad
		&& ly >= p.y - pad && ly <= p.y + lineH + pad;
}

// 命中"这块文字"（含它周围一点边距）。线上文字一律横排（angle==0），普通文字可能是
// 用户自己转过角度的：那就先把点转回文字自己的坐标系再判，否则"两端点不到、左右误命中"
// （双击线上文字进编辑、橡皮擦擦文字都靠它）
bool ShapeText::hitLabelBox(const float x, const float y) const
{
	const float half = borderPadding / 2.f + (win ? win->dpi : 1.f);
	const float cx = (rect.left + rect.right) * 0.5f;
	const float cy = (rect.top + rect.bottom) * 0.5f;
	float lx = x - cx, ly = y - cy;
	if (std::fabs(angle) > 1e-6f) {
		// 逆变换（屏幕 → 文字自己的坐标系）：D2D 的 Rotation(θ) 是 [c -s; s c]（y 向下、顺时针为正）
		const float c = std::cos(angle), s = std::sin(angle);
		const float u = lx * c + ly * s;
		const float v = -lx * s + ly * c;
		lx = u;
		ly = v;
	}
	const float halfW = (rect.right - rect.left) * 0.5f + half;
	const float halfH = (rect.bottom - rect.top) * 0.5f + half;
	return std::fabs(lx) <= halfW && std::fabs(ly) <= halfH;
}

void ShapeText::initAttachedText(const std::wstring& t, float px)
{
	text = t.empty() ? L"请输入文字" : t;
	autoText_ = text;   // 初始内容就是自动值：没被用户改过时应继续跟随测量
	fontSize = px;
	makeTextLayout();
	syncRectFromLayout();
}

void ShapeText::setAutoText(const std::wstring& t)
{
	if (isEditing || t.empty()) return;
	// 只有"内容已经不是自动值"时才算用户改过：双击进编辑但没改字，仍然继续跟着测量刷新
	if (!autoText_.empty() && text != autoText_) return;
	autoText_ = t;
	if (t == text) return;
	text = t;
	makeTextLayout();
	syncRectFromLayout();
	win->refresh();
}

void ShapeText::startEdit(bool selectAll)
{
	if (isEditing) return;
	isEditing = true;
	selectAll_ = selectAll && !text.empty();
	caretIndex_ = (int)text.size();
	editStartText_ = text;
	editStartGeo_ = editSnapshot();
	// 文字与插入符由形状直接绘制，键盘由宿主 onChar/onKey 转发
	win->setEditingText(this);
	win->beginTextEdit();
	win->refresh();
}

void ShapeText::applyTextFromUndo(const std::wstring& t)
{
	text = t;
	caretIndex_ = (int)text.size();
	selectAll_ = false;
	isEditing = false;
	editStartText_.clear();
	editStartGeo_.clear();
	makeTextLayout();
	if (attachTo) setAttachPose(attachCx, attachCy);
	else syncRectFromLayout();
	win->refresh();
}

void ShapeText::insertText(const std::wstring& s)
{
	if (!isEditing || s.empty()) return;
	if (selectAll_) {
		text.clear();
		caretIndex_ = 0;
		selectAll_ = false;
	}
	caretIndex_ = std::clamp(caretIndex_, 0, (int)text.size());
	text.insert((size_t)caretIndex_, s);
	caretIndex_ += (int)s.size();
	makeTextLayout();
	if (attachTo) setAttachPose(attachCx, attachCy);
	else if (!isFrame()) syncRectFromLayout();   // 普通文字随内容长；气泡保持拖出的框
	win->refresh();
}

void ShapeText::backspace()
{
	if (!isEditing || text.empty()) return;
	if (selectAll_) {
		text.clear();
		caretIndex_ = 0;
		selectAll_ = false;
		makeTextLayout();
		if (attachTo) setAttachPose(attachCx, attachCy);
		else if (!isFrame()) syncRectFromLayout();
		win->refresh();
		return;
	}
	caretIndex_ = std::clamp(caretIndex_, 0, (int)text.size());
	if (caretIndex_ <= 0) return;
	text.erase((size_t)(caretIndex_ - 1), 1);
	--caretIndex_;
	makeTextLayout();
	if (attachTo) setAttachPose(attachCx, attachCy);
	else if (!isFrame()) syncRectFromLayout();
	win->refresh();
}

void ShapeText::deleteForward()
{
	if (!isEditing) return;
	if (selectAll_) {
		text.clear();
		caretIndex_ = 0;
		selectAll_ = false;
		makeTextLayout();
		if (attachTo) setAttachPose(attachCx, attachCy);
		else if (!isFrame()) syncRectFromLayout();
		win->refresh();
		return;
	}
	selectAll_ = false;
	caretIndex_ = std::clamp(caretIndex_, 0, (int)text.size());
	if (caretIndex_ >= (int)text.size()) return;
	text.erase((size_t)caretIndex_, 1);
	makeTextLayout();
	if (attachTo) setAttachPose(attachCx, attachCy);
	else if (!isFrame()) syncRectFromLayout();
	win->refresh();
}

void ShapeText::moveCaret(int delta)
{
	if (!isEditing) return;
	selectAll_ = false;
	caretIndex_ = std::clamp(caretIndex_ + delta, 0, (int)text.size());
	win->refresh();
}

void ShapeText::moveCaretTo(bool end)
{
	if (!isEditing) return;
	selectAll_ = false;
	caretIndex_ = end ? (int)text.size() : 0;
	win->refresh();
}

void ShapeText::finishEdit(bool suppressNextClick)
{
	if (!isEditing) return;
	isEditing = false;
	selectAll_ = false;
	caretIndex_ = std::clamp(caretIndex_, 0, (int)text.size());
	win->setEditingText(nullptr);
	win->endTextEdit();
	makeTextLayout();
	syncRectFromLayout();
	// 一次编辑会话（无论打了多少字）只记一步撤销；空普通文字会被删掉，不记
	if (win->history && text != editStartText_ && (!text.empty() || isFrame())) {
		win->history->addTextEditStep(this, editStartGeo_, editSnapshot(),
			editStartText_, text);
	}
	// 点空白结束后，吞掉紧随的点击，避免立刻又选中并画出控件；
	// 如果本次点击本来就要继续命中其它标注，则不能吞。
	if (suppressNextClick) win->annotSuppressUntil = GetTickCount64() + 400;
	win->shapeHover = nullptr;
	win->refresh();
	if (text.empty()) {
		if (isFrame()) {
			// 气泡/爆裂允许空文字：保持空框
			win->refresh();
			return;
		}
		Ling::App::get()->dq.TryEnqueue([w = win, self = this]() {
			w->history->removeShape(self);
		});
	}
}

void ShapeText::applyStyle()
{
	setAttr();
	// 气泡字号固定（与文字工具默认字号一致；标注滑条调的是线宽，不能拿来改字号）；线宽跟着标注滑条
	if (isFrame()) {
		fontSize = 24.f * win->dpi;
		if (win->getCurToolId() == L"arrow")
			bubbleStroke_ = std::max(1.f, win->toolSub->getSliderVal());
		updateFrameStyle();
	}
	if (isEditing) {
		// 编辑中：样式变化后重排并重绘
		makeTextLayout();
		if (attachTo) setAttachPose(attachCx, attachCy);
		else if (!isFrame()) syncRectFromLayout();
	}
	else {
		makeTextLayout();
		syncRectFromLayout();
	}
	win->refresh();
}

void ShapeText::makeTextLayout()
{
	textLayout = Ling::D2D::get()->makeTextLayout(text, fontSize);
	if (!textLayout) return;
	textLayout->SetFontWeight(isBold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL, { 0, INT_MAX });
	textLayout->SetFontStyle(isItalic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, { 0, INT_MAX });
	// 线上文字：行距压到"一个字" —— 用户是"自己回车换行来竖排"的（一个字符一行），
	// 用字体默认行高（约 1.35 字）会把字与字的上下间距拉得比横排时的左右间距宽。
	// 行距取**字体自己的 baseline（≈ 一个字）**：再小的话第一行的上伸就会顶出行框
	// （被内容区裁剪切掉）；取行度量必须**两段式** —— GetLineMetrics(&lm, 1, &n) 在
	// 多于一行的排版上会直接失败返回全 0，拿 0 当 baseline 传下去字会被顶到行框上方，
	// 导致打字时文字被裁、上下间距不一致
	if (attachTo) {
		attachLineStep = 0.f;
		attachLineSlack = 0.f;
		UINT32 lineCount = 0;
		textLayout->GetLineMetrics(nullptr, 0, &lineCount);
		if (lineCount == 0) lineCount = 1;
		std::vector<DWRITE_LINE_METRICS> lines(lineCount);
		UINT32 got = 0;
		if (SUCCEEDED(textLayout->GetLineMetrics(lines.data(), lineCount, &got)) && got > 0) {
			attachLineStep = lines[0].baseline;
			textLayout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, attachLineStep, lines[0].baseline);
			attachLineSlack = lines[0].height > attachLineStep ? lines[0].height - attachLineStep : 0.f;
		}
	}
}

void ShapeText::setAttr()
{
	auto toolSub = win->toolSub.get();
	colorValue = toolSub->getSelectedColorValue();
	color = toolSub->getSelectedColor();
	fontSize = toolSub->getSliderVal();
	isBold = toolSub->isTextBold;
	isItalic = toolSub->isTextItalic;
	Ling::D2D::get()->deviceContext->CreateSolidColorBrush(color, textBrush.ReleaseAndGetAddressOf());
}
