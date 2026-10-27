#include "pch.h"
#include "Win/AnnotHost.h"
#include "ShapeArea.h"
#include "RoundBox.h"
#include <cmath>

namespace {
	constexpr float kPiA = 3.14159265358979323846f;
}
ShapeArea::ShapeArea(AnnotHost* win) :ShapeBase(win), draggers(8, D2D1::RectF(0,0,0,0))
{
}

D2D1_POINT_2F ShapeArea::center() const
{
	return ShapeBase::boxCenter(rect);
}

void ShapeArea::toLocal(float x, float y, float& lx, float& ly) const
{
	ShapeBase::boxToLocal(rect, angle, x, y, lx, ly);
}

void ShapeArea::withRotation(ID2D1DeviceContext* ctx, const std::function<void()>& draw)
{
	ShapeBase::withBoxRotation(ctx, rect, angle, draw);
}

Microsoft::WRL::ComPtr<ID2D1Geometry> ShapeArea::makeAreaGeometry(bool asEllipse) const
{
	auto factory = Ling::D2D::get()->d2dFactory.Get();
	Microsoft::WRL::ComPtr<ID2D1Geometry> base;
	if (asEllipse) {
		Microsoft::WRL::ComPtr<ID2D1EllipseGeometry> eg;
		factory->CreateEllipseGeometry(
			D2D1::Ellipse(center(),
				std::max(0.5f, (rect.right - rect.left) * 0.5f),
				std::max(0.5f, (rect.bottom - rect.top) * 0.5f)),
			eg.GetAddressOf());
		base = eg;
	}
	else if (!RoundBox::isSquare(radius)) {
		// 四角独立圆角：自己描路径
		base = RoundBox::build(factory, rect, radius);
		if (!base) return {};
	}
	else {
		Microsoft::WRL::ComPtr<ID2D1RectangleGeometry> rg;
		factory->CreateRectangleGeometry(rect, rg.GetAddressOf());
		base = rg;
	}
	if (std::fabs(angle) < 1e-6f) return base;
	Microsoft::WRL::ComPtr<ID2D1TransformedGeometry> tg;
	const auto c = center();
	factory->CreateTransformedGeometry(base.Get(),
		D2D1::Matrix3x2F::Rotation(angle * 180.f / kPiA, c), tg.GetAddressOf());
	return tg;
}

void ShapeArea::paintDragger(ID2D1DeviceContext* ctx)
{
	// 与 ShapeRect 同一套：选框 + 四角（无边中点、无单独旋转柄）
	withRotation(ctx, [&] {
		ctx->DrawRectangle(selectionRect(rect), brushHandleBorder.Get(), win->dpi, selectionStroke());
		static const int corners[] = { 0, 2, 4, 6 };
		for (int i : corners) paintHandle(ctx, draggers[i]);
		if (cornerDotsEnabled()) {
			// 圆角点：四角内侧各一个
			const float inset = 14.f * win->dpi;
			static const int dotCorners[4]{ 0, 2, 4, 6 };
			for (int i = 10; i <= 13; i++) {
				const bool active = (dotCorner == dotCorners[i - 10]);
				paintCornerDot(ctx, cornerDotPos(i, rect, inset), active);
			}
		}
	});
}

void ShapeArea::updateDraggers()
{
	ShapeBase::layoutBoxDraggers(draggers, selectionRect(rect), draggerSize * 0.5f);
}

int ShapeArea::hitResizeZone(float lx, float ly) const
{
	return hitBoxEdgeOrCorner(lx, ly, rect, std::max(8.f * win->dpi, strokeHitPad()));
}

bool ShapeArea::translateBy(float dx, float dy)
{
	rect = offsetRect(rect, dx, dy);
	updateDraggers();
	// 马赛克/模糊要按新位置重新采样；高亮等是空实现
	onGeometryChanged();
	onGeometrySettled();
	return true;
}

float ShapeArea::strokeHitPad() const
{
	return 6.f * win->dpi;
}

