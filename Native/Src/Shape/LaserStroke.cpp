#include "pch.h"
#include "LaserStroke.h"
#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace LaserStroke {
namespace {

constexpr float kPi = 3.14159265358979323846f;
// 单位向量（t 不参与轮廓计算，只沿用宿主点的 t）
const Point kUnit{ 1.f, 0.f, 0.f };
// 亚像素阈值：低于它就当作已消失（按 dpi 缩放）
constexpr float kGoneSize{ 0.35f };
constexpr float kCapGoneSize{ 0.5f };
constexpr float kStartCapMinSize{ 0.1f };
// 急转弯判定：低速 75°，高速 37.5°（QT cornerVariance）
constexpr float kCornerSpeed{ 35.f };

Point add(Point a, Point b) { return { a.x + b.x, a.y + b.y, a.t + b.t }; }
Point sub(Point a, Point b) { return { a.x - b.x, a.y - b.y, a.t - b.t }; }
Point smul(Point a, float s) { return { a.x * s, a.y * s, a.t * s }; }
float mag(Point a) { return std::sqrt(a.x * a.x + a.y * a.y); }

Point norm(Point a)
{
	const float m = mag(a);
	if (m < 1e-12f) return { 1.f, 0.f, a.t };
	return { a.x / m, a.y / m, a.t };
}

Point rot(Point a, float rad)
{
	const float c = std::cos(rad);
	const float s = std::sin(rad);
	return { c * a.x - s * a.y, s * a.x + c * a.y, a.t };
}

Point plerp(Point a, Point b, float t) { return add(a, smul(sub(b, a), t)); }

float dist(Point a, Point b)
{
	const float dx = b.x - a.x;
	const float dy = b.y - a.y;
	return std::sqrt(dx * dx + dy * dy);
}

float angleAt(Point p, Point p1, Point p2)
{
	return std::atan2(p2.y - p.y, p2.x - p.x) - std::atan2(p1.y - p.y, p1.x - p.x);
}

float normAngle(float a) { return std::atan2(std::sin(a), std::cos(a)); }

float runLength(const std::vector<Point>& ps)
{
	if (ps.size() < 2) return 0.f;
	float len = 0.f;
	for (size_t i = 1; i < ps.size(); ++i) len += dist(ps[i - 1], ps[i]);
	// 原版 laser-pointer math.runLength 会多加一次末段，这里保持一致
	len += dist(ps[ps.size() - 2], ps[ps.size() - 1]);
	return len;
}

float easeOut(float t)
{
	t = std::clamp(t, 0.f, 1.f);
	return t * (2.f - t);
}

// 按点龄收细：新点全粗，超过 hold 后从尾部开始消失
float sizeMapping(float timestamp, int64_t now)
{
	const float age = (float)(now - (int64_t)timestamp);
	if (age <= (float)kHoldMs) return 1.f;
	return easeOut(std::clamp(1.f - (age - (float)kHoldMs) / (float)kFadeMs, 0.f, 1.f));
}

Point lastPoint(const Trail& trail)
{
	if (!trail.tail.empty()) return trail.tail.back();
	if (!trail.stable.empty()) return trail.stable.back();
	return {};
}

void stabilizeTail(Trail& trail)
{
	trail.stable.insert(trail.stable.end(), trail.tail.begin(), trail.tail.end());
	trail.tail.clear();
}

float cornerVariance(float speed, float unit) { return speed > kCornerSpeed * unit ? 0.5f : 1.f; }

std::vector<Point> allPoints(const Trail& trail)
{
	std::vector<Point> pts = trail.stable;
	pts.insert(pts.end(), trail.tail.begin(), trail.tail.end());
	return pts;
}

float strokeSize(const Trail& trail, const Point& pt, int64_t now)
{
	const float base = std::max(1.f, trail.width) * 0.5f;
	return base * sizeMapping(pt.t, now);
}

std::vector<Point> getStrokeOutline(const Trail& trail, int64_t now)
{
	if (trail.fresh) return {};

	const std::vector<Point> points = allPoints(trail);
	const int len = (int)points.size();
	if (len == 0) return {};

	const float unit = trail.unit > 0.f ? trail.unit : 1.f;
	const float baseSize = std::max(1.f, trail.width) * 0.5f;

	if (len == 1) {
		const Point& c = points[0];
		const float size = strokeSize(trail, c, now);
		if (size < kCapGoneSize * unit) return {};
		std::vector<Point> ps;
		for (float theta = 0.f; theta <= kPi * 2.f; theta += kPi / 16.f)
			ps.push_back(add(c, smul(rot(kUnit, theta), size)));
		ps.push_back(add(c, smul(kUnit, size)));
		return ps;
	}

	if (len == 2) {
		const Point& c = points[0];
		const Point& n = points[1];
		const float cSize = strokeSize(trail, c, now);
		const float nSize = strokeSize(trail, n, now);
		if (cSize < kCapGoneSize * unit || nSize < kCapGoneSize * unit) return {};
		std::vector<Point> ps;
		const float pAngle = angleAt(c, Point{ c.x, c.y - 100.f * unit, c.t }, n);
		for (float theta = pAngle; theta <= kPi + pAngle; theta += kPi / 16.f)
			ps.push_back(add(c, smul(rot(kUnit, theta), cSize)));
		for (float theta = kPi + pAngle; theta <= kPi * 2.f + pAngle; theta += kPi / 16.f)
			ps.push_back(add(n, smul(rot(kUnit, theta), nSize)));
		if (!ps.empty()) ps.push_back(ps[0]);
		return ps;
	}

	std::vector<Point> forwardPoints;
	std::vector<Point> backwardPoints;
	float speed = 0.f;
	float prevSpeed = 0.f;
	int visibleStartIndex = 0;

	for (int i = 1; i < len - 1; ++i) {
		const Point& p = points[i - 1];
		const Point& c = points[i];
		const Point& n = points[i + 1];

		const float d = dist(p, c);
		speed = prevSpeed + (d - prevSpeed) * 0.2f;

		const float cSize = strokeSize(trail, c, now);
		if (cSize < kGoneSize * unit) {
			// 该点已经收没了：可见段起点后移
			visibleStartIndex = i + 1;
			continue;
		}

		const Point dirPC = norm(sub(p, c));
		const Point dirNC = norm(sub(n, c));
		const Point p1dirPC = rot(dirPC, kPi / 2.f);
		const Point p2dirPC = rot(dirPC, -kPi / 2.f);
		const Point p1dirNC = rot(dirNC, kPi / 2.f);
		const Point p2dirNC = rot(dirNC, -kPi / 2.f);

		const Point p1PC = add(c, smul(p1dirPC, cSize));
		const Point p2PC = add(c, smul(p2dirPC, cSize));
		const Point p1NC = add(c, smul(p1dirNC, cSize));
		const Point p2NC = add(c, smul(p2dirNC, cSize));

		const Point ftdir = add(p1dirPC, p2dirNC);
		const Point btdir = add(p2dirPC, p1dirNC);

		const Point paPC = add(c, smul(mag(ftdir) == 0.f ? dirPC : norm(ftdir), cSize));
		const Point paNC = add(c, smul(mag(btdir) == 0.f ? dirNC : norm(btdir), cSize));

		const float cAngle = normAngle(angleAt(c, p, n));
		const float dAngle = (75.f / 180.f) * kPi * cornerVariance(speed, unit);

		if (std::abs(cAngle) < dAngle) {
			// 急转弯：内侧补圆角，外侧绕过去，避免拐点被切掉
			const float tAngle = std::abs(normAngle(kPi - cAngle));
			if (tAngle == 0.f) continue;

			if (cAngle < 0.f) {
				backwardPoints.push_back(p2PC);
				backwardPoints.push_back(paNC);
				for (float theta = 0.f; theta <= tAngle; theta += tAngle / 4.f)
					forwardPoints.push_back(add(c, rot(smul(p1dirPC, cSize), theta)));
				for (float theta = tAngle; theta >= 0.f; theta -= tAngle / 4.f)
					backwardPoints.push_back(add(c, rot(smul(p1dirPC, cSize), theta)));
				backwardPoints.push_back(paNC);
				backwardPoints.push_back(p1NC);
			}
			else {
				forwardPoints.push_back(p1PC);
				forwardPoints.push_back(paPC);
				for (float theta = 0.f; theta <= tAngle; theta += tAngle / 4.f)
					backwardPoints.push_back(add(c, rot(smul(p1dirPC, -cSize), -theta)));
				for (float theta = tAngle; theta >= 0.f; theta -= tAngle / 4.f)
					forwardPoints.push_back(add(c, rot(smul(p1dirPC, -cSize), -theta)));
				forwardPoints.push_back(paPC);
				forwardPoints.push_back(p2NC);
			}
		}
		else {
			forwardPoints.push_back(paPC);
			backwardPoints.push_back(paNC);
		}
		prevSpeed = speed;
	}

	if (visibleStartIndex >= len - 2) {
		// 只剩末端还在：绘制中用全粗圆头，抬笔后连它一起收
		if (trail.keepHead) {
			const Point& c = points[len - 1];
			std::vector<Point> ps;
			for (float theta = 0.f; theta <= kPi * 2.f; theta += kPi / 16.f)
				ps.push_back(add(c, smul(rot(kUnit, theta), baseSize)));
			ps.push_back(add(c, smul(kUnit, baseSize)));
			return ps;
		}
		return {};
	}

	const Point& first = points[visibleStartIndex];
	const Point& second = points[visibleStartIndex + 1];
	const Point& penultimate = points[len - 2];
	const Point& ultimate = points[len - 1];

	const Point dirFS = norm(sub(second, first));
	const Point dirPU = norm(sub(penultimate, ultimate));
	const Point ppdirFS = rot(dirFS, -kPi / 2.f);
	const Point ppdirPU = rot(dirPU, kPi / 2.f);

	const float startCapSize = strokeSize(trail, first, now);
	std::vector<Point> startCap;
	const float endCapSize = trail.keepHead ? baseSize : strokeSize(trail, penultimate, now);
	std::vector<Point> endCap;

	if (startCapSize > kStartCapMinSize * unit) {
		for (float theta = 0.f; theta <= kPi; theta += kPi / 16.f)
			startCap.insert(startCap.begin(), add(first, rot(smul(ppdirFS, startCapSize), -theta)));
		startCap.insert(startCap.begin(), add(first, smul(ppdirFS, -startCapSize)));
	}
	else {
		startCap.push_back(first);
	}

	for (float theta = 0.f; theta <= kPi * 3.f; theta += kPi / 16.f)
		endCap.push_back(add(ultimate, rot(smul(ppdirPU, -endCapSize), -theta)));

	std::vector<Point> strokeOutline = startCap;
	strokeOutline.insert(strokeOutline.end(), forwardPoints.begin(), forwardPoints.end());
	for (int i = (int)endCap.size() - 1; i >= 0; --i)
		strokeOutline.push_back(endCap[i]);
	for (int i = (int)backwardPoints.size() - 1; i >= 0; --i)
		strokeOutline.push_back(backwardPoints[i]);
	if (!startCap.empty()) strokeOutline.push_back(startCap[0]);

	return strokeOutline;
}

} // namespace

