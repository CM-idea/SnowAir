#include "pch.h"
#include "ArrowPoly.h"
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace {
	constexpr float kPi = 3.14159265358979323846f;
}

void ArrowPolyState::setTwo(float x0, float y0, float x1, float y1)
{
	pts = { D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1) };
}

void ArrowPolyState::syncFromEnds(float sx, float sy, float ex, float ey)
{
	if (pts.size() < 2) {
		setTwo(sx, sy, ex, ey);
		return;
	}
	pts.front() = D2D1::Point2F(sx, sy);
	pts.back() = D2D1::Point2F(ex, ey);
}

D2D1_POINT_2F ArrowPolyState::front() const
{
	return pts.empty() ? D2D1::Point2F(0, 0) : pts.front();
}

D2D1_POINT_2F ArrowPolyState::back() const
{
	return pts.empty() ? D2D1::Point2F(0, 0) : pts.back();
}

void ArrowPolyState::translate(float dx, float dy)
{
	for (auto& p : pts) {
		p.x += dx;
		p.y += dy;
	}
}

float ArrowPolyState::pointToSegDist(float px, float py, D2D1_POINT_2F a, D2D1_POINT_2F b)
{
	const float vx = b.x - a.x, vy = b.y - a.y;
	const float len2 = vx * vx + vy * vy;
	float t = len2 > 0.f ? ((px - a.x) * vx + (py - a.y) * vy) / len2 : 0.f;
	t = std::clamp(t, 0.f, 1.f);
	return std::hypot(px - (a.x + t * vx), py - (a.y + t * vy));
}

std::vector<D2D1_POINT_2F> ArrowPolyState::transformAroundPivot(
	const std::vector<D2D1_POINT_2F>& src, int pivotIdx,
	D2D1_POINT_2F from, D2D1_POINT_2F to)
{
	if (pivotIdx < 0 || pivotIdx >= (int)src.size()) return src;
	const auto pivot = src[pivotIdx];
	const float ox = from.x - pivot.x, oy = from.y - pivot.y;
	const float nx = to.x - pivot.x, ny = to.y - pivot.y;
	const float oldLen = std::hypot(ox, oy);
	if (oldLen < 0.001f) return src;
	const float newLen = std::max(0.001f, std::hypot(nx, ny));
	const float scale = std::max(0.05f, newLen / oldLen);
	const float ang = std::atan2(ny, nx) - std::atan2(oy, ox);
	const float ca = std::cos(ang), sa = std::sin(ang);
	std::vector<D2D1_POINT_2F> out;
	out.reserve(src.size());
	for (int i = 0; i < (int)src.size(); i++) {
		if (i == pivotIdx) {
			out.push_back(src[i]);
			continue;
		}
		const float dx = src[i].x - pivot.x, dy = src[i].y - pivot.y;
		out.push_back(D2D1::Point2F(
			pivot.x + (dx * ca - dy * sa) * scale,
			pivot.y + (dx * sa + dy * ca) * scale));
	}
	return out;
}

namespace {
	std::vector<D2D1_POINT_2F> buildBigArrowTwo(
		D2D1_POINT_2F start, D2D1_POINT_2F end, float strokeWidth)
	{
		const float dx = end.x - start.x, dy = end.y - start.y;
		const float length = std::hypot(dx, dy);
		if (length < 6.f) return {};
		const float ux = dx / length, uy = dy / length;
		const float px = -uy, py = ux;
		const float tipHalf = 0.5f;
		const float bodyHalf = std::max(strokeWidth, 1.f) * 0.5f;
		const float headHalf = std::max(bodyHalf * 2.45f, bodyHalf + 4.f);
		const float headLength = std::min(length * 0.42f,
			std::max(std::max(strokeWidth * 3.2f, headHalf * 1.6f), 14.f));
		const auto neck = D2D1::Point2F(end.x - ux * headLength, end.y - uy * headLength);
		return {
			D2D1::Point2F(start.x + px * tipHalf, start.y + py * tipHalf),
			D2D1::Point2F(neck.x + px * bodyHalf, neck.y + py * bodyHalf),
			D2D1::Point2F(neck.x + px * headHalf, neck.y + py * headHalf),
			end,
			D2D1::Point2F(neck.x - px * headHalf, neck.y - py * headHalf),
			D2D1::Point2F(neck.x - px * bodyHalf, neck.y - py * bodyHalf),
			D2D1::Point2F(start.x - px * tipHalf, start.y - py * tipHalf),
		};
	}
}

