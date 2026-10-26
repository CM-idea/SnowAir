#include "pch.h"
#include "Win/AnnotHost.h"
#include "Tool/ToolSub.h"
#include "ShapePen.h"
#include "FreehandStroke.h"
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace {
	constexpr float kPi = 3.14159265358979323846f;

	// 8 方向吸附门槛：|dy| <= |dx|*tan(22.5°) 视为水平，反之垂直，其余吸附到 45°
	constexpr float kShiftTan22_5 = 0.41421356f;

	// 把 (x,y) 相对锚点 (ax,ay) 约束到 水平 / 垂直 / 45° 这八个方向之一
	void constrainEightWay(float ax, float ay, float x, float y, float& ox, float& oy)
	{
		const float dx = x - ax, dy = y - ay;
		const float adx = std::fabs(dx), ady = std::fabs(dy);
		if (ady <= adx * kShiftTan22_5) { ox = ax + dx; oy = ay; }
		else if (adx <= ady * kShiftTan22_5) { ox = ax; oy = ay + dy; }
		else {
			const float span = std::max(adx, ady);
			ox = ax + (dx >= 0 ? span : -span);
			oy = ay + (dy >= 0 ? span : -span);
		}
	}
}

ShapePen::ShapePen(AnnotHost* win) : ShapeBase(win),
	draggers(8, D2D1::RectF(0, 0, 0, 0))
{
	auto toolSub = win->toolSub.get();
	strokeWidth = toolSub->getSliderVal();
	isDash = toolSub->isPenDash;
	isSoft = toolSub->isPenSoft;
	isFade = toolSub->isPenFade;
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(toolSub->getSelectedColor(), brush.GetAddressOf());
	if (isFade) {
		// 渐隐画笔：自带平滑与逐点寿命，不走软笔/虚线
		isSoft = false;
		isDash = false;
		laser.width = std::max(1.f, strokeWidth);
	}
	ensureStrokeStyle();
}

ShapePen::~ShapePen()
{
}

int64_t ShapePen::nowMs() const
{
	return (int64_t)GetTickCount64();
}

void ShapePen::ensureStrokeStyle()
{
	strokeStyle.Reset();
	if (!isDash) return;
	auto d2d = Ling::D2D::get();
	const auto cap = D2D1_CAP_STYLE_ROUND;
	const auto join = D2D1_LINE_JOIN_ROUND;
	const float sw = std::max(strokeWidth, 1.f);
	float dashes[] = { 8.f / sw, 6.f / sw };
	d2d->d2dFactory->CreateStrokeStyle(
		D2D1::StrokeStyleProperties(cap, cap, cap, join, 10.f, D2D1_DASH_STYLE_CUSTOM, 0.f),
		dashes, ARRAYSIZE(dashes), strokeStyle.GetAddressOf());
}

void ShapePen::rebuildDashSpine()
{
	path.Reset();
	pathFilled = false;
	if (pts.empty()) return;
	auto d2d = Ling::D2D::get();
	d2d->d2dFactory->CreatePathGeometry(path.GetAddressOf());
	ComPtr<ID2D1GeometrySink> sink;
	path->Open(sink.GetAddressOf());
	sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_HOLLOW);
	if (pts.size() == 2) {
		sink->AddLine(pts[1]);
	}
	else if (pts.size() > 2) {
		auto at = [&](int i) -> D2D1_POINT_2F {
			if (i < 0) return pts[0];
			if (i >= (int)pts.size()) return pts.back();
			return pts[i];
		};
		for (int i = 0; i < (int)pts.size() - 1; i++) {
			const auto p0 = at(i - 1);
			const auto p1 = at(i);
			const auto p2 = at(i + 1);
			const auto p3 = at(i + 2);
			const D2D1_POINT_2F c1{ p1.x + (p2.x - p0.x) / 6.f, p1.y + (p2.y - p0.y) / 6.f };
			const D2D1_POINT_2F c2{ p2.x - (p3.x - p1.x) / 6.f, p2.y - (p3.y - p1.y) / 6.f };
			sink->AddBezier(D2D1::BezierSegment(c1, c2, p2));
		}
	}
	sink->EndFigure(D2D1_FIGURE_END_OPEN);
	sink->Close();
}

