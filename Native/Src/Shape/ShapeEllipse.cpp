#include "pch.h"
#include "Win/AnnotHost.h"
#include "Tool/ToolMain.h"
#include "Tool/ToolSub.h"
#include "ShapeEllipse.h"
#include "LinkGeom.h"
#include <cmath>

namespace {
	constexpr float kPiE = 3.14159265358979323846f;

	Microsoft::WRL::ComPtr<ID2D1StrokeStyle> makeDashStroke(float strokeWidth)
	{
		Microsoft::WRL::ComPtr<ID2D1StrokeStyle> style;
		const float sw = std::max(strokeWidth, 1.f);
		float dashes[] = { 10.f / sw, 8.f / sw };
		Ling::D2D::get()->d2dFactory->CreateStrokeStyle(
			D2D1::StrokeStyleProperties(
				D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
				D2D1_LINE_JOIN_MITER, 10.f, D2D1_DASH_STYLE_CUSTOM, 0.f),
			dashes, ARRAYSIZE(dashes), style.GetAddressOf());
		return style;
	}
}

ShapeEllipse::ShapeEllipse(AnnotHost* win) :ShapeBase(win), draggers{
	D2D1::RectF(0,0,0,0),
	D2D1::RectF(0,0,0,0),
	D2D1::RectF(0,0,0,0),
	D2D1::RectF(0,0,0,0),
	D2D1::RectF(0,0,0,0),
	D2D1::RectF(0,0,0,0),
	D2D1::RectF(0,0,0,0),
	D2D1::RectF(0,0,0,0) }
{
	auto toolSub = win->toolSub.get();
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(toolSub->getSelectedColor(), brush.GetAddressOf());
	strokeWidth = toolSub->getSliderVal();
	isFill = toolSub->isRectFill;
	isDash = toolSub->isRectDash && !isFill;
	if (isDash) strokeStyle = makeDashStroke(strokeWidth);
}

ShapeEllipse::~ShapeEllipse()
{
}

D2D1_POINT_2F ShapeEllipse::centerPt() const
{
	return { cx, cy };
}

void ShapeEllipse::syncEllipseFromRect()
{
	cx = (rect.left + rect.right) * 0.5f;
	cy = (rect.top + rect.bottom) * 0.5f;
	rx = (rect.right - rect.left) * 0.5f;
	ry = (rect.bottom - rect.top) * 0.5f;
}

void ShapeEllipse::toLocal(float x, float y, float& lx, float& ly) const
{
	if (std::fabs(angle) < 1e-6f) {
		lx = x;
		ly = y;
		return;
	}
	const float dx = x - cx, dy = y - cy;
	const float ca = std::cos(-angle), sa = std::sin(-angle);
	lx = cx + dx * ca - dy * sa;
	ly = cy + dx * sa + dy * ca;
}

void ShapeEllipse::withRotation(ID2D1DeviceContext* ctx, const std::function<void()>& draw)
{
	if (std::fabs(angle) < 1e-6f) {
		draw();
		return;
	}
	D2D1_MATRIX_3X2_F old{};
	ctx->GetTransform(&old);
	ctx->SetTransform(D2D1::Matrix3x2F::Rotation(angle * 180.f / kPiE, { cx, cy }) * old);
	draw();
	ctx->SetTransform(old);
}

// —— 连线（椭圆这一侧）——
// 点转回局部坐标后按椭圆方程判"在不在里面"
bool ShapeEllipse::hitLinkBody(const float x, const float y) const
{
	float lx = 0.f, ly = 0.f;
	toLocal(x, y, lx, ly);
	return LinkGeom::insideEllipse(centerPt(), rx, ry, lx, ly);
}

// 局部方向角 → 椭圆边缘交点，再转回世界坐标
bool ShapeEllipse::linkEdgePoint(const float localAngle, D2D1_POINT_2F& out) const
{
	D2D1_POINT_2F local{};
	if (!LinkGeom::edgeOnEllipse(rx, ry, localAngle, local)) return false;
	out = LinkGeom::rotateAbout(centerPt(), local, angle);
	return true;
}

