#include "pch.h"
#include "Win/AnnotHost.h"
#include "Tool/ToolSub.h"
#include "ShapePen.h"
#include "FreehandStroke.h"
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace {
	constexpr float kPi = 3.14159265358979323846f;
}

ShapePen::ShapePen(AnnotHost* win) : ShapeBase(win),
	draggers(8, D2D1::RectF(0, 0, 0, 0))
{
	auto toolSub = win->toolSub.get();
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(toolSub->getSelectedColor(), brush.GetAddressOf());
	strokeWidth = toolSub->getSliderVal();
	isDash = toolSub->isPenDash;
	isSoft = toolSub->isPenSoft;
	isFade = toolSub->isPenFade;
	if (isFade) {
		// 渐隐画笔：自带平滑/补点与逐点寿命，不走软笔/虚线
		isSoft = false;
		isDash = false;
		laser.width = std::max(1.f, strokeWidth);
		laser.unit = win->dpi;
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
	// 实线硬笔/软笔：对齐 QT FreehandStroke 填充轮廓
	auto d2d = Ling::D2D::get();
	path = FreehandStroke::outline(d2d->d2dFactory.Get(), pts, strokeWidth, isSoft, times);
	pathFilled = (path != nullptr);
}

void ShapePen::rebuildLaserPath(int64_t now)
{
	laserPath = LaserStroke::buildGeometry(Ling::D2D::get()->d2dFactory.Get(), laser, now);
}

void ShapePen::appendSample(float x, float y, bool force)
{
	// 渐隐画笔：点带时间戳按寿命收细，平滑/补点在 LaserStroke 内做
	if (isFade) {
		if (force) return; // 抬笔不再补点，与 QT closeTrail 一致
		const int64_t now = nowMs();
		LaserStroke::addPoint(laser, x, y, now);
		rebuildLaserPath(now);
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
	// QT：软笔更密、少 EMA；硬笔/虚线抑抖
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
	// 渐隐画笔：整段轮廓已经按点龄收细，直接填充
	if (isFade) {
		if (fadeDone) return;
		const int64_t now = nowMs();
		if (!laserPath) rebuildLaserPath(now);
		if (laserPath) withRotation(ctx, [&] { ctx->FillGeometry(laserPath.Get(), brush.Get()); });
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
		if (isFade) {
			// 渐隐画笔：开一条新轨迹，并让宿主定时器开始按寿命刷新
			laser = {};
			laser.width = std::max(1.f, strokeWidth);
			laser.unit = win->dpi;
			const int64_t now = nowMs();
			LaserStroke::addPoint(laser, x, y, now);
			rebuildLaserPath(now);
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

void ShapePen::mouseDrag(const float x, const float y)
{
	if (!settled) {
		appendSample(x, y, false);
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
			// 抬笔：稳定尾部、收起笔头圆，之后交给定时器一路收细到移除
			settled = true;
			const int64_t now = nowMs();
			LaserStroke::closeTrail(laser, now);
			rebuildLaserPath(now);
			win->requestFadeTick();
			return;
		}
		appendSample(x, y, true);
		settled = true;
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
	if (!settled) {
		// 绘制中不移除；只按当前时刻重建轮廓
		rebuildLaserPath(nowMs());
		return false;
	}
	const int64_t now = nowMs();
	if (LaserStroke::stillVisible(laser, now)) {
		rebuildLaserPath(now);
		return false;
	}
	fadeDone = true;
	laserPath.Reset();
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
	// 四角外侧一圈 = 旋转（角上仍是缩放）；只有已选中的标注才给旋转区
	const int rotCorner = isSelected() ? hitRotateRing(lx, ly, rect) : -1;
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
	if (lx >= rect.left && lx <= rect.right && ly >= rect.top && ly <= rect.bottom)
		hoverDraggerIndex = 8;
	else if (hitErase(x, y))
		hoverDraggerIndex = 8;
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
		rebuildLaserPath(nowMs());
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
	if (isFade) return false; // 渐隐画笔不参与橡皮命中（对齐 QT：laser 不进标注列表）
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