std::vector<D2D1_POINT_2F> ArrowPolyState::buildBigArrowPolygon(
	const std::vector<D2D1_POINT_2F>& pts, float strokeWidth)
{
	// 按折线生成大箭头多边形
	if (pts.size() < 2) return {};
	if (pts.size() == 2) return buildBigArrowTwo(pts[0], pts[1], strokeWidth);

	struct Seg { D2D1_POINT_2F a, b; float len; };
	std::vector<Seg> segs;
	float total = 0.f;
	for (size_t i = 1; i < pts.size(); i++) {
		const float len = std::hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y);
		if (len < 0.001f) continue;
		segs.push_back({ pts[i - 1], pts[i], len });
		total += len;
	}
	if (segs.empty() || total < 6.f) return {};

	const float tipHalf = 0.5f;
	const float bodyHalf = std::max(strokeWidth, 1.f) * 0.5f;
	const float headHalf = std::max(bodyHalf * 2.45f, bodyHalf + 4.f);
	const float headLength = std::min(total * 0.42f,
		std::min(std::max(std::max(strokeWidth * 3.2f, headHalf * 1.6f), 14.f),
			std::max(6.f, total - 2.f)));
	const float neckDist = std::max(0.f, total - headLength);

	struct Sample { D2D1_POINT_2F p; float dist, tx, ty; };
	std::vector<Sample> samples;
	auto pushSample = [&](D2D1_POINT_2F p, float dist, float tx, float ty) {
		if (!samples.empty()) {
			auto& last = samples.back();
			if (std::hypot(last.p.x - p.x, last.p.y - p.y) < 0.2f) {
				last.dist = dist; last.tx = tx; last.ty = ty;
				return;
			}
		}
		samples.push_back({ p, dist, tx, ty });
	};

	float walked = 0.f;
	bool neckInserted = false;
	for (const auto& seg : segs) {
		const float ux = (seg.b.x - seg.a.x) / seg.len;
		const float uy = (seg.b.y - seg.a.y) / seg.len;
		if (samples.empty()) pushSample(seg.a, 0.f, ux, uy);
		if (!neckInserted && walked < neckDist && walked + seg.len >= neckDist) {
			const float t = (neckDist - walked) / seg.len;
			const auto neck = D2D1::Point2F(
				seg.a.x + (seg.b.x - seg.a.x) * t,
				seg.a.y + (seg.b.y - seg.a.y) * t);
			pushSample(neck, neckDist, ux, uy);
			neckInserted = true;
		}
		walked += seg.len;
		if (walked <= neckDist + 0.01f)
			pushSample(seg.b, std::min(walked, neckDist), ux, uy);
	}
	if (!neckInserted)
		return buildBigArrowTwo(segs.back().a, segs.back().b, strokeWidth);

	auto halfAt = [&](float dist) {
		if (neckDist <= 0.001f) return bodyHalf;
		const float t = std::clamp(dist / neckDist, 0.f, 1.f);
		return tipHalf + (bodyHalf - tipHalf) * t;
	};

	std::vector<D2D1_POINT_2F> left, right;
	for (int i = 0; i < (int)samples.size(); i++) {
		auto s = samples[i];
		float tx = s.tx, ty = s.ty;
		if (i > 0 && i + 1 < (int)samples.size()) {
			const auto& prev = samples[i - 1];
			const auto& next = samples[i + 1];
			float ax = s.p.x - prev.p.x, ay = s.p.y - prev.p.y;
			float bx = next.p.x - s.p.x, by = next.p.y - s.p.y;
			const float al = std::hypot(ax, ay), bl = std::hypot(bx, by);
			tx = ax / (al > 0 ? al : 1.f) + bx / (bl > 0 ? bl : 1.f);
			ty = ay / (al > 0 ? al : 1.f) + by / (bl > 0 ? bl : 1.f);
			const float tl = std::hypot(tx, ty);
			if (tl > 0) { tx /= tl; ty /= tl; }
		}
		const float nx = -ty, ny = tx;
		const float hw = halfAt(s.dist);
		left.push_back(D2D1::Point2F(s.p.x + nx * hw, s.p.y + ny * hw));
		right.push_back(D2D1::Point2F(s.p.x - nx * hw, s.p.y - ny * hw));
	}

	const auto tip = pts.back();
	const auto neck = samples.back().p;
	const auto& lastSeg = segs.back();
	float ux = (lastSeg.b.x - lastSeg.a.x) / lastSeg.len;
	float uy = (lastSeg.b.y - lastSeg.a.y) / lastSeg.len;
	const float hdx = tip.x - neck.x, hdy = tip.y - neck.y;
	const float hl = std::hypot(hdx, hdy);
	if (hl > 0.001f) { ux = hdx / hl; uy = hdy / hl; }
	const float px = -uy, py = ux;

	std::vector<D2D1_POINT_2F> poly;
	poly.reserve(left.size() + right.size() + 3);
	poly.insert(poly.end(), left.begin(), left.end());
	poly.push_back(D2D1::Point2F(neck.x + px * headHalf, neck.y + py * headHalf));
	poly.push_back(tip);
	poly.push_back(D2D1::Point2F(neck.x - px * headHalf, neck.y - py * headHalf));
	for (int i = (int)right.size() - 1; i >= 0; --i)
		poly.push_back(right[i]);
	return poly;
}

