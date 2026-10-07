#include "pch.h"
#include "FreehandStroke.h"
#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace FreehandStroke {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kFixedPi = kPi + 0.0001f;
constexpr float kSoftRefSpeed = 0.35f;
constexpr float kSoftThinning = 0.72f;
constexpr float kStreamline = 0.72f;
constexpr float kSmoothing = 0.78f;

struct Vec { float x = 0; float y = 0; };
struct StrokePoint {
	Vec point;
	Vec vector{ 1, 1 };
	float pressure = 0.5f;
	float distance = 0;
	float runningLength = 0;
};
struct TimedPt { D2D1_POINT_2F p{}; int64_t t = 0; };

static float clampf(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }
static Vec v(float x, float y) { return { x, y }; }
static Vec add(Vec a, Vec b) { return { a.x + b.x, a.y + b.y }; }
static Vec sub(Vec a, Vec b) { return { a.x - b.x, a.y - b.y }; }
static Vec mul(Vec a, float s) { return { a.x * s, a.y * s }; }
static Vec neg(Vec a) { return { -a.x, -a.y }; }
static float dpr(Vec a, Vec b) { return a.x * b.x + a.y * b.y; }
static float mag(Vec a) { return std::hypot(a.x, a.y); }
static Vec uni(Vec a)
{
	const float m = mag(a);
	if (m < 1e-12f) return { 1, 0 };
	return { a.x / m, a.y / m };
}
static Vec per(Vec a) { return { a.y, -a.x }; }
static Vec lrp(Vec a, Vec b, float t) { return add(a, mul(sub(b, a), t)); }
static bool eq(Vec a, Vec b) { return a.x == b.x && a.y == b.y; }
static float dist(Vec a, Vec b) { return std::hypot(a.x - b.x, a.y - b.y); }
static float dist2(Vec a, Vec b)
{
	const float dx = a.x - b.x, dy = a.y - b.y;
	return dx * dx + dy * dy;
}
static Vec rotAround(Vec p, Vec c, float ang)
{
	const float s = std::sin(ang), co = std::cos(ang);
	const float x = p.x - c.x, y = p.y - c.y;
	return { x * co - y * s + c.x, x * s + y * co + c.y };
}
static float easeSin(float t)
{
	t = clampf(t, 0.f, 1.f);
	return std::sin((t * kPi) / 2.f);
}
static float strokeWidthToSize(float strokeWidth)
{
	return strokeWidth / (2.f * easeSin(0.5f));
}
static float radiusAt(float size, float thinning, float pressure)
{
	return size * easeSin(0.5f - thinning * (0.5f - pressure));
}
static float blendPressureByDistance(float prev, float distance, float refPx)
{
	const float sp = std::min(1.f, distance / std::max(1.f, refPx));
	const float rp = 1.f - sp;
	return clampf(prev + (rp - prev) * (0.28f + sp * 0.45f), 0.05f, 1.f);
}
static float pressureFromSpeed(float speedPxPerMs)
{
	const float s = std::max(0.f, speedPxPerMs);
	const float t = std::exp(-s / kSoftRefSpeed);
	return clampf(0.08f + 0.92f * t, 0.06f, 1.f);
}
static float turnBoost(const D2D1_POINT_2F& a, const D2D1_POINT_2F& b, const D2D1_POINT_2F& c)
{
	const float d0x = b.x - a.x, d0y = b.y - a.y;
	const float d1x = c.x - b.x, d1y = c.y - b.y;
	const float l0 = std::hypot(d0x, d0y), l1 = std::hypot(d1x, d1y);
	if (l0 < 1e-3f || l1 < 1e-3f) return 0.f;
	const float dot = (d0x * d1x + d0y * d1y) / (l0 * l1);
	const float bend = clampf(1.f - dot, 0.f, 2.f) * 0.5f;
	return bend * bend * 0.42f;
}
static void smoothPressures(std::vector<float>& pr)
{
	if (pr.size() < 3) return;
	constexpr float a = 0.22f;
	for (size_t i = 1; i < pr.size(); ++i)
		pr[i] = pr[i - 1] * (1.f - a) + pr[i] * a;
	for (int i = (int)pr.size() - 2; i >= 0; --i)
		pr[i] = pr[i] * (1.f - a) + pr[i + 1] * a;
}

