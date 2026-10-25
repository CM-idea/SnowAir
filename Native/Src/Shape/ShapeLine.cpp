#include "pch.h"
#include "Win/AnnotHost.h"
#include "Tool/ToolMain.h"
#include "Tool/ToolSub.h"
#include "ShapeLine.h"
#include "ShapeLink.h"
#include "ShapeText.h"
#include "../History.h"
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace {
	constexpr float kPi = 3.14159265358979323846f;

	// 角度：屏幕坐标（y 向下）里「视觉逆时针」的扫角，0~2π
	float ccwSweepFromTo(float from, float to)
	{
		float d = from - to;
		while (d < 0.f) d += 2.f * kPi;
		while (d >= 2.f * kPi) d -= 2.f * kPi;
		return d;
	}
}

ShapeLine::ShapeLine(AnnotHost* win, bool highlighter, int kind) : ShapeBase(win)
{
	kind_ = kind;
	auto toolSub = win->toolSub.get();
	auto color = toolSub->getSelectedColor();
	if (highlighter) {
		// 荧光笔：与画笔共用滑条（显示 4 = 实际 20）+ 透明 + 平头 + 不虚线
		this->highlighter = true;
		color.a = std::max(0.1f, std::min(1.f, toolSub->penMarkerOpacity / 100.f));
		strokeWidth = toolSub->getSliderVal() * HighlighterWidthScale;
		isDash = false;
		isRound = false;
	}
	else {
		if (toolSub->isLineTransparent) color.a = 0.5f;
		strokeWidth = toolSub->getSliderVal();
		isDash = toolSub->isArrowDash;
		isRound = toolSub->isArrowRound;
	}
	// 角度：三个点（腿-顶点-腿）都要能自由拖动
	if (kind_ == Angle) {
		poly.freeStart = true;
		poly.freeEnd = true;
	}
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(color, brush.GetAddressOf());
	ensureStrokeStyle();
}

ShapeLine::~ShapeLine()
{
}

void ShapeLine::resetFree()
{
	poly.freeStart = false;
	poly.freeEnd = false;
	poly.breakHoverKey = -1;
}

// —— 连线（线段这一侧）：全部委托给 ShapeLink ——
bool ShapeLine::hasLink() const { return ShapeLink::hasLink(poly); }
// 拖动中提示点**只跟着起点**：见 ShapeArrow 同名实现
bool ShapeLine::linkStartPoint(D2D1_POINT_2F& out) const
{
	if (!poly.linkStart.shape || poly.pts.size() < 2) return false;
	out = poly.pts.front();
	return true;
}
bool ShapeLine::linkButtonPos(D2D1_POINT_2F& out) const { return ShapeLink::linkButtonPos(poly, out); }
bool ShapeLine::hitLinkButton(const float x, const float y, const float dpi) const
{
	return ShapeLink::hitLinkButton(poly, x, y, dpi);
}
void ShapeLine::syncLinks() { ShapeLink::sync(win, poly); }

