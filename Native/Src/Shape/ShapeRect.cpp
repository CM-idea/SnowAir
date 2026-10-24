#include "pch.h"
#include "Win/AnnotHost.h"
#include "Tool/ToolMain.h"
#include "Tool/ToolSub.h"
#include "Tool/IconCodes.h"
#include "ShapeRect.h"
#include "RoundBox.h"
#include "LinkGeom.h"
#include <algorithm>
#include <cmath>

namespace {
	constexpr float kPi = 3.14159265358979323846f;
}

ShapeRect::ShapeRect(AnnotHost* win, int kind, int sides) :ShapeBase(win), draggers{
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
	kind_ = (kind == Polygon || kind == Star) ? kind : Rect;
	sides_ = std::clamp(sides, 3, 20);
	// 圆角按百分比（0–100）存：100 = 该形状能到的最大圆角，跟着形状尺寸走
	radiusPct_ = std::clamp(toolSub->cornerRadius, 0.f, 100.f);
	uniformPct_ = true;
	if (isDash) strokeStyle = makeDashStroke(strokeWidth);
}

ShapeRect::~ShapeRect()
{
}

void ShapeRect::setSides(int n)
{
	const int v = std::clamp(n, 3, 20);
	if (v == sides_) return;
	sides_ = v;
	win->refresh();
}

void ShapeRect::setCornerRadius(float pct)
{
	const float v = std::clamp(pct, 0.f, 100.f);
	if (std::fabs(v - radiusPct_) < 0.01f && uniformPct_) return;
	radiusPct_ = v;
	uniformPct_ = true;   // 滑条 = 四角统一；圆角点拖过的独立半径让位
	win->refresh();
}

// 百分比 → 矩形的像素半径：100% = 短边的一半（胶囊/椭圆状）
float ShapeRect::pctRadiusPx() const
{
	return std::clamp(radiusPct_, 0.f, 100.f) / 100.f * RoundBox::maxRadius(rect);
}

// 命中实际形状：直接用画出来的几何做"包含点"判定，不再按外接矩形算 ——
// 星形的凹口、圆角多边形被切掉的角，都不会再"点空白也选中"
bool ShapeRect::hitShapeLocal(float lx, float ly, bool includeInterior) const
{
	auto geo = buildShape();
	if (!geo) return false;
	const D2D1_POINT_2F p{ lx, ly };
	BOOL inside = FALSE;
	if (includeInterior
		&& SUCCEEDED(geo->FillContainsPoint(p, nullptr, &inside)) && inside)
		return true;
	// 描边骑在轮廓线上：命中带取半个线宽 + 4px
	const float pad = strokeWidth * 0.5f + 4.f * win->dpi;
	inside = FALSE;
	return SUCCEEDED(geo->StrokeContainsPoint(p, pad, nullptr, nullptr, &inside)) && inside;
}

Microsoft::WRL::ComPtr<ID2D1PathGeometry> ShapeRect::buildShape() const
{
	auto factory = Ling::D2D::get()->d2dFactory.Get();
	if (!hasCornerDots()) return buildPolyPath();
	if (!uniformPct_) return RoundBox::build(factory, rect, radius);
	const float rr = pctRadiusPx();
	const float r4[4]{ rr, rr, rr, rr };
	return RoundBox::build(factory, rect, r4);
}