ArrowHitInfo ArrowPolyState::hit(float x, float y, float dpi, bool dragging, bool allowVirtualBreak) const
{
	ArrowHitInfo out;
	if (pts.size() < 2) return out;
	// 箭头命中半径 6（逻辑像素 × dpi）
	const float r = 6.f * dpi;
	const auto pf = D2D1::Point2F(x, y);
	auto nearPt = [&](D2D1_POINT_2F p) {
		return std::hypot(pf.x - p.x, pf.y - p.y) <= r;
	};

	if (nearPt(pts.front())) {
		out.kind = ArrowHitInfo::Start;
		out.pointIndex = 0;
		return out;
	}
	if (nearPt(pts.back())) {
		out.kind = ArrowHitInfo::End;
		out.pointIndex = (int)pts.size() - 1;
		return out;
	}
	for (int i = 1; i + 1 < (int)pts.size(); i++) {
		if (nearPt(pts[i])) {
			out.kind = ArrowHitInfo::Break;
			out.pointIndex = i;
			return out;
		}
	}
	const int breakCount = std::max(0, (int)pts.size() - 2);
	if (allowVirtualBreak && !dragging && breakCount < 3) {
		for (int i = 0; i + 1 < (int)pts.size(); i++) {
			const auto mid = D2D1::Point2F(
				(pts[i].x + pts[i + 1].x) * 0.5f,
				(pts[i].y + pts[i + 1].y) * 0.5f);
			if (nearPt(mid)) {
				out.kind = ArrowHitInfo::Break;
				out.insertAfter = i;
				return out;
			}
		}
	}
	const float segTol = 8.f * dpi;
	for (int i = 1; i < (int)pts.size(); i++) {
		if (pointToSegDist(x, y, pts[i - 1], pts[i]) <= segTol) {
			out.kind = ArrowHitInfo::Move;
			return out;
		}
	}
	return out;
}