// 折线长度中点（图像坐标）：文本工具悬停在线段中间时在那儿显示"可以打字"的空白
bool ShapeLine::shaftMidPoint(float& mx, float& my) const
{
	const auto& pts = poly.pts;
	if (kind_ == Angle && pts.size() >= 3) {
		// 角度的数字放在角的**内侧**（角平分线上、弧的外侧）——放到外侧会看不出是谁的角度。
		// 为了不压到两条边线：按文字框在"垂直于角平分线"方向的半投影 ÷ sin(半角) 算最小距离，
		// 夹角越窄离顶点越远（窄角时宁可放到线段尽头之外，也不横跨到边线上）。
		// 角度的文字本身不挖洞（见 AnnotHost::ensureShaftText），所以即使极端情况贴边也不会剪线。
		const auto v = pts[1], a = pts[0], c = pts[2];
		const float la = std::hypot(a.x - v.x, a.y - v.y);
		const float lc = std::hypot(c.x - v.x, c.y - v.y);
		if (la < 1.f || lc < 1.f) return false;
		float arcStart = 0.f;
		const float sweep = angleSweep(arcStart);
		const float arcR = std::min(0.45f * std::min(la, lc),
			std::max(30.f * win->dpi, 1.6f * angleLabelFontPx() + 1.5f * strokeWidth));
		const float am = arcStart - sweep * 0.5f;
		const float fpx = angleLabelFontPx();
		// 文字框尺寸（"30°"/"180°"）：宽度按字符数估，高度约 1.35 字，宁可估大
		const float boxW = fpx * 0.58f * (float)angleLabelText().size();
		const float boxH = fpx * 1.35f;
		const float halfW = boxW * 0.5f + 8.f * win->dpi;
		const float halfH = boxH * 0.5f + 8.f * win->dpi;
		// 文字框在"垂直于角平分线"方向上的半投影（轴对齐框的支撑函数）
		const float projPerp = halfW * std::fabs(std::sin(am)) + halfH * std::fabs(std::cos(am));
		// 文字框朝顶点方向的半投影：保证整块字都在弧的外侧
		const float projBack = halfW * std::fabs(std::cos(am)) + halfH * std::fabs(std::sin(am));
		// 离顶点的最小距离：文字框正好卡在两条边线之间（留 6px 空隙）。
		// 不做"最远距离"截断 —— 截断反而会把它按回边线上
		const float halfAngle = std::clamp(sweep * 0.5f, 0.05f, 1.5f);
		float offset = (projPerp + 6.f * win->dpi) / std::sin(halfAngle);
		offset = std::max(offset, arcR + projBack + 10.f * win->dpi + strokeWidth * 0.5f);
		mx = v.x + std::cos(am) * offset;
		my = v.y + std::sin(am) * offset;
		return true;
	}
	if (pts.size() < 2) return false;
	float total = 0.f;
	for (size_t i = 1; i < pts.size(); i++)
		total += std::hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y);
	if (total < 2.f) return false;
	const float half = total * 0.5f;
	float walk = 0.f;
	for (size_t i = 1; i < pts.size(); i++) {
		const float seg = std::hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y);
		if (walk + seg >= half) {
			const float t = seg > 0.f ? (half - walk) / seg : 0.f;
			mx = pts[i - 1].x + (pts[i].x - pts[i - 1].x) * t;
			my = pts[i - 1].y + (pts[i].y - pts[i - 1].y) * t;
			return true;
		}
		walk += seg;
	}
	mx = pts.back().x;
	my = pts.back().y;
	return true;
}

float ShapeLine::shaftAngle() const
{
	const auto& pts = poly.pts;
	if (pts.size() < 2) return 0.f;
	return std::atan2(pts.back().y - pts.front().y, pts.back().x - pts.front().x);
}

// 当前测量区间：起始边 + 扫角。
// 方向在第三点落下时定死（从较小夹角那一侧开始），之后一直沿这个方向累计：
// 越过 180° 继续涨到 360°，不会翻到另一侧"重来"。
float ShapeLine::angleSweep(float& start) const
{
	start = 0.f;
	if (poly.pts.size() < 3) return 0.f;
	const auto v = poly.pts[1], a = poly.pts[0], c = poly.pts[2];
	const float t1 = std::atan2(a.y - v.y, a.x - v.x);
	const float t2 = std::atan2(c.y - v.y, c.x - v.x);
	bool ccw = angleCcw_;
	if (!angleDirSet_) ccw = ccwSweepFromTo(t1, t2) <= kPi;   // 兜底：按较小夹角
	if (ccw) {
		start = t1;
		return ccwSweepFromTo(t1, t2);
	}
	start = t2;
	return ccwSweepFromTo(t2, t1);
}

// 角度测量：0~360°（允许张角本身，超过 180° 就是优角）
float ShapeLine::measuredAngleDeg() const
{
	if (poly.pts.size() < 3) return 0.f;
	const auto v = poly.pts[1], a = poly.pts[0], c = poly.pts[2];
	const float la = std::hypot(a.x - v.x, a.y - v.y);
	const float lc = std::hypot(c.x - v.x, c.y - v.y);
	if (la < 0.5f || lc < 0.5f) return 0.f;
	float arcStart = 0.f;
	return angleSweep(arcStart) * 180.f / kPi;
}

std::wstring ShapeLine::angleLabelText() const
{
	return std::to_wstring((int)std::lround(measuredAngleDeg())) + L"\u00B0";
}

// 把自动测量的角度写回挂在这条线上的文字（用户手动改过的不动）
void ShapeLine::updateAngleLabel()
{
	if (kind_ != Angle || !win || !win->history) return;
	const std::wstring label = angleLabelText();
	for (auto& s : win->history->shapes) {
		if (auto* t = dynamic_cast<ShapeText*>(s.get());
			t && t->attachTo == this && !s->isUndo) {
			t->setAutoText(label);
		}
	}
}