void ShapeArea::mouseDrag(const float x, const float y)
{
	if (hoverDraggerIndex >= 10 && hoverDraggerIndex <= 13) {
		// 圆角点：向内拖 = 加大圆角。位移先转回局部坐标，再投影到"指向框内"的对角线方向。
		float dx = x - dotPressX, dy = y - dotPressY;
		if (std::fabs(angle) > 1e-6f) {
			const float ca = std::cos(-angle), sa = std::sin(-angle);
			const float nx = dx * ca - dy * sa, ny = dx * sa + dy * ca;
			dx = nx; dy = ny;
		}
		const int slot = dotSlot(hoverDraggerIndex);
		const float sx = (slot == 0 || slot == 3) ? 1.f : -1.f;
		const float sy = (slot == 0 || slot == 1) ? 1.f : -1.f;
		const float inward = (dx * sx + dy * sy) * 0.70710678f;
		const float maxR = RoundBox::maxRadius(rect);
		if (dotDragAll) {
			float base = 0.f;
			for (float r : dotStart) base = std::max(base, r);
			const float v = std::clamp(base + inward, 0.f, maxR);
			for (auto& r : radius) r = v;
		}
		else {
			// 已单击选中该点 → 只调这一个角；上限按"其余三角固定"算，可以到整条短边
			const float maxOne = RoundBox::maxRadiusFor(rect, dotStart, slot);
			radius[slot] = std::clamp(dotStart[slot] + inward, 0.f, maxOne);
		}
		onGeometryChanged();
		win->refresh();
		return;
	}
	if (hoverDraggerIndex == 9) {
		// 角外一圈拖动 = 旋转：按增量转，按下时形状不会突然跳角；Shift 吸附到 8 个朝向
		const auto c = center();
		float next = rotateStartAngle + (std::atan2(y - c.y, x - c.x) - rotateStartAtan);
		if (GetKeyState(VK_SHIFT) & 0x8000) next = snapRotation8(next);
		angle = next;
		updateDraggers();
		onGeometryChanged();
		return;
	}
	onGeometryChanged();
	if (hoverDraggerIndex == 8) {
		const float w = rect.right - rect.left;
		const float h = rect.bottom - rect.top;
		rect.left = x - pressX;
		rect.top = y - pressY;
		rect.right = rect.left + w;
		rect.bottom = rect.top + h;
	}
	else {
		// 缩放：固定对边，Shift 等比
		rect = dragScale(x, y, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
	}
	updateDraggers();
}

void ShapeArea::mouseDown(const float x, const float y)
{
	if (hoverDraggerIndex == -1) {
		pressX = x;
		pressY = y;
		rect = D2D1::RectF(x, y, x, y);
		beginScale(rect, angle, -1, x, y);   // 新建：按下点就是固定角
		hoverDraggerIndex = 4;
		return;
	}
	if (hoverDraggerIndex >= 10 && hoverDraggerIndex <= 13) {
		// 按下本身不改变选中态：本次是"整体调"还是"单角调"，看该点此前是否已被单击选中
		dotPressIndex = hoverDraggerIndex;
		dotDragAll = (dotCorner != cornerFromDotSlot(dotSlot(hoverDraggerIndex)));
		for (int i = 0; i < 4; i++) dotStart[i] = radius[i];
		dotPressX = x;
		dotPressY = y;
		return;
	}
	if (hoverDraggerIndex == 9) {
		// 记录按下点方位与初始角度，拖动时按增量旋转
		const auto c = center();
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

void ShapeArea::mouseUp(const float x, const float y)
{
	// 圆角点上"单击不拖" = 选中/取消选中这个角；拖动（真的改了圆角）不动选中态
	if (dotPressIndex >= 10 && dotPressIndex <= 13) {
		if (std::hypot(x - dotPressX, y - dotPressY) < 3.f * win->dpi) {
			const int corner = cornerFromDotSlot(dotSlot(dotPressIndex));
			dotCorner = (dotCorner == corner) ? -1 : corner;
			win->refresh();
		}
		dotPressIndex = -1;
	}
	updateDraggers();
	onGeometrySettled();
}

void ShapeArea::mouseMove(const float x, const float y)
{
	hoverDraggerIndex = -1;
	if (!isSelected()) dotCorner = -1;
	float lx, ly;
	toLocal(x, y, lx, ly);
	if (cornerDotsEnabled() && isSelected()) {
		// 圆角点最优先（它在四角内侧，不能被角缩放/框内移动抢走）
		const int dot = hitCornerDot(lx, ly, rect);
		if (dot >= 0) {
			hoverDraggerIndex = dot;
			return;
		}
	}
	// 角/边控点优先，旋转环只吃控点以外的近角区域
	const int edge = hitResizeZone(lx, ly);
	if (edge >= 0) {
		hoverDraggerIndex = edge;
		return;
	}
	const int rotCorner = isSelected() ? hitRotateRing(lx, ly, rect) : -1;
	if (rotCorner >= 0) {
		hoverDraggerIndex = 9;
		rotateCorner = rotCorner; // 光标按这个角挑字形
		return;
	}
	const float half = strokeHitPad();
	if (lx >= rect.left - half && lx <= rect.right + half && ly >= rect.top - half && ly <= rect.bottom + half)
		hoverDraggerIndex = 8;
}

void ShapeArea::setCursor()
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

bool ShapeArea::hitErase(const float x, const float y)
{
	float lx, ly;
	toLocal(x, y, lx, ly);
	return lx >= rect.left && lx <= rect.right && ly >= rect.top && ly <= rect.bottom;
}
