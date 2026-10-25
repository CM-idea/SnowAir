#include "pch.h"
#include "LaserStroke.h"
#include <algorithm>
#include <cmath>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace LaserStroke {
namespace {

constexpr float kPi = 3.14159265358979323846f;
// 亚像素阈值：半径低于该值视为不可见
constexpr float kGoneRadius{ 0.2f };
// 单帧参与绘制的采样上限，超出后等距抽稀以限制几何量
constexpr int kMaxDrawSamples{ 160 };
// 圆帽的细分段数
constexpr int kCapSegments{ 8 };

struct Vertex
{
	float x{ 0.f };
	float y{ 0.f };
	float r{ 0.f };
};

Vertex vertexAt(const Trail& trail, size_t i, int64_t now)
{
	const Point& p = trail.points[i];
	const int64_t age = std::max<int64_t>(0, now - p.t);
	const float elapsed = (float)std::min<int64_t>(age, kLifeMs) / (float)kLifeMs;
	// 半径只随时间衰减：拖尾长度等于寿命内的位移，与采样率无关
	const float base = std::max(1.f, trail.width) * 0.5f;
	return { p.x, p.y, base * (1.f - elapsed * elapsed) };
}

// 轮廓点作控制点、相邻中点作曲线点，闭合后一次填充，边缘保持连续曲率
ComPtr<ID2D1PathGeometry> smoothPath(ID2D1Factory* factory, const std::vector<D2D1_POINT_2F>& outline)
{
	ComPtr<ID2D1PathGeometry> geo;
	const size_t n = outline.size();
	if (!factory || n < 3) return geo;
	if (FAILED(factory->CreatePathGeometry(geo.GetAddressOf())) || !geo) return geo;
	ComPtr<ID2D1GeometrySink> sink;
	if (FAILED(geo->Open(sink.GetAddressOf())) || !sink) return geo;
	sink->SetFillMode(D2D1_FILL_MODE_WINDING);

	auto med = [&](size_t i, size_t j) {
		const size_t a = i % n, b = j % n;
		return D2D1::Point2F((outline[a].x + outline[b].x) * 0.5f,
			(outline[a].y + outline[b].y) * 0.5f);
	};
	sink->BeginFigure(med(n - 1, 0), D2D1_FIGURE_BEGIN_FILLED);
	for (size_t i = 0; i < n; ++i)
		sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(outline[i], med(i, i + 1)));
	sink->EndFigure(D2D1_FIGURE_END_CLOSED);
	if (FAILED(sink->Close())) {
		geo.Reset();
		return geo;
	}
	return geo;
}

// 法线取前后两点连线，端点单侧；左右两条边 + 两端圆帽，首尾相接成一条闭合轮廓
std::vector<D2D1_POINT_2F> buildOutline(const std::vector<Vertex>& vs)
{
	const int n = (int)vs.size();
	std::vector<D2D1_POINT_2F> left((size_t)n), right((size_t)n);

	auto unit = [](float dx, float dy, float& ox, float& oy) {
		const float len = std::sqrt(dx * dx + dy * dy);
		if (len < 1e-4f) { ox = 1.f; oy = 0.f; return; }
		ox = dx / len;
		oy = dy / len;
	};

	for (int i = 0; i < n; ++i) {
		const Vertex& p = vs[(size_t)i];
		const Vertex& a = vs[(size_t)std::max(0, i - 1)];
		const Vertex& b = vs[(size_t)std::min(n - 1, i + 1)];
		float ux = 1.f, uy = 0.f;
		unit(b.x - a.x, b.y - a.y, ux, uy);
		const float nx = -uy, ny = ux;
		const float r = std::max(0.01f, p.r);
		left[(size_t)i] = D2D1::Point2F(p.x - nx * r, p.y - ny * r);
		right[(size_t)i] = D2D1::Point2F(p.x + nx * r, p.y + ny * r);
	}

	std::vector<D2D1_POINT_2F> out;
	out.reserve((size_t)n * 2 + kCapSegments * 2);
	for (const auto& p : left) out.push_back(p);

	// 终点圆帽：-法线 侧 → 前进方向 → +法线 侧
	{
		const Vertex& e = vs[(size_t)n - 1];
		const Vertex& prev = vs[(size_t)n - 2];
		float ux = 1.f, uy = 0.f;
		unit(e.x - prev.x, e.y - prev.y, ux, uy);
		const float nx = -uy, ny = ux;
		const float r = std::max(0.01f, e.r);
		for (int k = 1; k < kCapSegments; ++k) {
			const float th = kPi * (float)k / (float)kCapSegments;
			const float c = std::cos(th), s = std::sin(th);
			out.push_back(D2D1::Point2F(e.x + (-nx * c + ux * s) * r, e.y + (-ny * c + uy * s) * r));
		}
	}

	for (int i = n - 1; i >= 0; --i) out.push_back(right[(size_t)i]);

	// 起点圆帽：+法线 侧 → 反方向 → -法线 侧
	{
		const Vertex& f = vs[0];
		const Vertex& next = vs[1];
		float ux = 1.f, uy = 0.f;
		unit(next.x - f.x, next.y - f.y, ux, uy);
		const float nx = -uy, ny = ux;
		const float r = std::max(0.01f, f.r);
		for (int k = 1; k < kCapSegments; ++k) {
			const float th = kPi * (float)k / (float)kCapSegments;
			const float c = std::cos(th), s = std::sin(th);
			out.push_back(D2D1::Point2F(f.x + (nx * c - ux * s) * r, f.y + (ny * c - uy * s) * r));
		}
	}
	return out;
}

} // namespace