void ShapePen::rebuildPath()
{
	path.Reset();
	pathFilled = false;
	if (pts.empty()) return;
	if (isDash) {
		rebuildDashSpine();
		return;
	}
	// 实线硬笔/软笔：填充轮廓
	auto d2d = Ling::D2D::get();
	path = FreehandStroke::outline(d2d->d2dFactory.Get(), pts, strokeWidth, isSoft, times);
	pathFilled = (path != nullptr);
}

void ShapePen::appendSample(float x, float y, bool force)
{
	// 渐隐画笔：点带时间戳按寿命收细，平滑在 LaserStroke 内做
	if (isFade) {
		if (force) return; // 抬笔不再补点
		LaserStroke::addPoint(laser, x, y, nowMs());
		return;
	}

	if (pts.empty()) {
		clock0 = nowMs();
		pts.push_back({ x, y });
		times.push_back(0);
		smoothX = x;
		smoothY = y;
		rebuildPath();
		return;
	}

	D2D1_POINT_2F sample{ x, y };
	// 软笔更密、少 EMA；硬笔/虚线抑抖
	if (!isSoft || isDash) {
		const float k = isDash ? 0.42f : 0.38f;
		smoothX = smoothX * (1.f - k) + x * k;
		smoothY = smoothY * (1.f - k) + y * k;
		if (!force) {
			sample.x = smoothX;
			sample.y = smoothY;
		}
	}

	const auto& last = pts.back();
	const float dx = sample.x - last.x, dy = sample.y - last.y;
	const float minDist = isSoft && !isDash ? 0.45f * win->dpi : 0.9f * win->dpi;
	if (!force && dx * dx + dy * dy < minDist * minDist) return;

	pts.push_back(sample);
	times.push_back(nowMs() - clock0);
	rebuildPath();
}

void ShapePen::resetShiftStraight()
{
	shiftStraight = false;
	shiftBaseCount = 0;
	shiftAnchor = { 0.f, 0.f };
}

// 按住 Shift 画标准线：只约束"按下 Shift 之后"画出来的那一段，不碰已经手绘出来的轨迹。
// 按下 Shift 那一刻记下"当时画到哪"（锚点）与"已经有多少个采样点"（基线），之后每帧把点截回
// 基线、再补一个约束到 8 方向的端点 —— 提交出来就是"一段手绘 + 一段标准直线"。
// 松手只清会话状态，点全都留着继续手绘（与 QT6 版 applyDrawConstraint 的分段语义一致）。
void ShapePen::applyShiftStraight(float x, float y)
{
	const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
	if (!shift) {
		resetShiftStraight();
		appendSample(x, y, false);
		return;
	}
	if (!shiftStraight) {
		shiftStraight = true;
		if (pts.empty()) {
			// 起笔就按着 Shift：锚点补成第一个点
			clock0 = nowMs();
			pts.push_back({ x, y });
			times.push_back(0);
		}
		shiftAnchor = pts.back();
		shiftBaseCount = pts.size();
	}
	float ox = x, oy = y;
	constrainEightWay(shiftAnchor.x, shiftAnchor.y, x, y, ox, oy);
	// 截回基线，只保留一个约束端点（每帧重算，不会越描越偏）
	if (pts.size() != shiftBaseCount) {
		pts.resize(shiftBaseCount);
		if (times.size() > shiftBaseCount) times.resize(shiftBaseCount);
	}
	pts.push_back({ ox, oy });
	times.push_back(nowMs() - clock0);
	smoothX = ox;   // 与后续自由手绘的 EMA 接上，松开 Shift 时不会跳
	smoothY = oy;
	rebuildPath();
}

void ShapePen::syncRectFromPts()
{
	if (pts.empty()) {
		rect = {};
		return;
	}
	float l = pts[0].x, t = pts[0].y, r = pts[0].x, b = pts[0].y;
	for (auto& p : pts) {
		l = std::min(l, p.x); t = std::min(t, p.y);
		r = std::max(r, p.x); b = std::max(b, p.y);
	}
	const float pad = strokeWidth * 0.5f + 4.f;
	rect = D2D1::RectF(l - pad, t - pad, r + pad, b + pad);
	if (rect.right - rect.left < 8.f) rect.right = rect.left + 8.f;
	if (rect.bottom - rect.top < 8.f) rect.bottom = rect.top + 8.f;
}