static std::vector<TimedPt> streamlineTimed(const std::vector<TimedPt>& pts, float amount)
{
	if (pts.size() < 2 || amount <= 0.f) return pts;
	const float t = clampf(1.f - amount, 0.05f, 0.95f);
	std::vector<TimedPt> out;
	out.reserve(pts.size());
	out.push_back(pts.front());
	D2D1_POINT_2F prev = pts.front().p;
	for (size_t i = 1; i < pts.size(); ++i) {
		TimedPt s;
		s.p = { prev.x + (pts[i].p.x - prev.x) * t, prev.y + (pts[i].p.y - prev.y) * t };
		s.t = pts[i].t;
		out.push_back(s);
		prev = s.p;
	}
	out.back().p = pts.back().p;
	out.back().t = pts.back().t;
	return out;
}

static std::vector<TimedPt> simplifyTimed(const std::vector<TimedPt>& pts, float minDist)
{
	std::vector<TimedPt> out;
	if (pts.empty()) return out;
	out.push_back(pts.front());
	for (size_t i = 1; i + 1 < pts.size(); ++i) {
		const float dx = out.back().p.x - pts[i].p.x;
		const float dy = out.back().p.y - pts[i].p.y;
		if (std::hypot(dx, dy) >= minDist) out.push_back(pts[i]);
	}
	if (out.back().p.x != pts.back().p.x || out.back().p.y != pts.back().p.y || out.back().t != pts.back().t)
		out.push_back(pts.back());
	return out;
}

static std::vector<D2D1_POINT_2F> simplifyPts(const std::vector<D2D1_POINT_2F>& pts, float minDist)
{
	std::vector<D2D1_POINT_2F> out;
	if (pts.empty()) return out;
	out.push_back(pts.front());
	for (size_t i = 1; i + 1 < pts.size(); ++i) {
		const float dx = out.back().x - pts[i].x;
		const float dy = out.back().y - pts[i].y;
		if (std::hypot(dx, dy) >= minDist) out.push_back(pts[i]);
	}
	if (out.back().x != pts.back().x || out.back().y != pts.back().y)
		out.push_back(pts.back());
	return out;
}

static std::vector<D2D1_POINT_2F> streamlinePts(const std::vector<D2D1_POINT_2F>& pts, float amount)
{
	if (pts.size() < 2 || amount <= 0.f) return pts;
	const float t = clampf(1.f - amount, 0.05f, 0.95f);
	std::vector<D2D1_POINT_2F> out;
	out.reserve(pts.size());
	out.push_back(pts.front());
	D2D1_POINT_2F prev = pts.front();
	for (size_t i = 1; i < pts.size(); ++i) {
		const D2D1_POINT_2F next{ prev.x + (pts[i].x - prev.x) * t, prev.y + (pts[i].y - prev.y) * t };
		out.push_back(next);
		prev = next;
	}
	out.back() = pts.back();
	return out;
}

static std::vector<StrokePoint> getStrokePoints(std::vector<Vec> pts, std::vector<float> pr, float size, bool last)
{
	if (pts.empty()) return {};

	if (pts.size() == 2) {
		const Vec lastP = pts[1];
		const float lastPr = pr.size() > 1 ? pr[1] : 0.5f;
		pts.resize(1);
		pr.resize(1);
		for (int i = 1; i < 5; ++i) {
			pts.push_back(lrp(pts[0], lastP, i / 4.f));
			pr.push_back(lastPr);
		}
	}
	if (pts.size() == 1) {
		pts.push_back(add(pts[0], v(1, 1)));
		pr.push_back(pr[0]);
	}

	const float t = 0.15f + (1.f - kStreamline) * 0.85f;
	std::vector<StrokePoint> out;
	StrokePoint first;
	first.point = pts[0];
	first.pressure = pr[0];
	first.vector = { 1, 1 };
	out.push_back(first);

	bool reachedMin = false;
	float running = 0;
	StrokePoint prev = out[0];
	const int max = (int)pts.size() - 1;

	for (int i = 1; i < (int)pts.size(); ++i) {
		const Vec point = (last && i == max) ? pts[i] : lrp(prev.point, pts[i], t);
		if (eq(prev.point, point)) continue;
		const float d = dist(point, prev.point);
		running += d;
		if (i < max && !reachedMin) {
			if (running < size) continue;
			reachedMin = true;
		}
		StrokePoint sp;
		sp.point = point;
		sp.pressure = prev.pressure * 0.5f + pr[i] * 0.5f;
		sp.vector = uni(sub(prev.point, point));
		sp.distance = d;
		sp.runningLength = running;
		out.push_back(sp);
		prev = sp;
	}
	if (out.size() > 1) out[0].vector = out[1].vector;
	return out;
}

