#include "pch.h"
#include "Win/AnnotHost.h"
#include "Tool/ToolSub.h"
#include "ShapeEllipse.h"
#include "LinkGeom.h"
#include <cmath>

namespace {
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

// 二次选中后修改样式时，与矩形共用同一套填充 / 虚线 / 线宽逻辑。
void ShapeEllipse::applyStyle()
{
	auto* ts = win->toolSub.get();
	if (!ts) return;
	strokeWidth = std::max(1.f, ts->getSliderVal());
	isFill = ts->isRectFill;
	isDash = ts->isRectDash && !isFill;
	strokeStyle = isDash ? makeDashStroke(strokeWidth) : nullptr;
	brush->SetColor(ts->getSelectedColor());
	win->refresh();
}

// 反向同步：把本形状的属性写回工具状态，供重新打开该标注的工具时展示。
void ShapeEllipse::syncToolFromShape() const
{
	auto* ts = win->toolSub.get();
	if (!ts) return;
	ts->isEllipse = true;
	ts->isRectFill = isFill;
	ts->isRectDash = isDash;
	ts->setShapeSliderVal(L"rect", strokeWidth);
}

UINT32 ShapeEllipse::styleColorValue() const
{
	D2D1_COLOR_F c{};
	if (brush) c = brush->GetColor();
	return ShapeBase::packColor(c);
}

D2D1_POINT_2F ShapeEllipse::centerPt() const
{
	return ShapeBase::boxCenter(rect);
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
	ShapeBase::boxToLocal(rect, angle, x, y, lx, ly);
}

void ShapeEllipse::withRotation(ID2D1DeviceContext* ctx, const std::function<void()>& draw)
{
	ShapeBase::withBoxRotation(ctx, rect, angle, draw);
}

// —— 连线（椭圆这一侧）——
// 点转回局部坐标后按椭圆方程判"在不在里面"
bool ShapeEllipse::hitLinkBody(const float x, const float y) const
{
	float lx = 0.f, ly = 0.f;
	toLocal(x, y, lx, ly);
	// 同矩形：可见描边（外半圈）也算落在形状上，容差 = 描边半宽 + 1px
	const float pad = strokeWidth * 0.5f + 1.f;
	return LinkGeom::insideEllipse(centerPt(), rx + pad, ry + pad, lx, ly);
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
	ShapeBase::layoutBoxDraggers(draggers, selectionRect(rect), draggerSize * 0.5f);
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
		ctx->DrawRectangle(selectionRect(rect), brushHandleBorder.Get(), win->dpi, selectionStroke());
		static const int corners[] = { 0, 2, 4, 6 };
		for (int i : corners) paintHandle(ctx, draggers[i]);
	});
}

std::vector<float> ShapeEllipse::editSnapshot() const
{
	return { rect.left, rect.top, rect.right, rect.bottom, angle, strokeWidth };
}

void ShapeEllipse::applyEditSnapshot(const std::vector<float>& s)
{
	if (s.size() < 5) return;
	rect = D2D1::RectF(s[0], s[1], s[2], s[3]);
	angle = s[4];
	if (s.size() > 5) strokeWidth = s[5];
	syncEllipseFromRect();
	updateDraggers();
}

bool ShapeEllipse::translateBy(float dx, float dy)
{
	rect = offsetRect(rect, dx, dy);
	syncEllipseFromRect();
	updateDraggers();
	return true;
}

void ShapeEllipse::mouseDrag(const float x, const float y)
{
	if (hoverDraggerIndex == 9) {
		// 角外一圈拖动 = 旋转：按增量转，按下时形状不会突然跳角；Shift 吸附到 8 个朝向
		const auto c = centerPt();
		float next = rotateStartAngle + (std::atan2(y - c.y, x - c.x) - rotateStartAtan);
		if (GetKeyState(VK_SHIFT) & 0x8000) next = snapRotation8(next);
		angle = next;
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
	// 缩放：固定对边，Shift 等比（正方形/正圆也由同一套锚点算出来，不再事后改 ry）
	rect = dragScale(x, y, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
	syncEllipseFromRect();
	updateDraggers();
}

void ShapeEllipse::mouseDown(const float x, const float y)
{
	if (hoverDraggerIndex == -1) {
		beginScale(rect, angle, -1, x, y);   // 新建：按下点就是固定角
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
	beginScale(rect, angle, hoverDraggerIndex, x, y);
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
	// 角/边控点优先，旋转环只吃控点以外的区域，避免角柄外半圈误触发旋转
	const float band = std::max(8.f * win->dpi, strokeWidth * 0.5f + 4.f * win->dpi);
	const int edge = hitBoxEdgeOrCorner(lx, ly, rect, band);
	if (edge >= 0) {
		hoverDraggerIndex = edge;
		return;
	}
	const int rotCorner = isSelected() ? hitRotateRing(lx, ly, rect) : -1;
	if (rotCorner >= 0) {
		hoverDraggerIndex = 9;
		rotateCorner = rotCorner;
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