void addPoint(Trail& trail, float x, float y, int64_t now)
{
	Point point{ x, y, (float)now };

	if (!trail.original.empty()) {
		const Point& last = trail.original.back();
		if (last.x == point.x && last.y == point.y) return;
	}
	trail.original.push_back(point);

	if (trail.fresh) {
		trail.fresh = false;
		trail.stable.push_back(point);
		return;
	}

	if (kStreamline > 0.f)
		point = plerp(lastPoint(trail), point, 1.f - kStreamline);

	const Point from = lastPoint(trail);
	const float step = dist(from, point);
	if (step > 7.f * trail.unit) {
		// 采样太稀：按 5px 补点，免得快速划动时轮廓出现折角
		const int n = std::clamp((int)std::ceil(step / (5.f * trail.unit)), 2, 8);
		for (int i = 1; i <= n; ++i) {
			const float t = (float)i / (float)n;
			Point mid = plerp(from, point, t);
			mid.t = from.t + (point.t - from.t) * t;
			trail.tail.push_back(mid);
		}
	}
	else {
		trail.tail.push_back(point);
	}
	if (runLength(trail.tail) > kMaxTailLength * trail.unit)
		stabilizeTail(trail);
}

void closeTrail(Trail& trail, int64_t now)
{
	stabilizeTail(trail);
	trail.keepHead = false;
	trail.closedAt = now;
}