void ArrowPolyState::beginDrag(const ArrowHitInfo& h, float x, float y)
{
	dragOrigin = pts;
	press = D2D1::Point2F(x, y);
	dragPointIndex = h.pointIndex;
	dragFree = false;
	if (h.kind == ArrowHitInfo::Start) {
		drag = DragKind::Start;
		dragPointIndex = 0;
		dragFree = freeStart;
	}
	else if (h.kind == ArrowHitInfo::End) {
		drag = DragKind::End;
		dragPointIndex = (int)pts.size() - 1;
		dragFree = freeEnd;
	}
	else if (h.kind == ArrowHitInfo::Break) {
		drag = DragKind::Break;
		if (h.insertAfter >= 0) {
			const int breakCount = std::max(0, (int)pts.size() - 2);
			if (breakCount >= 3) {
				drag = DragKind::None;
				return;
			}
			const int after = std::clamp(h.insertAfter, 0, (int)pts.size() - 2);
			pts.insert(pts.begin() + after + 1, D2D1::Point2F(x, y));
			if ((int)pts.size() > 5) pts.resize(5);
			dragPointIndex = after + 1;
			dragOrigin = pts;
		}
		dragFree = true;
	}
	else if (h.kind == ArrowHitInfo::Move) {
		drag = DragKind::Move;
	}
	else {
		drag = DragKind::None;
	}
}

void ArrowPolyState::dragTo(float x, float y)
{
	if (drag == DragKind::None || dragOrigin.empty()) return;
	if (drag == DragKind::Move) {
		const float dx = x - press.x, dy = y - press.y;
		pts = dragOrigin;
		translate(dx, dy);
		return;
	}
	pts = dragOrigin;
	const int idx = dragPointIndex;
	if (idx < 0 || idx >= (int)pts.size()) return;
	if (dragFree || drag == DragKind::Break) {
		pts[idx] = D2D1::Point2F(x, y);
		return;
	}
	const int pivotIdx = (drag == DragKind::Start) ? (int)pts.size() - 1 : 0;
	pts = transformAroundPivot(dragOrigin, pivotIdx, dragOrigin[idx], D2D1::Point2F(x, y));
	pts[idx] = D2D1::Point2F(x, y);
}

void ArrowPolyState::endDrag()
{
	drag = DragKind::None;
	dragPointIndex = -1;
	dragOrigin.clear();
}

bool ArrowPolyState::toggleFreeAt(float x, float y, float dpi)
{
	const auto h = hit(x, y, dpi, false, false);
	if (h.kind == ArrowHitInfo::Start) {
		freeStart = !freeStart;
		return true;
	}
	if (h.kind == ArrowHitInfo::End) {
		freeEnd = !freeEnd;
		return true;
	}
	return false;
}