static std::vector<Vec> getStrokeOutline(const std::vector<StrokePoint>& pts, float size, float thinning)
{
	if (pts.empty() || size <= 0) return {};

	const float total = pts.back().runningLength;
	const float minD = (size * kSmoothing) * (size * kSmoothing);
	std::vector<Vec> left, right;
	left.reserve(pts.size() + 8);
	right.reserve(pts.size() + 8);

	Vec prevVector = pts[0].vector;
	Vec lastLeft = pts[0].point;
	Vec lastRight = pts[0].point;
	float prevR = -1;
	bool sharp = false;

	for (size_t i = 0; i < pts.size(); ++i) {
		const bool isLast = (i == pts.size() - 1);
		if (!isLast && total - pts[i].runningLength < 3) continue;

		const float targetR = std::max(0.01f, radiusAt(size, thinning, pts[i].pressure));
		const float r = (prevR < 0) ? targetR : prevR + (targetR - prevR) * 0.22f;
		prevR = r;
		const Vec c = pts[i].point;
		const Vec dir = pts[i].vector;

		if (isLast) {
			const Vec n = mul(per(dir), r);
			left.push_back(sub(c, n));
			right.push_back(add(c, n));
			break;
		}

		const Vec nextDir = pts[i + 1].vector;
		const float dNext = dpr(dir, nextDir);
		const bool cornerPrev = dpr(dir, prevVector) < -0.35f && !sharp;
		const bool cornerNext = dNext < -0.35f;
		if (cornerPrev || cornerNext) {
			const Vec n = mul(per(prevVector), r);
			for (int k = 0; k <= 6; ++k) {
				const float a = kFixedPi * (k / 6.f);
				left.push_back(rotAround(sub(c, n), c, a));
				right.push_back(rotAround(add(c, n), c, -a));
			}
			lastLeft = left.back();
			lastRight = right.back();
			if (cornerNext) sharp = true;
			continue;
		}
		sharp = false;

		Vec offset = lrp(nextDir, dir, dNext);
		offset = mul(per(uni(offset)), r);
		const Vec lp = sub(c, offset);
		const Vec rp = add(c, offset);
		if (i <= 1 || dist2(lastLeft, lp) > minD) {
			left.push_back(lp);
			lastLeft = lp;
		}
		if (i <= 1 || dist2(lastRight, rp) > minD) {
			right.push_back(rp);
			lastRight = rp;
		}
		prevVector = dir;
	}

	if (left.empty() || right.empty()) {
		const Vec c = pts[0].point;
		const float r = std::max(0.5f, radiusAt(size, thinning, pts[0].pressure));
		std::vector<Vec> circle;
		for (float a = 0; a <= kPi * 2; a += kPi / 8)
			circle.push_back(add(c, v(std::cos(a) * r, std::sin(a) * r)));
		circle.push_back(circle.front());
		return circle;
	}

	std::vector<Vec> out;
	out.reserve(left.size() + right.size() + 48);
	out.insert(out.end(), left.begin(), left.end());

	{
		const Vec c = pts.back().point;
		const float r = std::max(0.01f, radiusAt(size, thinning, pts.back().pressure));
		const Vec start = add(c, mul(per(neg(pts.back().vector)), r));
		for (int k = 1; k < 16; ++k)
			out.push_back(rotAround(start, c, kFixedPi * 1.5f * (k / 16.f)));
	}

	for (int i = (int)right.size() - 1; i >= 0; --i)
		out.push_back(right[i]);

	{
		const Vec c = pts.front().point;
		const Vec start = right.front();
		for (int k = 1; k <= 8; ++k)
			out.push_back(rotAround(start, c, kFixedPi * (k / 8.f)));
	}
	return out;
}

