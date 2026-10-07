#include "pch.h"
#include "Win/AnnotHost.h"
#include "Tool/ToolMain.h"
#include "Tool/ToolSub.h"
#include "Tool/IconCodes.h"
#include "ShapeRect.h"
#include "RoundBox.h"
#include "LinkGeom.h"
#include <cmath>

namespace {
	constexpr float kPi = 3.14159265358979323846f;

	// D2D CUSTOM dash 长度会再乘 strokeWidth，这里按目标像素反算，避免粗线几乎看不出虚线
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

ShapeRect::ShapeRect(AnnotHost* win) :ShapeBase(win), draggers{
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
	for (auto& r : radius) r = toolSub->getCornerRadiusPx();
	if (isDash) strokeStyle = makeDashStroke(strokeWidth);
}

ShapeRect::~ShapeRect()
{
}

Microsoft::WRL::ComPtr<ID2D1PathGeometry> ShapeRect::buildShape() const
{
	return RoundBox::build(Ling::D2D::get()->d2dFactory.Get(), rect, radius);
}

D2D1_POINT_2F ShapeRect::center() const
{
	return { (rect.left + rect.right) * 0.5f, (rect.top + rect.bottom) * 0.5f };
}

void ShapeRect::toLocal(float x, float y, float& lx, float& ly) const
{
	if (std::fabs(angle) < 1e-6f) {
		lx = x;
		ly = y;
		return;
	}
	const auto c = center();
	const float dx = x - c.x, dy = y - c.y;
	const float ca = std::cos(-angle), sa = std::sin(-angle);
	lx = c.x + dx * ca - dy * sa;
	ly = c.y + dx * sa + dy * ca;
}

void ShapeRect::withRotation(ID2D1DeviceContext* ctx, const std::function<void()>& draw)
{
	if (std::fabs(angle) < 1e-6f) {
		draw();
		return;
	}
	D2D1_MATRIX_3X2_F old{};
	ctx->GetTransform(&old);
	const auto c = center();
	ctx->SetTransform(D2D1::Matrix3x2F::Rotation(angle * 180.f / kPi, c) * old);
	draw();
	ctx->SetTransform(old);
}

// —— 连线（矩形这一侧）——
// 点转回局部坐标后判"在不在框里"：没填充的矩形内部也算"落在形状里"，
// 这样从形状里起笔就能连上（用户要的手感）
bool ShapeRect::hitLinkBody(const float x, const float y) const
{
	float lx = 0.f, ly = 0.f;
	toLocal(x, y, lx, ly);
	return LinkGeom::insideRect(rect, lx, ly);
}

D2D1_POINT_2F ShapeRect::linkCenter() const
{
	return center();
}

// 局部方向角 → 矩形边缘交点（四角圆角要贴着圆弧收），再转回世界坐标
bool ShapeRect::linkEdgePoint(const float localAngle, D2D1_POINT_2F& out) const
{
	const float hw = (rect.right - rect.left) * 0.5f;
	const float hh = (rect.bottom - rect.top) * 0.5f;
	D2D1_POINT_2F local{};
	if (!LinkGeom::edgeOnRect(hw, hh, radius, localAngle, local)) return false;
	out = LinkGeom::rotateAbout(center(), local, angle);
	return true;
}

void ShapeRect::updateDraggers()
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

void ShapeRect::paint(ID2D1DeviceContext* ctx)
{
	withRotation(ctx, [&] {
		// 四角可不同圆角，走自建路径（D2D 的 RoundedRect 只支持统一半径）
		auto geo = buildShape();
		if (!geo) return;
		if (isFill) ctx->FillGeometry(geo.Get(), brush.Get());
		else ctx->DrawGeometry(geo.Get(), brush.Get(), strokeWidth, strokeStyle.Get());
	});
}

void ShapeRect::paintDragger(ID2D1DeviceContext* ctx)
{
	// 选框 + 四角 + 圆角点；旋转改为「角外一圈悬停」，不再有独立旋转柄
	withRotation(ctx, [&] {
		ctx->DrawRectangle(
			D2D1::RectF(rect.left - 1.f, rect.top - 1.f, rect.right + 1.f, rect.bottom + 1.f),
			brushHandleBorder.Get(), win->dpi);
		static const int corners[] = { 0, 2, 4, 6 };
		for (int i : corners) paintHandle(ctx, draggers[i]);
		// Adobe AI 式圆角点：四角内侧各一个
		const float inset = 14.f * win->dpi;
		static const int dotCorners[4]{ 0, 2, 4, 6 };
		for (int i = 10; i <= 13; i++) {
			const bool active = (dotCorner == dotCorners[i - 10]);
			paintCornerDot(ctx, cornerDotPos(i, rect, inset), active);
		}
	});
}

void ShapeRect::mouseDrag(const float x, const float y)
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
			// 未选中该点 → 四角一起调（以四角最大值为基准，通常四角相等）
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
		// 圆角是"这个标注自己的"属性：不回写工具，新标注仍按默认/属性栏设置
		win->refresh();
		return;
	}
	if (hoverDraggerIndex == 9) {
		// 角外一圈拖动 = 旋转：按增量转，按下时形状不会突然跳角
		const auto c = center();
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
		updateDraggers();
		return;
	}
	float lx, ly;
	toLocal(x, y, lx, ly);
	if (hoverDraggerIndex == 0 || hoverDraggerIndex == 4 || hoverDraggerIndex == 2 || hoverDraggerIndex == 6) {
		auto [left, right] = std::minmax(pressX, lx);
		auto [top, bottom] = std::minmax(pressY, ly);
		rect.left = left;
		rect.right = right;
		rect.top = top;
		rect.bottom = bottom;
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
	updateDraggers();
}