void paintArrowPolyHandles(
	ID2D1DeviceContext* ctx,
	const ArrowPolyState& poly,
	float dpi,
	ID2D1SolidColorBrush* fill,
	ID2D1SolidColorBrush* border,
	ID2D1SolidColorBrush* shadow,
	bool dragging,
	bool showVirtualBreaks)
{
	if (poly.pts.size() < 2) return;
	auto drawDot = [&](D2D1_POINT_2F c, ID2D1SolidColorBrush* sh, ID2D1SolidColorBrush* bd) {
		ctx->FillEllipse(D2D1::Ellipse(c, 5.f * dpi, 5.f * dpi), sh);
		ctx->FillEllipse(D2D1::Ellipse(c, 4.f * dpi, 4.f * dpi), bd);
		ctx->DrawEllipse(D2D1::Ellipse(c, 4.f * dpi, 4.f * dpi), fill, 1.5f * dpi);
	};

	drawDot(poly.pts.front(), shadow, border);
	drawDot(poly.pts.back(), shadow, border);
	for (int i = 1; i + 1 < (int)poly.pts.size(); i++)
		drawDot(poly.pts[i], shadow, border);

	const int breakCount = std::max(0, (int)poly.pts.size() - 2);
	if (showVirtualBreaks && !dragging && breakCount < 3) {
		// 虚中点：每帧只建一次淡色刷，避免 CreateSolidColorBrush 风暴卡死
		ComPtr<ID2D1SolidColorBrush> faintShadow, faintBorder;
		auto d2d = Ling::D2D::get();
		d2d->deviceContext->CreateSolidColorBrush(
			D2D1::ColorF(0.f, 0.f, 0.f, 24.f / 255.f), faintShadow.GetAddressOf());
		d2d->deviceContext->CreateSolidColorBrush(
			D2D1::ColorF(0x34 / 255.f, 0xC7 / 255.f, 0x59 / 255.f, 0.38f), faintBorder.GetAddressOf());
		for (int i = 0; i + 1 < (int)poly.pts.size(); i++) {
			const auto mid = D2D1::Point2F(
				(poly.pts[i].x + poly.pts[i + 1].x) * 0.5f,
				(poly.pts[i].y + poly.pts[i + 1].y) * 0.5f);
			const bool solid = (poly.breakHoverKey == i + 1000);
			if (solid) drawDot(mid, shadow, border);
			else drawDot(mid, faintShadow.Get(), faintBorder.Get());
		}
	}

	if (poly.freeStart || poly.freeEnd) {
		ComPtr<ID2D1SolidColorBrush> ringBrush;
		Ling::D2D::get()->deviceContext->CreateSolidColorBrush(
			D2D1::ColorF(0x34 / 255.f, 0xC7 / 255.f, 0x59 / 255.f, 140.f / 255.f),
			ringBrush.GetAddressOf());
		if (poly.freeStart)
			ctx->DrawEllipse(D2D1::Ellipse(poly.pts.front(), 7.f * dpi, 7.f * dpi), ringBrush.Get(), 2.f * dpi);
		if (poly.freeEnd)
			ctx->DrawEllipse(D2D1::Ellipse(poly.pts.back(), 7.f * dpi, 7.f * dpi), ringBrush.Get(), 2.f * dpi);
	}
}

namespace ArrowParts {

ComPtr<ID2D1PathGeometry> shaftPath(ID2D1Factory* factory, const std::vector<D2D1_POINT_2F>& pts)
{
	ComPtr<ID2D1PathGeometry> path;
	if (!factory || pts.size() < 2) return path;
	if (FAILED(factory->CreatePathGeometry(path.GetAddressOf())) || !path) return {};
	ComPtr<ID2D1GeometrySink> sink;
	if (FAILED(path->Open(sink.GetAddressOf())) || !sink) return {};
	sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_HOLLOW);
	for (size_t i = 1; i < pts.size(); i++)
		sink->AddLine(pts[i]);
	sink->EndFigure(D2D1_FIGURE_END_OPEN);
	if (FAILED(sink->Close())) return {};
	return path;
}

namespace {
	// 趣味箭头SVG02.svg 的原始坐标：A 起笔、N 绕圈挂点、T 尖端
	struct FunCubic { float x1, y1, x2, y2, x, y; bool loop; };
	constexpr float kFunAx = 1.00024f, kFunAy = 5.43897f;
	constexpr float kFunTx = 180.2200f, kFunTy = 69.8590f;
	constexpr float kFunNx = 81.3054f, kFunNy = 16.9368f; // 圈的挂点
	// A→N、N→底→N（圈）、N→T
	const FunCubic kFunCubics[] = {
		{ 11.5302f, 2.17897f, 28.3902f, -1.21103f, 47.9202f, 2.87897f, false },
		{ 54.5544f, 4.26699f, 70.1543f, 7.53169f, 81.3054f, 16.9368f, false },
		{ 85.0972f, 20.1350f, 88.3747f, 24.0431f, 90.6102f, 28.8290f, true },
		{ 92.0104f, 31.8308f, 95.6153f, 39.5592f, 92.1202f, 46.7790f, true },
		{ 88.9802f, 53.2690f, 81.2502f, 56.9790f, 74.1702f, 56.2790f, true },
		{ 63.4802f, 55.2290f, 55.9302f, 44.2890f, 56.9702f, 35.6190f, true },
		{ 58.2402f, 25.0590f, 72.0102f, 20.0290f, 76.4302f, 18.4190f, true },
		{ 78.0464f, 17.8291f, 79.6729f, 17.3376f, 81.3054f, 16.9368f, true },
		{ 112.5520f, 9.26524f, 146.0140f, 34.8083f, 152.0100f, 39.3890f, false },
		{ 165.1000f, 49.3790f, 174.1200f, 60.5490f, 180.2200f, 69.8590f, false },
	};
}