void ShapePen::scalePtsToRect(const D2D1_RECT_F& oldR, const D2D1_RECT_F& newR)
{
	const float ow = std::max(1.f, oldR.right - oldR.left);
	const float oh = std::max(1.f, oldR.bottom - oldR.top);
	const float nw = newR.right - newR.left;
	const float nh = newR.bottom - newR.top;
	for (auto& p : pts) {
		const float u = (p.x - oldR.left) / ow;
		const float v = (p.y - oldR.top) / oh;
		p.x = newR.left + u * nw;
		p.y = newR.top + v * nh;
	}
	rebuildPath();
}

D2D1_POINT_2F ShapePen::boxCenter() const
{
	return { (rect.left + rect.right) * 0.5f, (rect.top + rect.bottom) * 0.5f };
}

void ShapePen::toLocal(float x, float y, float& lx, float& ly) const
{
	if (std::fabs(boxAngle) < 1e-6f) {
		lx = x;
		ly = y;
		return;
	}
	const auto c = boxCenter();
	const float dx = x - c.x, dy = y - c.y;
	const float ca = std::cos(-boxAngle), sa = std::sin(-boxAngle);
	lx = c.x + dx * ca - dy * sa;
	ly = c.y + dx * sa + dy * ca;
}

void ShapePen::withRotation(ID2D1DeviceContext* ctx, const std::function<void()>& draw)
{
	if (std::fabs(boxAngle) < 1e-6f) {
		draw();
		return;
	}
	D2D1_MATRIX_3X2_F old{};
	ctx->GetTransform(&old);
	const auto c = boxCenter();
	ctx->SetTransform(D2D1::Matrix3x2F::Rotation(boxAngle * 180.f / kPi, c) * old);
	draw();
	ctx->SetTransform(old);
}

void ShapePen::updateDraggers()
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

void ShapePen::paint(ID2D1DeviceContext* ctx)
{
	// 渐隐画笔：每帧按当前时刻整条填充，粗细已经随点龄收细
	if (isFade) {
		if (fadeDone) return;
		withRotation(ctx, [&] {
			LaserStroke::paint(ctx, brush.Get(), laser, nowMs());
		});
		return;
	}
	if (pts.empty() || fadeDone) return;
	withRotation(ctx, [&] {
		if (pts.size() == 1) {
			const float r = strokeWidth * 0.5f;
			ctx->FillEllipse(D2D1::Ellipse(pts[0], r, r), brush.Get());
			return;
		}
		if (!path) rebuildPath();
		if (!path) return;
		if (pathFilled)
			ctx->FillGeometry(path.Get(), brush.Get());
		else
			ctx->DrawGeometry(path.Get(), brush.Get(), strokeWidth, strokeStyle.Get());
	});
}

void ShapePen::paintDragger(ID2D1DeviceContext* ctx)
{
	if (isFade || !settled || pts.empty()) return;
	// 选框 + 四角；旋转改为「角外一圈悬停」，不再画独立旋转柄
	withRotation(ctx, [&] {
		ctx->DrawRectangle(
			D2D1::RectF(rect.left - 1.f, rect.top - 1.f, rect.right + 1.f, rect.bottom + 1.f),
			brushHandleBorder.Get(), win->dpi);
		static const int corners[] = { 0, 2, 4, 6 };
		for (int i : corners) paintHandle(ctx, draggers[i]);
	});
}