void ShapeEllipse::updateDraggers()
{
	const float half = draggerSize * 0.5f;
	const float w = rect.right - rect.left;
	const float h = rect.bottom - rect.top;
	draggers[0] = D2D1::RectF(rect.left - half, rect.top - half, rect.left + half, rect.top + half);
	draggers[1] = D2D1::RectF(rect.left + w * 0.5f - half, rect.top - half, rect.left + w * 0.5f + half, rect.top + half);
	draggers[2] = D2D1::RectF(rect.right - half, rect.top - half, rect.right + half, rect.top + half);
	draggers[3] = D2D1::RectF(rect.right - half, rect.top + h * 0.5f - half, rect.right + half, rect.top + h * 0.5f + half);
	draggers[4] = D2D1::RectF(rect.right - half, rect.bottom - half, rect.right + half, rect.bottom + half);
	draggers[5] = D2D1::RectF(rect.left + w * 0.5f - half, rect.bottom - half, rect.left + w * 0.5f + half, rect.bottom + half);
	draggers[6] = D2D1::RectF(rect.left - half, rect.bottom - half, rect.left + half, rect.bottom + half);
	draggers[7] = D2D1::RectF(rect.left - half, rect.top + h * 0.5f - half, rect.left + half, rect.top + h * 0.5f + half);
}

void ShapeEllipse::paint(ID2D1DeviceContext* ctx)
{
	withRotation(ctx, [&] {
		D2D1_ELLIPSE ellipse = D2D1::Ellipse({ cx, cy }, rx, ry);
		if (isFill) ctx->FillEllipse(ellipse, brush.Get());
		else ctx->DrawEllipse(ellipse, brush.Get(), strokeWidth, strokeStyle.Get());
	});
}

void ShapeEllipse::paintDragger(ID2D1DeviceContext* ctx)
{
	// 选框 + 四角；旋转改为「角外一圈悬停」，不再画独立旋转柄
	withRotation(ctx, [&] {
		ctx->DrawRectangle(
			D2D1::RectF(rect.left - 1.f, rect.top - 1.f, rect.right + 1.f, rect.bottom + 1.f),
			brushHandleBorder.Get(), win->dpi);
		static const int corners[] = { 0, 2, 4, 6 };
		for (int i : corners) paintHandle(ctx, draggers[i]);
	});
}

void ShapeEllipse::mouseDrag(const float x, const float y)
{
	if (hoverDraggerIndex == 9) {
		// 角外一圈拖动 = 旋转：按增量转，按下时形状不会突然跳角
		const auto c = centerPt();
		angle = rotateStartAngle + (std::atan2(y - c.y, x - c.x) - rotateStartAtan);
		updateDraggers();
		return;
	}
	if (hoverDraggerIndex == 8) {
		const float w = rect.right - rect.left;
		const float h = rect.bottom - rect.top;
		rect.left = x - pressX;
		rect.top = y - pressY;
		rect.right = rect.left + w;
		rect.bottom = rect.top + h;
		syncEllipseFromRect();
		updateDraggers();
		return;
	}
	float lx, ly;
	toLocal(x, y, lx, ly);
	if (hoverDraggerIndex == 0 || hoverDraggerIndex == 4 || hoverDraggerIndex == 2 || hoverDraggerIndex == 6) {
		auto [left, right] = std::minmax(pressX, lx);
		auto [top, bottom] = std::minmax(pressY, ly);
		rect = D2D1::RectF(left, top, right, bottom);
	}
	else if (hoverDraggerIndex == 1 || hoverDraggerIndex == 5) {
		auto [top, bottom] = std::minmax(pressY, ly);
		rect.top = top;
		rect.bottom = bottom;
	}
	else if (hoverDraggerIndex == 3 || hoverDraggerIndex == 7) {
		auto [left, right] = std::minmax(pressX, lx);
		rect.left = left;
		rect.right = right;
	}
	const bool shiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
	if (hoverDraggerIndex != 8 && shiftDown) {
		const float w = rect.right - rect.left;
		const float h = rect.bottom - rect.top;
		if (w > h) rect.bottom = rect.top + w;
		else rect.right = rect.left + h;
	}
	syncEllipseFromRect();
	if (shiftDown) ry = rx;
	updateDraggers();
}