ComPtr<ID2D1PathGeometry> funShaftPath(
	ID2D1Factory* factory, const std::vector<D2D1_POINT_2F>& pts,
	float loopScale, float loopRot, D2D1_POINT_2F* tipFrom, float headBack)
{
	ComPtr<ID2D1PathGeometry> path;
	if (!factory || pts.size() < 2) return path;
	if (FAILED(factory->CreatePathGeometry(path.GetAddressOf())) || !path) return {};
	ComPtr<ID2D1GeometrySink> sink;
	if (FAILED(path->Open(sink.GetAddressOf())) || !sink) return {};

	const float dx = pts[1].x - pts[0].x;
	const float dy = pts[1].y - pts[0].y;
	const float len = std::hypot(dx, dy);

	sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_HOLLOW);
	if (len < 12.f) {
		// 太短就不绕圈，退回普通折线
		for (size_t i = 1; i < pts.size(); i++) sink->AddLine(pts[i]);
	}
	else {
		// 把 SVG 的整条杆身（含圈）整体旋转/缩放到起笔段上：
		// 圈的形状、交叉角度、杆身下沉的弧度都原样保留
		const float ux = dx / len;
		// 圈要在屏幕下方：杆身朝左时把整条图形镜像一次（左→右画手绘感也更自然）
		const bool flip = ux < 0.f;
		const float svgDx = kFunTx - kFunAx;
		const float svgDy = (flip ? -1.f : 1.f) * (kFunTy - kFunAy);
		const float svgLen = std::hypot(svgDx, svgDy);
		const float scale = len / svgLen;
		const float rot = std::atan2(dy, dx) - std::atan2(svgDy, svgDx);
		const float cs = std::cos(rot), sn = std::sin(rot);
		auto map = [&](float x, float y) {
			const float px = (x - kFunAx) * scale;
			const float py = (y - kFunAy) * scale * (flip ? -1.f : 1.f);
			return D2D1::Point2F(
				pts[0].x + px * cs - py * sn,
				pts[0].y + px * sn + py * cs);
		};
		// 头部方向取「尖端往回 headBack 距离」的连线方向：
		// 尖端的瞬时切线在末端弯得厉害时会让 V 看起来歪向一边，
		// 用头部自身覆盖的那一段杆身的平均方向才视觉居中
		if (tipFrom) {
			const D2D1_POINT_2F tip = map(kFunTx, kFunTy);
			D2D1_POINT_2F anchor = tip;
			if (headBack > 0.5f && len > 12.f) {
				auto cubicAt = [](D2D1_POINT_2F p0, D2D1_POINT_2F p1,
					D2D1_POINT_2F p2, D2D1_POINT_2F p3, float t) {
					const float u = 1.f - t;
					const float a = u * u * u, b = 3.f * u * u * t;
					const float c = 3.f * u * t * t, d = t * t * t;
					return D2D1::Point2F(
						p0.x * a + p1.x * b + p2.x * c + p3.x * d,
						p0.y * a + p1.y * b + p2.y * c + p3.y * d);
				};
				// 从尖端往回到挂点：最后两段 → 圈之前的两段（跳过圈本身）
				const int order[] = { 9, 8, 1, 0 };
				float walked = 0.f;
				bool done = false;
				for (int idx : order) {
					if (done || idx >= (int)ARRAYSIZE(kFunCubics)) break;
					const auto& c = kFunCubics[idx];
					const auto p0 = (idx > 0)
						? map(kFunCubics[idx - 1].x, kFunCubics[idx - 1].y)
						: map(kFunAx, kFunAy);
					const auto p1 = map(c.x1, c.y1);
					const auto p2 = map(c.x2, c.y2);
					const auto p3 = map(c.x, c.y);
					auto last = p3;
					for (int s = 1; s <= 24 && !done; s++) {
						const auto q = cubicAt(p0, p1, p2, p3, 1.f - s / 24.f);
						const float seg = std::hypot(q.x - last.x, q.y - last.y);
						if (walked + seg >= headBack) {
							anchor = q;
							done = true;
							break;
						}
						walked += seg;
						last = q;
					}
					if (!done) anchor = last;
				}
			}
			*tipFrom = anchor;
		}
		// 每条箭头给圈一点缩放/旋转差异（挂点不动，杆身仍接得上）
		const float jc = std::cos(loopRot), js = std::sin(loopRot);
		auto jitter = [&](float x, float y) {
			const float lx = (x - kFunNx) * loopScale;
			const float ly = (y - kFunNy) * loopScale;
			return D2D1::Point2F(kFunNx + lx * jc - ly * js,
				kFunNy + lx * js + ly * jc);
		};
		for (const auto& c : kFunCubics) {
			auto p1 = D2D1::Point2F(c.x1, c.y1);
			auto p2 = D2D1::Point2F(c.x2, c.y2);
			auto p3 = D2D1::Point2F(c.x, c.y);
			if (c.loop) { // 只抖圈本身
				p1 = jitter(p1.x, p1.y);
				p2 = jitter(p2.x, p2.y);
				p3 = jitter(p3.x, p3.y);
			}
			sink->AddBezier(D2D1::BezierSegment(
				map(p1.x, p1.y), map(p2.x, p2.y), map(p3.x, p3.y)));
		}
		for (size_t i = 2; i < pts.size(); i++) sink->AddLine(pts[i]);
	}
	sink->EndFigure(D2D1_FIGURE_END_OPEN);
	if (FAILED(sink->Close())) return {};
	return path;
}

