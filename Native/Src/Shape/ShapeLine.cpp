#include "pch.h"
#include "Win/AnnotHost.h"
#include "Tool/ToolMain.h"
#include "Tool/ToolSub.h"
#include "ShapeLine.h"
#include "ShapeLink.h"
#include <cmath>

using Microsoft::WRL::ComPtr;

ShapeLine::ShapeLine(AnnotHost* win) : ShapeBase(win)
{
	auto toolSub = win->toolSub.get();
	auto color = toolSub->getSelectedColor();
	if (toolSub->isLineTransparent) color.a = 0.5f;
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(color, brush.GetAddressOf());
	strokeWidth = toolSub->getSliderVal();
	isDash = toolSub->isArrowDash;
	isRound = toolSub->isArrowRound;
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

bool ShapeLine::toggleFreeEndpoint(float x, float y)
{
	if (!isSelected()) return false;
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
	const float minLen = std::max(24.f * win->dpi, strokeWidth * 6.f);
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
	auto path = ArrowParts::shaftPath(Ling::D2D::get()->d2dFactory.Get(), poly.pts);
	if (path) ctx->DrawGeometry(path.Get(), brush.Get(), strokeWidth, strokeStyle.Get());
}

void ShapeLine::paintDragger(ID2D1DeviceContext* ctx)
{
	const bool dragging = poly.drag != ArrowPolyState::DragKind::None;
	paintArrowPolyHandles(ctx, poly, win->dpi,
		brushHandleFill.Get(), brushHandleBorder.Get(), brushHandleShadow.Get(), dragging);
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
}

void ShapeLine::mouseDown(const float x, const float y)
{
	if (hoverDraggerIndex == -1 || poly.pts.size() < 2) {
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
	creating = false;
	poly.beginDrag(lastHit, x, y);
}

void ShapeLine::mouseUp(const float x, const float y)
{
	(void)x; (void)y;
	if (poly.drag == ArrowPolyState::DragKind::Break && poly.dragOrigin.size() == poly.pts.size()
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
	ensureMinLength();
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
	const bool allowVirtual = isSelected();
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