void ShapeRect::mouseDown(const float x, const float y)
{
	if (hoverDraggerIndex == -1) {
		pressX = x;
		pressY = y;
		hoverDraggerIndex = 4;
		return;
	}
	if (hoverDraggerIndex == 9) {
		const auto c = center();
		rotateStartAngle = angle;
		rotateStartAtan = std::atan2(y - c.y, x - c.x);
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

void ShapeRect::mouseUp(const float x, const float y)
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
}

void ShapeRect::mouseMove(const float x, const float y)
{
	hoverDraggerIndex = -1;
	if (!isSelected()) dotCorner = -1; // 取消选中后，圆角点回到"整体调整"态
	float lx, ly;
	toLocal(x, y, lx, ly);
	// 圆角点最优先（它在四角内侧，不能被角缩放/框内移动抢走）
	if (isSelected()) {
		const int dot = hitCornerDot(lx, ly, rect);
		if (dot >= 0) {
			hoverDraggerIndex = dot;
			return;
		}
	}
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
	const bool inOuter = lx >= rect.left - half && lx <= rect.right + half
		&& ly >= rect.top - half && ly <= rect.bottom + half;
	if (!inOuter) return;
	// 填充：整块可点；描边：未选中只靠上面的边命中，选中后中空也可拖
	if (isFill || isSelected())
		hoverDraggerIndex = 8;
}

void ShapeRect::mouseWheel(const float x, const float y, const short delta)
{
	if (isFill) {
		// 滚轮 = 整体调整（四角一起），与圆角点的"整体拖动"一致
		float mx = 0.f;
		for (float r : radius) mx = std::max(mx, r);
		if (mx <= 0.f) return;
		const float step = win->dpi;
		const float next = std::clamp(mx + (delta < 0 ? -step : step), win->dpi, 100.f * win->dpi);
		if (next == mx) return;
		for (auto& r : radius) r = next;
		// 同上：滚轮也是只改这个标注
		win->refresh();
		return;
	}
	auto next = strokeWidth + (delta < 0 ? -win->dpi : win->dpi);
	auto applied = win->toolSub->setShapeSliderVal(L"rect", next);
	if (applied == strokeWidth) return;
	strokeWidth = applied;
	if (isDash) strokeStyle = makeDashStroke(strokeWidth);
	win->refresh();
}

void ShapeRect::setCursor()
{
	if (applyUnselectedHoverCursor()) return;
	if (hoverDraggerIndex == 9) {
		setRotateCursor(rotateCorner, angle);
	}
	else if (hoverDraggerIndex == 0 || hoverDraggerIndex == 4) {
		SetCursor(LoadCursor(nullptr, IDC_SIZENWSE));
	}
	else if (hoverDraggerIndex == 1 || hoverDraggerIndex == 5) {
		SetCursor(LoadCursor(nullptr, IDC_SIZENS));
	}
	else if (hoverDraggerIndex == 2 || hoverDraggerIndex == 6) {
		SetCursor(LoadCursor(nullptr, IDC_SIZENESW));
	}
	else if (hoverDraggerIndex == 3 || hoverDraggerIndex == 7) {
		SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
	}
	else if (hoverDraggerIndex == 8) {
		SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
	}
	else {
		SetCursor(LoadCursor(nullptr, IDC_ARROW));
	}
}

bool ShapeRect::hitErase(const float x, const float y)
{
	float lx, ly;
	toLocal(x, y, lx, ly);
	const float half = strokeWidth * 0.5f + win->dpi;
	return lx >= rect.left - half && lx <= rect.right + half
		&& ly >= rect.top - half && ly <= rect.bottom + half;
}