void addPoint(Trail& trail, float x, float y, int64_t now)
{
	auto& ps = trail.points;
	// 淘汰过期点，保留一个用于尾部收细
	while (ps.size() > 1 && now - ps[1].t >= kLifeMs) ps.erase(ps.begin());
	if (!ps.empty() && now - ps.back().t >= kLifeMs) ps.clear();

	// 原地重复的采样不刷新寿命，静止时轨迹自然淡出
	if (trail.hasLast && trail.lastX == x && trail.lastY == y) return;
	trail.hasLast = true;
	trail.lastX = x;
	trail.lastY = y;

	Point p{ x, y, now, !ps.empty() };
	if (p.connected) {
		const Point& prev = ps.back();
		p.x = prev.x + (p.x - prev.x) * kFollow;
		p.y = prev.y + (p.y - prev.y) * kFollow;
	}
	if ((int)ps.size() >= kMaxPoints) ps.erase(ps.begin());
	ps.push_back(p);
}

bool visible(const Trail& trail, int64_t now)
{
	if (trail.points.size() < 2) return false;
	// 最新点过期即整段不可见
	return now - trail.points.back().t < kLifeMs;
}

void paint(ID2D1DeviceContext* ctx, ID2D1Brush* brush, const Trail& trail, int64_t now)
{
	const int n = trail.pointCount();
	if (!ctx || !brush || n < 2) return;

	// 等距抽稀：只影响绘制精度，不改变轨迹长度
	const int stride = std::max(1, (n + kMaxDrawSamples - 1) / kMaxDrawSamples);
	std::vector<Vertex> vs;
	vs.reserve((size_t)(n / stride) + 2);
	for (int i = 0; i < n; i += stride) vs.push_back(vertexAt(trail, (size_t)i, now));
	if ((n - 1) % stride != 0) vs.push_back(vertexAt(trail, (size_t)(n - 1), now));

	// 剔除半径已不可见的尾部采样点
	size_t first = 0;
	while (first + 1 < vs.size() && vs[first].r < kGoneRadius) ++first;
	vs.erase(vs.begin(), vs.begin() + (ptrdiff_t)first);
	if (vs.size() < 3) return;

	// 三点平滑：抹平折角，端点保持不变以保证笔头不滞后
	std::vector<Vertex> sm = vs;
	for (size_t i = 1; i + 1 < vs.size(); ++i) {
		sm[i].x = vs[i - 1].x * 0.25f + vs[i].x * 0.5f + vs[i + 1].x * 0.25f;
		sm[i].y = vs[i - 1].y * 0.25f + vs[i].y * 0.5f + vs[i + 1].y * 0.25f;
	}

	ComPtr<ID2D1Factory> factory;
	ctx->GetFactory(factory.GetAddressOf());
	if (!factory) return;
	auto geo = smoothPath(factory.Get(), buildOutline(sm));
	if (!geo) return;
	ctx->FillGeometry(geo.Get(), brush);
}

} // namespace LaserStroke