static ComPtr<ID2D1PathGeometry> pathFromOutline(ID2D1Factory* factory, const std::vector<Vec>& outline)
{
	ComPtr<ID2D1PathGeometry> geo;
	if (!factory || outline.size() < 2) return geo;
	factory->CreatePathGeometry(geo.GetAddressOf());
	ComPtr<ID2D1GeometrySink> sink;
	geo->Open(sink.GetAddressOf());
	sink->SetFillMode(D2D1_FILL_MODE_WINDING);

	auto toP = [](Vec v) { return D2D1::Point2F(v.x, v.y); };
	auto med = [&](size_t i, size_t j) {
		return D2D1::Point2F((outline[i].x + outline[j].x) * 0.5f, (outline[i].y + outline[j].y) * 0.5f);
	};
	const size_t n = outline.size();
	sink->BeginFigure(med(n - 1, 0), D2D1_FIGURE_BEGIN_FILLED);
	for (size_t i = 0; i < n; ++i) {
		const size_t j = (i + 1) % n;
		sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(toP(outline[i]), med(i, j)));
	}
	sink->EndFigure(D2D1_FIGURE_END_CLOSED);
	sink->Close();
	return geo;
}

} // namespace

ComPtr<ID2D1PathGeometry> outline(ID2D1Factory* factory, const std::vector<D2D1_POINT_2F>& pts,
	float strokeWidth, bool softPen, const std::vector<int64_t>& times)
{
	if (!factory || pts.empty()) return {};

	const float width = std::max(1.f, strokeWidth);
	const float size = strokeWidthToSize(width);

	std::vector<Vec> raw;
	std::vector<float> pr;
	raw.reserve(pts.size());
	pr.reserve(pts.size());

	if (softPen) {
		const bool useTime = (times.size() == pts.size());
		std::vector<TimedPt> timed;
		timed.reserve(pts.size());
		for (size_t i = 0; i < pts.size(); ++i) {
			TimedPt s;
			s.p = pts[i];
			s.t = useTime ? times[i] : (int64_t)i * 16;
			timed.push_back(s);
		}
		timed = streamlineTimed(timed, 0.42f);
		timed = simplifyTimed(timed, std::max(1.2f, size * 0.12f));

		raw.push_back({ timed[0].p.x, timed[0].p.y });
		pr.push_back(0.88f);
		for (size_t i = 1; i < timed.size(); ++i) {
			raw.push_back({ timed[i].p.x, timed[i].p.y });
			const float d = std::hypot(timed[i].p.x - timed[i - 1].p.x, timed[i].p.y - timed[i - 1].p.y);
			float target;
			if (useTime) {
				float dt = (float)(timed[i].t - timed[i - 1].t);
				if (dt < 1.f) dt = 0.45f;
				target = pressureFromSpeed(d / dt);
			}
			else {
				target = blendPressureByDistance(pr.back(), d, 4.f);
			}
			if (i >= 2)
				target = std::min(1.f, target + turnBoost(timed[i - 2].p, timed[i - 1].p, timed[i].p));
			constexpr float follow = 0.55f;
			pr.push_back(clampf(pr.back() * (1.f - follow) + target * follow, 0.06f, 1.f));
		}
		smoothPressures(pr);
	}
	else {
		const auto streamed = streamlinePts(pts, 0.62f);
		const auto src = simplifyPts(streamed, std::max(1.6f, size * 0.18f));
		for (const auto& p : src) {
			raw.push_back({ p.x, p.y });
			pr.push_back(0.5f);
		}
	}

	const float thinning = softPen ? kSoftThinning : 0.f;
	const auto sp = getStrokePoints(std::move(raw), std::move(pr), size, true);
	const auto poly = getStrokeOutline(sp, size, thinning);
	return pathFromOutline(factory, poly);
}

} // namespace FreehandStroke