bool stillVisible(const Trail& trail, int64_t now)
{
	if (trail.pointCount() <= 0) return false;
	int64_t newest = 0;
	if (!trail.tail.empty()) newest = (int64_t)trail.tail.back().t;
	else if (!trail.stable.empty()) newest = (int64_t)trail.stable.back().t;
	else if (!trail.original.empty()) newest = (int64_t)trail.original.back().t;
	return now - newest < kHoldMs + kFadeMs;
}

std::vector<Point> outline(const Trail& trail, int64_t now)
{
	return getStrokeOutline(trail, now);
}

ComPtr<ID2D1PathGeometry> buildGeometry(ID2D1Factory* factory, const Trail& trail, int64_t now)
{
	ComPtr<ID2D1PathGeometry> geo;
	if (!factory) return geo;

	const std::vector<Point> o = getStrokeOutline(trail, now);
	const int n = (int)o.size();
	if (n < 2) return geo;
	if (FAILED(factory->CreatePathGeometry(geo.GetAddressOf())) || !geo) return {};
	ComPtr<ID2D1GeometrySink> sink;
	if (FAILED(geo->Open(sink.GetAddressOf())) || !sink) return {};

	auto P = [](const Point& p) { return D2D1::Point2F(p.x, p.y); };
	sink->SetFillMode(D2D1_FILL_MODE_WINDING);
	sink->BeginFigure(P(o[0]), D2D1_FIGURE_BEGIN_FILLED);
	if (n == 2) {
		sink->AddLine(P(o[1]));
	}
	else {
		// 中点二次曲线闭合（QT appendSmoothClosed）：控制点取顶点，终点取相邻中点
		for (int i = 1; i < n - 1; ++i) {
			sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(
				P(o[i]),
				D2D1::Point2F((o[i].x + o[i + 1].x) * 0.5f, (o[i].y + o[i + 1].y) * 0.5f)));
		}
		sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(P(o[n - 1]), P(o[0])));
	}
	sink->EndFigure(D2D1_FIGURE_END_CLOSED);
	if (FAILED(sink->Close())) return {};
	return geo;
}

} // namespace LaserStroke