ComPtr<ID2D1PathGeometry> openHeadPath(ID2D1Factory* factory, D2D1_POINT_2F from, D2D1_POINT_2F to, float strokeWidth, float headScale)
{
	ComPtr<ID2D1PathGeometry> path;
	const float dx = to.x - from.x;
	const float dy = to.y - from.y;
	if (!factory || std::hypot(dx, dy) < 2.f) return path;
	const float angle = std::atan2(dy, dx);
	// headScale 同时放大翼长和张开角，趣味箭头用略大的头部
	const float len = (10.f + strokeWidth * 2.f) * headScale;
	const float spread = (kPi / 6.f) * headScale;
	const float lx = to.x - len * std::cos(angle - spread);
	const float ly = to.y - len * std::sin(angle - spread);
	const float rx = to.x - len * std::cos(angle + spread);
	const float ry = to.y - len * std::sin(angle + spread);

	if (FAILED(factory->CreatePathGeometry(path.GetAddressOf())) || !path) return {};
	ComPtr<ID2D1GeometrySink> sink;
	if (FAILED(path->Open(sink.GetAddressOf())) || !sink) return {};
	// 两段独立直线会在尖端变钝，必须一条折线走完（尖端由 join 决定尖/圆）
	sink->BeginFigure({ lx, ly }, D2D1_FIGURE_BEGIN_HOLLOW);
	sink->AddLine(to);
	sink->AddLine({ rx, ry });
	sink->EndFigure(D2D1_FIGURE_END_OPEN);
	if (FAILED(sink->Close())) return {};
	return path;
}

float annotBarHalf(float strokeWidth)
{
	return std::max(6.f, strokeWidth * 2.2f);
}