void ShapePen::mouseDown(const float x, const float y)
{
	if (!settled) {
		resetShiftStraight();
		if (isFade) {
			// 渐隐画笔：开一条新轨迹，并让宿主定时器开始按寿命刷新
			laser = {};
			laser.width = std::max(1.f, strokeWidth);
			LaserStroke::addPoint(laser, x, y, nowMs());
			win->requestFadeTick();
			hoverDraggerIndex = 0;
			return;
		}
		pts.clear();
		times.clear();
		clock0 = nowMs();
		smoothX = x;
		smoothY = y;
		pts.push_back({ x, y });
		times.push_back(0);
		rebuildPath();
		hoverDraggerIndex = 0;
		return;
	}
	if (hoverDraggerIndex == 9) {
		const auto c = boxCenter();
		rotateStartAngle = boxAngle;
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
	else if (hoverDraggerIndex == 2) { pressX = rect.left; pressY = rect.bottom; }
	else if (hoverDraggerIndex == 4) { pressX = rect.left; pressY = rect.top; }
	else if (hoverDraggerIndex == 6) { pressX = rect.right; pressY = rect.top; }
}

std::vector<float> ShapePen::editSnapshot() const
{
	if (isFade) return {};   // 渐隐画笔不参与撤销
	std::vector<float> s{ strokeWidth, boxAngle, rect.left, rect.top, rect.right, rect.bottom,
		(float)pts.size() };
	s.reserve(7 + pts.size() * 3);
	for (size_t i = 0; i < pts.size(); ++i) {
		s.push_back(pts[i].x);
		s.push_back(pts[i].y);
		s.push_back((float)(i < times.size() ? times[i] : 0));
	}
	return s;
}

void ShapePen::applyEditSnapshot(const std::vector<float>& s)
{
	if (s.size() < 7) return;
	strokeWidth = s[0];
	boxAngle = s[1];
	rect = D2D1::RectF(s[2], s[3], s[4], s[5]);
	const int n = std::max(0, (int)s[6]);
	pts.clear();
	times.clear();
	for (int i = 0; i < n; ++i) {
		const size_t base = 7 + (size_t)i * 3;
		if (base + 2 >= s.size()) break;
		pts.push_back({ s[base], s[base + 1] });
		times.push_back((int64_t)s[base + 2]);
	}
	rebuildPath();
	updateDraggers();
}

bool ShapePen::translateBy(float dx, float dy)
{
	rect = offsetRect(rect, dx, dy);
	// pts 与 rect 同一空间（未旋转局部坐标），一起平移
	for (auto& p : pts) {
		p.x += dx;
		p.y += dy;
	}
	rebuildPath();
	updateDraggers();
	return true;
}

void ShapePen::mouseDrag(const float x, const float y)
{
	if (!settled) {
		if (isFade) { appendSample(x, y, false); return; }  // 渐隐画笔不走 Shift 直线
		applyShiftStraight(x, y);
		return;
	}
	if (isFade) return; // 渐隐画笔落笔即弃控点，不做变换
	if (hoverDraggerIndex == 9) {
		// 角外一圈拖动 = 旋转：按增量转
		const auto c = boxCenter();
		boxAngle = rotateStartAngle + (std::atan2(y - c.y, x - c.x) - rotateStartAtan);
		updateDraggers();
		return;
	}
	if (hoverDraggerIndex == 8) {
		const float w = rect.right - rect.left;
		const float h = rect.bottom - rect.top;
		const float nl = x - pressX;
		const float nt = y - pressY;
		const float dx = nl - rect.left;
		const float dy = nt - rect.top;
		rect.left = nl;
		rect.top = nt;
		rect.right = nl + w;
		rect.bottom = nt + h;
		for (auto& p : pts) {
			p.x += dx;
			p.y += dy;
		}
		rebuildPath();
		updateDraggers();
		return;
	}
	if (hoverDraggerIndex == 0 || hoverDraggerIndex == 2
		|| hoverDraggerIndex == 4 || hoverDraggerIndex == 6) {
		float lx, ly;
		toLocal(x, y, lx, ly);
		const auto oldR = rect;
		auto [left, right] = std::minmax(pressX, lx);
		auto [top, bottom] = std::minmax(pressY, ly);
		rect = D2D1::RectF(left, top, right, bottom);
		if (rect.right - rect.left < 8.f) rect.right = rect.left + 8.f;
		if (rect.bottom - rect.top < 8.f) rect.bottom = rect.top + 8.f;
		scalePtsToRect(oldR, rect);
		updateDraggers();
	}
}

void ShapePen::mouseUp(const float x, const float y)
{
	if (!settled) {
		if (isFade) {
			// 抬笔后由定时器按寿命收细并移除
			settled = true;
			win->requestFadeTick();
			return;
		}
		// Shift 直线段收笔：再算一次约束端点（而不是把原始点直接补进去，免得末尾多一个折角）
		if (shiftStraight) applyShiftStraight(x, y);
		else appendSample(x, y, true);
		settled = true;
		resetShiftStraight();
		syncRectFromPts();
		updateDraggers();
		rebuildPath();
		return;
	}
	updateDraggers();
}

bool ShapePen::needsFadeTick() const
{
	// 渐隐画笔：落笔中（尾部持续收细）与抬笔后（整段收没）都要 tick
	if (!isFade || fadeDone) return false;
	return laser.pointCount() > 0;
}

bool ShapePen::tickFade()
{
	if (!isFade) return false;
	if (!settled) return false; // 落笔中不移除
	const int64_t now = nowMs();
	if (LaserStroke::visible(laser, now)) return false;
	fadeDone = true;
	return true;
}

void ShapePen::mouseMove(const float x, const float y)
{
	hoverDraggerIndex = -1;
	if (isFade) return; // 渐隐画笔不可点选/变换
	if (!settled) {
		if (hitErase(x, y)) hoverDraggerIndex = 8;
		return;
	}
	float lx, ly;
	toLocal(x, y, lx, ly);
	// 命中规则按"选中态"分开，和矩形/箭头保持一致：
	//  - 未选中：只有线迹本身可命中。包围盒内部不是画笔的"身体"——否则一条弯曲线会把整块
	//    矩形区域都吃掉，在下一次落笔时被误判成"拖动旧图形"，区域内再也画不了新东西。
	//  - 已选中：角外旋转 / 边框缩放 / 内部拖动，方便二次编辑。
	if (isSelected()) {
		// 四角外侧一圈 = 旋转（角上仍是缩放）；只有已选中的标注才给旋转区
		const int rotCorner = hitRotateRing(lx, ly, rect);
		if (rotCorner >= 0) {
			hoverDraggerIndex = 9;
			rotateCorner = rotCorner;
			return;
		}
		const int edge = hitBoxEdgeOrCorner(lx, ly, rect, std::max(8.f * win->dpi, 6.f * win->dpi));
		if (edge == 0 || edge == 2 || edge == 4 || edge == 6) {
			hoverDraggerIndex = edge;
			return;
		}
		if (lx >= rect.left && lx <= rect.right && ly >= rect.top && ly <= rect.bottom) {
			hoverDraggerIndex = 8;
			return;
		}
	}
	if (hitErase(x, y)) hoverDraggerIndex = 8;
}

void ShapePen::mouseWheel(const float x, const float y, const short delta)
{
	(void)x; (void)y;
	auto next = strokeWidth + (delta < 0 ? -win->dpi : win->dpi);
	auto applied = win->toolSub->setShapeSliderVal(L"pen", next);
	if (applied == strokeWidth) return;
	strokeWidth = applied;
	if (isFade) {
		laser.width = std::max(1.f, strokeWidth);
		win->refresh();
		return;
	}
	ensureStrokeStyle();
	rebuildPath();
	if (settled) {
		syncRectFromPts();
		updateDraggers();
	}
	win->refresh();
}

void ShapePen::setCursor()
{
	if (applyUnselectedHoverCursor()) return;
	if (!settled) {
		SetCursor(LoadCursor(nullptr, IDC_CROSS));
		return;
	}
	if (hoverDraggerIndex == 9) setRotateCursor(rotateCorner, boxAngle);
	else if (hoverDraggerIndex == 0 || hoverDraggerIndex == 4) SetCursor(LoadCursor(nullptr, IDC_SIZENWSE));
	else if (hoverDraggerIndex == 2 || hoverDraggerIndex == 6) SetCursor(LoadCursor(nullptr, IDC_SIZENESW));
	else if (hoverDraggerIndex == 8) SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
	else SetCursor(LoadCursor(nullptr, IDC_ARROW));
}

bool ShapePen::hitErase(const float x, const float y)
{
	if (isFade) return false; // 渐隐画笔不参与橡皮命中（laser 不进标注列表）
	if (pts.empty()) return false;
	float lx = x, ly = y;
	if (settled) toLocal(x, y, lx, ly);
	if (pts.size() == 1) {
		const float r = strokeWidth * 0.5f + 4.f;
		const float dx = lx - pts[0].x, dy = ly - pts[0].y;
		return dx * dx + dy * dy <= r * r;
	}
	if (!path) rebuildPath();
	if (!path) return false;
	BOOL contains = FALSE;
	if (pathFilled)
		path->FillContainsPoint({ lx, ly }, nullptr, &contains);
	else
		path->StrokeContainsPoint({ lx, ly }, strokeWidth + 4.f * win->dpi, strokeStyle.Get(), nullptr, &contains);
	return contains == TRUE;
}