void ShapeEllipse::mouseDown(const float x, const float y)
{
	if (hoverDraggerIndex == -1) {
		pressX = x;
		pressY = y;
		hoverDraggerIndex = 0;
		return;
	}
	if (hoverDraggerIndex == 9) {
		const auto c = centerPt();
		rotateStartAngle = angle;
		rotateStartAtan = std::atan2(y - c.y, x - c.x);
		return;
	}
	if (hoverDraggerIndex == 8) {
		pressX = x - rect.left;
		pressY = y - rect.top;
		return;
	}
	float lx, ly;
	toLocal(x, y, lx, ly);
	if (hoverDraggerIndex == 0) { pressX = rect.right; pressY = rect.bottom; }
	else if (hoverDraggerIndex == 1) { pressY = rect.bottom; }
	else if (hoverDraggerIndex == 2) { pressX = rect.left; pressY = rect.bottom; }
	else if (hoverDraggerIndex == 3) { pressX = rect.left; }
	else if (hoverDraggerIndex == 4) { pressX = rect.left; pressY = rect.top; }
	else if (hoverDraggerIndex == 5) { pressY = rect.top; }
	else if (hoverDraggerIndex == 6) { pressX = rect.right; pressY = rect.top; }
	else if (hoverDraggerIndex == 7) { pressX = rect.right; }
}

void ShapeEllipse::mouseUp(const float x, const float y)
{
	updateDraggers();
}

void ShapeEllipse::mouseMove(const float x, const float y)
{
	hoverDraggerIndex = -1;
	float lx, ly;
	toLocal(x, y, lx, ly);
	// 四角外侧一圈 = 旋转（角上仍是缩放）；只有已选中的标注才给旋转区
	const int rotCorner = isSelected() ? hitRotateRing(lx, ly, rect) : -1;
	if (rotCorner >= 0) {
		hoverDraggerIndex = 9;
		rotateCorner = rotCorner;
		return;
	}
	const float band = std::max(8.f * win->dpi, strokeWidth * 0.5f + 4.f * win->dpi);
	const int edge = hitBoxEdgeOrCorner(lx, ly, rect, band);
	if (edge >= 0) {
		hoverDraggerIndex = edge;
		return;
	}
	const float half = strokeWidth * 0.5f + 4.f * win->dpi;
	const float dx = lx - cx, dy = ly - cy;
	const float outerRx = rx + half, outerRy = ry + half;
	if (outerRx <= 0 || outerRy <= 0) return;
	const bool insideOuter = (dx / outerRx) * (dx / outerRx) + (dy / outerRy) * (dy / outerRy) <= 1.f;
	if (!insideOuter) return;
	if (isFill || isSelected()) {
		// 填充整块可点；选中后中空也可拖
		hoverDraggerIndex = 8;
		return;
	}
	// 描边未选中：只命中描边环（外圈内、内圈外）
	const float irx = rx - half, iry = ry - half;
	if (irx <= 0.f || iry <= 0.f) {
		hoverDraggerIndex = 8;
		return;
	}
	const bool insideInner = (dx / irx) * (dx / irx) + (dy / iry) * (dy / iry) <= 1.f;
	if (!insideInner) hoverDraggerIndex = 8;
}

void ShapeEllipse::mouseWheel(const float x, const float y, const short delta)
{
	if (isFill) return;
	auto next = strokeWidth + (delta < 0 ? -win->dpi : win->dpi);
	auto applied = win->toolSub->setShapeSliderVal(L"rect", next);
	if (applied == strokeWidth) return;
	strokeWidth = applied;
	if (isDash) strokeStyle = makeDashStroke(strokeWidth);
	win->refresh();
}

void ShapeEllipse::setCursor()
{
	if (applyUnselectedHoverCursor()) return;
	if (hoverDraggerIndex == 9) setRotateCursor(rotateCorner, angle);
	else if (hoverDraggerIndex == 0 || hoverDraggerIndex == 4) SetCursor(LoadCursor(nullptr, IDC_SIZENWSE));
	else if (hoverDraggerIndex == 1 || hoverDraggerIndex == 5) SetCursor(LoadCursor(nullptr, IDC_SIZENS));
	else if (hoverDraggerIndex == 2 || hoverDraggerIndex == 6) SetCursor(LoadCursor(nullptr, IDC_SIZENESW));
	else if (hoverDraggerIndex == 3 || hoverDraggerIndex == 7) SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
	else if (hoverDraggerIndex == 8) SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
	else SetCursor(LoadCursor(nullptr, IDC_ARROW));
}

bool ShapeEllipse::hitErase(const float x, const float y)
{
	float lx, ly;
	toLocal(x, y, lx, ly);
	const float half = strokeWidth * 0.5f + win->dpi;
	const float dx = lx - cx, dy = ly - cy;
	const float orx = rx + half, ory = ry + half;
	if (orx <= 0 || ory <= 0) return false;
	return (dx / orx) * (dx / orx) + (dy / ory) * (dy / ory) <= 1.f;
}