bool ShapeLine::toggleFreeEndpoint(float x, float y)
{
	if (!isSelected() || kind_ == Angle) return false;   // 角度固定三点，不切换自由端点
	if (!poly.toggleFreeAt(x, y, win->dpi)) return false;
	win->refresh();
	return true;
}

void ShapeLine::ensureStrokeStyle()
{
	// 线段就是箭头只有轴线的那一半：虚线挂轴线，cap/join 跟「圆头」
	strokeStyle = ArrowParts::makeShaftStyle(
		Ling::D2D::get()->d2dFactory.Get(), strokeWidth, isRound, isDash);
}

void ShapeLine::ensureMinLength()
{
	if (poly.pts.size() < 2) return;
	float length = 0.f;
	for (size_t i = 1; i < poly.pts.size(); i++)
		length += std::hypot(poly.pts[i].x - poly.pts[i - 1].x, poly.pts[i].y - poly.pts[i - 1].y);
	const float minLen = highlighter
		? std::max(8.f * win->dpi, strokeWidth * 2.f)
		: std::max(24.f * win->dpi, strokeWidth * 6.f);
	if (length >= minLen) return;
	auto& a = poly.pts.front();
	auto& b = poly.pts.back();
	const float dx = b.x - a.x, dy = b.y - a.y;
	const float cur = std::hypot(dx, dy);
	float ux = 1.f, uy = 0.f;
	if (cur >= 1.f) { ux = dx / cur; uy = dy / cur; }
	b.x = a.x + ux * minLen;
	b.y = a.y + uy * minLen;
}

void ShapeLine::paint(ID2D1DeviceContext* ctx)
{
	// 连线端点先贴回形状边缘（形状可能刚被拖动/缩放，甚至已经被删掉）
	ShapeLink::sync(win, poly);
	// 角度还在等第二个点：画一个点示意
	if (kind_ == Angle && poly.pts.size() == 2
		&& std::fabs(poly.pts[0].x - poly.pts[1].x) < 0.5f
		&& std::fabs(poly.pts[0].y - poly.pts[1].y) < 0.5f) {
		const float r = std::max(2.f, strokeWidth * 0.6f);
		ctx->FillEllipse(D2D1::Ellipse(poly.pts[0], r, r), brush.Get());
		return;
	}
	auto factory = Ling::D2D::get()->d2dFactory.Get();
	if (kind_ == Wave) {
		auto wave = buildWavePath();
		if (wave) ctx->DrawGeometry(wave.Get(), brush.Get(), strokeWidth, strokeStyle.Get());
		return;
	}
	auto path = ArrowParts::shaftPath(factory, poly.pts);
	if (path) ctx->DrawGeometry(path.Get(), brush.Get(), strokeWidth, strokeStyle.Get());
	if (kind_ == Angle) paintAngleArc(ctx);
}

// 波浪线：首尾两点之间铺一条正弦波，两端幅度收为 0（端点仍可拖）
ComPtr<ID2D1PathGeometry> ShapeLine::buildWavePath()
{
	if (poly.pts.size() < 2) return nullptr;
	const D2D1_POINT_2F a = poly.pts.front();
	const D2D1_POINT_2F b = poly.pts.back();
	const float dx = b.x - a.x, dy = b.y - a.y;
	const float len = std::hypot(dx, dy);
	if (len < 1.f) return nullptr;
	const float ux = dx / len, uy = dy / len;
	const float nx = -uy, ny = ux;
	const float pi = 3.14159265358979323846f;
	// 柔和但不至于太平：幅度略收于默认值，半波长拉长（波峰更圆润、起伏不锯齿）
	const float amp = std::min(strokeWidth * 1.2f, len * 0.13f);
	// 「全波浪」：按整数个半波铺满整段，起止都落在零相位（端点不动），中间始终是满幅
	const float wantHalf = std::max(24.f * win->dpi, strokeWidth * 4.5f);
	const int halfPeriods = std::max(1, (int)std::lround(len / wantHalf));
	// 采样密一点：小幅度波形靠折线也要看不出棱角
	const int steps = std::max(12, (int)(len / (1.5f * win->dpi)));
	ComPtr<ID2D1PathGeometry> path;
	Ling::D2D::get()->d2dFactory->CreatePathGeometry(path.GetAddressOf());
	if (!path) return nullptr;
	ComPtr<ID2D1GeometrySink> sink;
	path->Open(sink.GetAddressOf());
	sink->BeginFigure(a, D2D1_FIGURE_BEGIN_HOLLOW);
	for (int i = 1; i <= steps; ++i) {
		const float s = len * (float)i / (float)steps;
		const float off = amp * std::sin(pi * (float)halfPeriods * s / len);
		sink->AddLine(D2D1::Point2F(a.x + ux * s + nx * off, a.y + uy * s + ny * off));
	}
	sink->EndFigure(D2D1_FIGURE_END_OPEN);
	sink->Close();
	return path;
}