// 多边形/星形：在内接椭圆上均匀取点；星形外顶点与内顶点交替（内半径 0.45）。
// 圆角 = 每个顶点按百分比倒圆（二次贝塞尔在相邻两条边的切点上过渡）
Microsoft::WRL::ComPtr<ID2D1PathGeometry> ShapeRect::buildPolyGeometry(
	const D2D1_RECT_F& r, int kind, int sides, float radiusPct)
{
	auto factory = Ling::D2D::get()->d2dFactory.Get();
	const float w = r.right - r.left;
	const float h = r.bottom - r.top;
	if (!factory || w < 1.f || h < 1.f) return nullptr;
	const float cx = (r.left + r.right) * 0.5f;
	const float cy = (r.top + r.bottom) * 0.5f;
	const float rx = w * 0.5f, ry = h * 0.5f;
	const int n = std::clamp(sides, 3, 20);
	const bool star = kind == Star;
	const int count = star ? n * 2 : n;
	const float inner = 0.45f;
	std::vector<D2D1_POINT_2F> v((size_t)count);
	for (int i = 0; i < count; i++) {
		const float t = -kPi * 0.5f + (float)i * 2.f * kPi / (float)count;   // 顶点朝上
		const float k = (star && (i & 1)) ? inner : 1.f;
		v[(size_t)i] = D2D1_POINT_2F{ cx + std::cos(t) * rx * k, cy + std::sin(t) * ry * k };
	}
	Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
	if (FAILED(factory->CreatePathGeometry(path.GetAddressOf())) || !path) return nullptr;
	Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
	if (FAILED(path->Open(sink.GetAddressOf())) || !sink) return nullptr;
	sink->SetFillMode(D2D1_FILL_MODE_WINDING);
	const float pct = std::clamp(radiusPct, 0.f, 100.f) / 100.f;   // 100% = 顶点能倒的最大圆角
	for (int i = 0; i < count; i++) {
		const auto cur = v[(size_t)i];
		const auto prev = v[(size_t)((i + count - 1) % count)];
		const auto next = v[(size_t)((i + 1) % count)];
		const float lenIn = std::hypot(cur.x - prev.x, cur.y - prev.y);
		const float lenOut = std::hypot(next.x - cur.x, next.y - cur.y);
		// 顶点两侧各留 t 的直线段，中间用二次贝塞尔倒圆；t 不超过相邻边的一半
		const float t = pct * 0.45f * std::min(lenIn, lenOut);
		D2D1_POINT_2F p0 = cur, p1 = cur;
		if (lenIn > 1e-3f) {
			p0 = { cur.x + (prev.x - cur.x) / lenIn * t, cur.y + (prev.y - cur.y) / lenIn * t };
		}
		if (lenOut > 1e-3f) {
			p1 = { cur.x + (next.x - cur.x) / lenOut * t, cur.y + (next.y - cur.y) / lenOut * t };
		}
		if (i == 0) sink->BeginFigure(p0, D2D1_FIGURE_BEGIN_FILLED);
		else sink->AddLine(p0);
		// 二次贝塞尔：控制点 = 顶点，终点 = 出边的切点（p1）；写反会变成"拐到顶点再折回来"
		if (t > 0.5f) sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(cur, p1));
		else sink->AddLine(cur);
	}
	sink->EndFigure(D2D1_FIGURE_END_CLOSED);
	sink->Close();
	return path;
}

Microsoft::WRL::ComPtr<ID2D1PathGeometry> ShapeRect::buildPolyPath() const
{
	return buildPolyGeometry(rect, kind_, sides_, radiusPct_);
}

// D2D CUSTOM dash 长度会再乘 strokeWidth，这里按目标像素反算，避免粗线几乎看不出虚线
Microsoft::WRL::ComPtr<ID2D1StrokeStyle> ShapeRect::makeDashStroke(float strokeWidth)
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
	if (kind_ != Rect) return hitShapeLocal(lx, ly, true);
	// 可见的**描边**也算落在形状上：描边骑在边缘线上，外半圈在几何之外，
	// 不补这点容差，贴着描边起笔就联不上。容差 = 描边半宽 + 1px。
	const float pad = strokeWidth * 0.5f + 1.f;
	const D2D1_RECT_F r{ rect.left - pad, rect.top - pad, rect.right + pad, rect.bottom + pad };
	return LinkGeom::insideRect(r, lx, ly);
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
		// 圆角点：四角内侧各一个（只有矩形有圆角）
		if (hasCornerDots()) {
			const float inset = 14.f * win->dpi;
			static const int dotCorners[4]{ 0, 2, 4, 6 };
			for (int i = 10; i <= 13; i++) {
				const bool active = (dotCorner == dotCorners[i - 10]);
				paintCornerDot(ctx, cornerDotPos(i, rect, inset), active);
			}
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
		// 之前是"百分比统一圆角"的话，先把当前值落成像素，拖起来才不会从 0 跳
		if (uniformPct_) {
			const float rr = pctRadiusPx();
			for (auto& r : radius) r = rr;
			uniformPct_ = false;
		}
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
	if (isSelected() && hasCornerDots()) {
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
	if (kind_ != Rect) {
		// 多边形/星形：按实际轮廓判 —— 描边附近一定命中；填充或已选中时内部也能拖
		if (hitShapeLocal(lx, ly, isFill || isSelected())) hoverDraggerIndex = 8;
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
	if (isFill && hasCornerDots()) {
		// 滚轮 = 整体调整（四角一起），与圆角点的"整体拖动"一致
		if (uniformPct_) {
			const float next = std::clamp(radiusPct_ + (delta < 0 ? -5.f : 5.f), 0.f, 100.f);
			if (next == radiusPct_) return;
			radiusPct_ = next;
			win->refresh();
			return;
		}
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
	if (kind_ != Rect) {
		// 擦除：描边 / 填充内部都算，空的地方（星形凹口、圆角切掉处）不算
		return hitShapeLocal(lx, ly, isFill);
	}
	const float half = strokeWidth * 0.5f + win->dpi;
	return lx >= rect.left - half && lx <= rect.right + half
		&& ly >= rect.top - half && ly <= rect.bottom + half;
}