ComPtr<ID2D1PathGeometry> annotBarsPath(ID2D1Factory* factory, const std::vector<D2D1_POINT_2F>& pts, float strokeWidth)
{
	ComPtr<ID2D1PathGeometry> path;
	if (!factory || pts.size() < 2) return path;
	if (FAILED(factory->CreatePathGeometry(path.GetAddressOf())) || !path) return {};
	ComPtr<ID2D1GeometrySink> sink;
	if (FAILED(path->Open(sink.GetAddressOf())) || !sink) return {};

	const float half = annotBarHalf(strokeWidth);
	auto addBar = [&](D2D1_POINT_2F p, float dx, float dy) {
		const float len = std::hypot(dx, dy);
		if (len < 1.f) return;
		const float a = std::atan2(dy, dx) + kPi * 0.5f;
		const float cx = std::cos(a) * half;
		const float cy = std::sin(a) * half;
		sink->BeginFigure({ p.x - cx, p.y - cy }, D2D1_FIGURE_BEGIN_HOLLOW);
		sink->AddLine({ p.x + cx, p.y + cy });
		sink->EndFigure(D2D1_FIGURE_END_OPEN);
	};
	const auto a0 = pts.front();
	const auto a1 = pts[1];
	const auto b0 = pts[pts.size() - 2];
	const auto b1 = pts.back();
	addBar(a0, a1.x - a0.x, a1.y - a0.y);
	addBar(b1, b1.x - b0.x, b1.y - b0.y);
	if (FAILED(sink->Close())) return {};
	return path;
}

ComPtr<ID2D1StrokeStyle> makeShaftStyle(ID2D1Factory* factory, float strokeWidth, bool roundCap, bool dashed)
{
	ComPtr<ID2D1StrokeStyle> style;
	if (!factory) return style;
	// 圆头：圆帽 + 圆角；平头：平帽 + 斜接（折线用斜接不会在拐点露缺口）
	const auto cap = roundCap ? D2D1_CAP_STYLE_ROUND : D2D1_CAP_STYLE_FLAT;
	const auto join = roundCap ? D2D1_LINE_JOIN_ROUND : D2D1_LINE_JOIN_MITER;
	if (dashed) {
		const float sw = std::max(strokeWidth, 1.f);
		// 圆帽会在每段两端各鼓出 sw/2，间隙相应加宽，视觉节奏才与平头一致
		const float gap = 6.f + (roundCap ? sw : 0.f);
		float dashes[] = { 8.f / sw, gap / sw };
		factory->CreateStrokeStyle(
			D2D1::StrokeStyleProperties(cap, cap, cap, join, 10.f, D2D1_DASH_STYLE_CUSTOM, 0.f),
			dashes, ARRAYSIZE(dashes), style.GetAddressOf());
	}
	else {
		factory->CreateStrokeStyle(
			D2D1::StrokeStyleProperties(cap, cap, cap, join, 10.f, D2D1_DASH_STYLE_SOLID, 0.f),
			nullptr, 0, style.GetAddressOf());
	}
	return style;
}

ComPtr<ID2D1StrokeStyle> makeHeadStyle(ID2D1Factory* factory, bool roundCap)
{
	ComPtr<ID2D1StrokeStyle> style;
	if (!factory) return style;
	// 与轴线同一套 cap/join —— 圆头要作用到箭头头部；
	// 虚线永远不作用于头部，所以这里固定 SOLID
	const auto cap = roundCap ? D2D1_CAP_STYLE_ROUND : D2D1_CAP_STYLE_FLAT;
	const auto join = roundCap ? D2D1_LINE_JOIN_ROUND : D2D1_LINE_JOIN_MITER;
	factory->CreateStrokeStyle(
		D2D1::StrokeStyleProperties(cap, cap, cap, join, 10.f, D2D1_DASH_STYLE_SOLID, 0.f),
		nullptr, 0, style.GetAddressOf());
	return style;
}

} // namespace ArrowParts