// 角度弧：沿当前测量方向从起始边画到另一条边（0~360° 都画得出来）
void ShapeLine::paintAngleArc(ID2D1DeviceContext* ctx)
{
	if (poly.pts.size() < 3) return;
	const D2D1_POINT_2F v = poly.pts[1];
	const D2D1_POINT_2F a = poly.pts[0];
	const D2D1_POINT_2F c = poly.pts[2];
	const float la = std::hypot(a.x - v.x, a.y - v.y);
	const float lc = std::hypot(c.x - v.x, c.y - v.y);
	if (la < 1.f || lc < 1.f) return;
	float arcStart = 0.f;
	const float sweep = angleSweep(arcStart);
	if (sweep < 1e-4f) return;
	// 自动半径：不超过两边的 45%，并按线宽/字号留出最小值
	const float r = std::min(0.45f * std::min(la, lc),
		std::max(30.f * win->dpi, 1.6f * angleLabelFontPx() + 1.5f * strokeWidth));
	if (r < 2.f) return;
	const int steps = std::max(8, (int)(sweep / (kPi / 36.f)));
	ComPtr<ID2D1PathGeometry> path;
	Ling::D2D::get()->d2dFactory->CreatePathGeometry(path.GetAddressOf());
	if (!path) return;
	ComPtr<ID2D1GeometrySink> sink;
	path->Open(sink.GetAddressOf());
	sink->BeginFigure(D2D1::Point2F(v.x + std::cos(arcStart) * r, v.y + std::sin(arcStart) * r),
		D2D1_FIGURE_BEGIN_HOLLOW);
	for (int i = 1; i <= steps; ++i) {
		const float t = arcStart - sweep * (float)i / (float)steps;   // y 向下：角度减小 = 视觉逆时针
		sink->AddLine(D2D1::Point2F(v.x + std::cos(t) * r, v.y + std::sin(t) * r));
	}
	sink->EndFigure(D2D1_FIGURE_END_OPEN);
	sink->Close();
	ctx->DrawGeometry(path.Get(), brush.Get(), std::max(1.f, strokeWidth), strokeStyle.Get());
}

void ShapeLine::paintDragger(ID2D1DeviceContext* ctx)
{
	const bool dragging = poly.drag != ArrowPolyState::DragKind::None;
	// 角度固定三个点、波浪线固定首尾两点：都不绘制“可加折点”的虚线中点
	paintArrowPolyHandles(ctx, poly, win->dpi,
		brushHandleFill.Get(), brushHandleBorder.Get(), brushHandleShadow.Get(), dragging,
		kind_ == Straight);
}

int ShapeLine::hoverFromHit(const ArrowHitInfo& h) const
{
	if (h.kind == ArrowHitInfo::Start) return 0;
	if (h.kind == ArrowHitInfo::End) return 1;
	if (h.kind == ArrowHitInfo::Break && h.insertAfter >= 0) return 100 + h.insertAfter;
	if (h.kind == ArrowHitInfo::Break) return 10 + std::max(0, h.pointIndex);
	if (h.kind == ArrowHitInfo::Move) return 8;
	return -1;
}

std::vector<float> ShapeLine::editSnapshot() const
{
	std::vector<float> s{ strokeWidth, (float)poly.pts.size() };
	s.reserve(2 + poly.pts.size() * 2);
	for (const auto& p : poly.pts) {
		s.push_back(p.x);
		s.push_back(p.y);
	}
	return s;
}

void ShapeLine::applyEditSnapshot(const std::vector<float>& s)
{
	if (s.size() < 2) return;
	strokeWidth = s[0];
	const int n = std::max(0, (int)s[1]);
	poly.pts.clear();
	for (int i = 0; i < n; ++i) {
		const size_t base = 2 + (size_t)i * 2;
		if (base + 1 >= s.size()) break;
		poly.pts.push_back({ s[base], s[base + 1] });
	}
}

void ShapeLine::mouseDrag(const float x, const float y)
{
	if (poly.drag == ArrowPolyState::DragKind::None) return;
	float mx = x, my = y;
	const bool shiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
	if (shiftDown && poly.dragFree
		&& (poly.drag == ArrowPolyState::DragKind::Start || poly.drag == ArrowPolyState::DragKind::End)
		&& poly.dragOrigin.size() >= 2) {
		const auto pivot = (poly.drag == ArrowPolyState::DragKind::Start)
			? poly.dragOrigin.back() : poly.dragOrigin.front();
		constrainToEightDirections(pivot.x, pivot.y, x, y, mx, my);
	}
	poly.dragTo(mx, my);
	// 连线：端点落进形状就吸附；已连上的端点拖不断，只在边缘上滑
	ShapeLink::dragEndpoint(win, poly, creating, mx, my);
	// 角度：测的是张角，两条边各画各的长度 —— 拖一条边不会改另一条边的长度
	if (kind_ == Angle && poly.pts.size() >= 3) {
		// 从顶点起笔拖出来的第三点：方向等边长大到能判断时再定，避免一上来就朝反方向累计
		if (!angleDirSet_) {
			const auto v = poly.pts[1], a = poly.pts[0], c = poly.pts[2];
			if (std::hypot(c.x - v.x, c.y - v.y) > 3.f * win->dpi
				&& std::hypot(a.x - v.x, a.y - v.y) > 3.f * win->dpi) {
				angleCcw_ = ccwSweepFromTo(std::atan2(a.y - v.y, a.x - v.x),
					std::atan2(c.y - v.y, c.x - v.x)) <= kPi;
				angleDirSet_ = true;
			}
		}
		updateAngleLabel();
	}
}

void ShapeLine::mouseDown(const float x, const float y)
{
	// 角度：三点式 —— 第一下点/拖出基线，第二下点/拖出第三点，完成后才走普通编辑
	if (kind_ == Angle && poly.pts.size() < 3) {
		if (!angleBaselineDone_) {
			if (!angleFirstSet_) {
				poly.setTwo(x, y, x, y);
				angleFirstSet_ = true;
			}
			else {
				poly.pts.resize(2);
				poly.pts[1] = { x, y };
			}
		}
		else {
			poly.pts.resize(2);
			poly.pts.push_back({ x, y });
			// 第三点落下：定下测量方向 —— 从较小夹角那一侧开始；之后沿这个方向连续累计，
			// 越过 180° 继续涨到 360°，不会翻到另一侧重来
			const auto& A = poly.pts[0];
			const auto& V = poly.pts[1];
			const auto& C = poly.pts[2];
			if (!angleDirSet_ && std::hypot(C.x - V.x, C.y - V.y) > 3.f * win->dpi) {
				angleCcw_ = ccwSweepFromTo(std::atan2(A.y - V.y, A.x - V.x),
					std::atan2(C.y - V.y, C.x - V.x)) <= kPi;
				angleDirSet_ = true;
			}
		}
		creating = true;
		lastHit.kind = ArrowHitInfo::End;
		lastHit.pointIndex = (int)poly.pts.size() - 1;
		lastHit.insertAfter = -1;
		hoverDraggerIndex = 1;
		poly.beginDrag(lastHit, x, y);
		poly.dragFree = true;
		return;
	}
	// 只有"还没成形"的线才允许按落笔点初始化。
	// 已成形、已选中的线被再点一下（hoverDraggerIndex == -1，比如点在角度的数字上、
	// 或者刚编辑完文字）时绝不能 setTwo —— 那会把整条线（角度的两条边）缩成一个点。
	if (poly.pts.size() < 2) {
		poly.setTwo(x, y, x, y);
		creating = true;
		// 落点落在矩形/椭圆里就从那儿起笔（端点吸附到形状边缘）
		ShapeLink::bindAtEndpoint(win, poly, true, x, y);
		lastHit.kind = ArrowHitInfo::End;
		lastHit.pointIndex = 1;
		lastHit.insertAfter = -1;
		hoverDraggerIndex = 1;
		poly.beginDrag(lastHit, x, y);
		poly.dragFree = true;
		return;
	}
	if (hoverDraggerIndex == -1) {
		// 按在线的空白侧/线上文字上：不动几何，交给"双击进文字编辑"
		creating = false;
		return;
	}
	creating = false;
	poly.beginDrag(lastHit, x, y);
}

void ShapeLine::mouseUp(const float x, const float y)
{
	(void)x; (void)y;
	if (kind_ == Straight && poly.drag == ArrowPolyState::DragKind::Break && poly.dragOrigin.size() == poly.pts.size()
		&& poly.dragPointIndex > 0 && poly.dragPointIndex + 1 < (int)poly.pts.size()) {
		const auto& a = poly.pts[poly.dragPointIndex - 1];
		const auto& b = poly.pts[poly.dragPointIndex];
		const auto& c = poly.pts[poly.dragPointIndex + 1];
		const float move = std::hypot(b.x - poly.dragOrigin[poly.dragPointIndex].x,
			b.y - poly.dragOrigin[poly.dragPointIndex].y);
		const float ab = std::hypot(b.x - a.x, b.y - a.y);
		const float bc = std::hypot(c.x - b.x, c.y - b.y);
		const float ac = std::hypot(c.x - a.x, c.y - a.y);
		if (move < 3.f * win->dpi && ab + bc <= ac + 1.5f * win->dpi)
			poly.pts.erase(poly.pts.begin() + poly.dragPointIndex);
	}
	poly.endDrag();
	creating = false;
	// 角度三点式：基线（两点）松开即成形；第三点落完就完成（文字由宿主挂上）
	if (kind_ == Angle) {
		if (!angleBaselineDone_) {
			if (poly.pts.size() >= 2) {
				const float d = std::hypot(poly.pts[1].x - poly.pts[0].x,
					poly.pts[1].y - poly.pts[0].y);
				if (d > 3.f * win->dpi) angleBaselineDone_ = true;
			}
		}
		else if (poly.pts.size() >= 3) {
			// 两条边保持各自画出来的长度（测角度，不做等长处理）
			updateAngleLabel();
		}
	}
	else {
		ensureMinLength();
	}
	ShapeLink::sync(win, poly);   // 收尾：端点再贴一次边缘（ensureMinLength 可能动过它）
}

bool ShapeLine::hitBody(float x, float y)
{
	return poly.hit(x, y, win->dpi, false, false).kind != ArrowHitInfo::None;
}

void ShapeLine::mouseMove(const float x, const float y)
{
	ShapeLink::sync(win, poly);   // 命中/悬停都按"贴回边缘之后"的端点算
	const bool dragging = poly.drag != ArrowPolyState::DragKind::None;
	// 角度只有三个点、波浪线只有首尾两点：不提供“线段中点加折点”的虚拟控点
	const bool allowVirtual = isSelected() && kind_ == Straight;
	lastHit = poly.hit(x, y, win->dpi, dragging, allowVirtual);
	if (lastHit.kind == ArrowHitInfo::Break && lastHit.insertAfter >= 0)
		poly.breakHoverKey = lastHit.insertAfter + 1000;
	else if (lastHit.kind == ArrowHitInfo::Break)
		poly.breakHoverKey = lastHit.pointIndex;
	else
		poly.breakHoverKey = -1;
	hoverDraggerIndex = hoverFromHit(lastHit);
}

void ShapeLine::mouseWheel(const float x, const float y, const short delta)
{
	(void)x; (void)y;
	auto next = strokeWidth + (delta < 0 ? -win->dpi : win->dpi);
	auto applied = win->toolSub->setShapeSliderVal(L"arrow", next);
	if (applied == strokeWidth) return;
	strokeWidth = applied;
	ensureStrokeStyle();
	win->refresh();
}

void ShapeLine::setCursor()
{
	if (applyUnselectedHoverCursor()) return;
	// 控点（端点/断点）：点按手型；杆身：移动
	if (hoverDraggerIndex == 0 || hoverDraggerIndex == 1 || hoverDraggerIndex >= 10)
		SetCursor(LoadCursor(nullptr, IDC_HAND));
	else if (hoverDraggerIndex == 8)
		SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
	else
		SetCursor(LoadCursor(nullptr, IDC_ARROW));
}

bool ShapeLine::hitErase(const float x, const float y)
{
	return hitBody(x, y);
}

void ShapeLine::constrainToEightDirections(const float anchorX, const float anchorY, const float mouseX, const float mouseY, float& targetX, float& targetY)
{
	float dx = mouseX - anchorX;
	float dy = mouseY - anchorY;
	float absX = fabsf(dx);
	float absY = fabsf(dy);
	if (absY <= absX * 0.41421356237f) {
		targetX = anchorX + dx;
		targetY = anchorY;
	}
	else if (absX <= absY * 0.41421356237f) {
		targetX = anchorX;
		targetY = anchorY + dy;
	}
	else {
		float span = absX > absY ? absX : absY;
		targetX = anchorX + (dx >= 0.f ? span : -span);
		targetY = anchorY + (dy >= 0.f ? span : -span);
	}
}
