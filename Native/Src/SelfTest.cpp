#include "pch.h"
#include "SelfTest.h"
#include "Shape/RoundBox.h"
#include "Shape/LinkGeom.h"
#include "Shape/ShapeBase.h"
#include "Shape/ArrowPoly.h"
#include <cstdio>

namespace
{
	FILE* g_log{ nullptr };
	int g_fail{ 0 };

	void check(bool ok, const wchar_t* name)
	{
		if (!ok) ++g_fail;
		if (g_log) fwprintf(g_log, L"[%s] %s\n", ok ? L"PASS" : L"FAIL", name);
	}

	bool approx(float a, float b, float eps = 0.001f)
	{
		return std::fabs(a - b) <= eps;
	}

	// 三次贝塞尔在参数 t 处的有符号曲率
	double penCurvature(const ArrowPolyState::PenSeg& s, float t)
	{
		if (s.line) return 0.0;
		const double u = 1.0 - t;
		const double d1x = 3.0 * (u * u * (s.c1.x - s.p0.x) + 2.0 * u * t * (s.c2.x - s.c1.x) + t * t * (s.p1.x - s.c2.x));
		const double d1y = 3.0 * (u * u * (s.c1.y - s.p0.y) + 2.0 * u * t * (s.c2.y - s.c1.y) + t * t * (s.p1.y - s.c2.y));
		const double d2x = 6.0 * (u * (s.c2.x - 2.0 * s.c1.x + s.p0.x) + t * (s.p1.x - 2.0 * s.c2.x + s.c1.x));
		const double d2y = 6.0 * (u * (s.c2.y - 2.0 * s.c1.y + s.p0.y) + t * (s.p1.y - 2.0 * s.c2.y + s.c1.y));
		const double speed = std::hypot(d1x, d1y);
		if (speed < 1e-9) return 0.0;
		return (d1x * d2y - d1y * d2x) / (speed * speed * speed);
	}

	double bezX(const ArrowPolyState::PenSeg& s, float t)
	{
		const double u = 1.0 - t;
		return s.p0.x * u * u * u + 3.0 * s.c1.x * u * u * t + 3.0 * s.c2.x * u * t * t + s.p1.x * t * t * t;
	}
	double bezY(const ArrowPolyState::PenSeg& s, float t)
	{
		const double u = 1.0 - t;
		return s.p0.y * u * u * u + 3.0 * s.c1.y * u * u * t + 3.0 * s.c2.y * u * t * t + s.p1.y * t * t * t;
	}
}

int RunSelfTest()
{
	wchar_t exe[MAX_PATH]{};
	GetModuleFileNameW(nullptr, exe, MAX_PATH);
	std::wstring dir(exe);
	const auto slash = dir.find_last_of(L"\\/");
	if (slash != std::wstring::npos) dir.resize(slash + 1);
	_wfopen_s(&g_log, (dir + L"SnowAir-selftest.log").c_str(), L"w, ccs=UTF-8");

	// —— RoundBox ——
	const D2D1_RECT_F r10x4{ 0.f, 0.f, 10.f, 4.f };
	check(approx(RoundBox::maxRadius(r10x4), 2.f), L"RoundBox::maxRadius(10x4) == 2");

	float radii[4]{ 100.f, 100.f, 100.f, 100.f };
	float clamped[4]{};
	RoundBox::clamp(r10x4, radii, clamped);
	check(clamped[0] <= 2.f && clamped[1] <= 2.f && clamped[2] <= 2.f && clamped[3] <= 2.f,
		L"RoundBox::clamp 半径不超过短边一半");

	float squareRadii[4]{ 0.f, 0.f, 0.f, 0.f };
	check(RoundBox::isSquare(squareRadii), L"RoundBox::isSquare 全零为直角");
	float some[4]{ 2.f, 0.f, 0.f, 0.f };
	check(!RoundBox::isSquare(some), L"RoundBox::isSquare 非零不为直角");

	// —— LinkGeom：轴线交点 ——
	{
		float none[4]{ 0.f, 0.f, 0.f, 0.f };
		D2D1_POINT_2F p{};
		const bool ok = LinkGeom::edgeOnRect(5.f, 3.f, none, 0.f, p);
		check(ok && approx(p.x, 5.f) && approx(p.y, 0.f), L"LinkGeom::edgeOnRect 0° 交于右中点");
		D2D1_POINT_2F e{};
		check(LinkGeom::edgeOnEllipse(5.f, 3.f, 0.f, e) && approx(e.x, 5.f),
			L"LinkGeom::edgeOnEllipse 0° 交于右端点");
	}
	check(LinkGeom::insideRect(r10x4, 5.f, 2.f), L"LinkGeom::insideRect 内部为真");
	check(!LinkGeom::insideRect(r10x4, 11.f, 2.f), L"LinkGeom::insideRect 外部为假");
	{
		const D2D1_POINT_2F c{ 0.f, 0.f };
		check(LinkGeom::insideEllipse(c, 5.f, 3.f, 4.f, 0.f), L"LinkGeom::insideEllipse 内部为真");
		check(!LinkGeom::insideEllipse(c, 5.f, 3.f, 0.f, 3.5f), L"LinkGeom::insideEllipse 外部为假");
	}

	// —— 颜色打包 ——
	check(ShapeBase::packColor(D2D1::ColorF(1.f, 0.5f, 0.f, 0.25f)) == 0xFF800040u,
		L"ShapeBase::packColor 1/0.5/0/0.25 -> 0xFF800040");
	check(ShapeBase::packColor(D2D1::ColorF(0.f, 0.f, 0.f, 0.f)) == 0x00000000u,
		L"ShapeBase::packColor 全零 -> 0");

	// —— 钢笔曲线：曲率贴合样条 ——
	{
		auto buildPen = [](const std::vector<D2D1_POINT_2F>& pts) {
			ArrowPolyState st;
			st.pen = true;
			st.pts = pts;
			st.penStraight.assign(pts.size() - 1, false);
			return st;
		};
		const std::vector<D2D1_POINT_2F> arcPts{
			D2D1::Point2F(119.f, 382.f), D2D1::Point2F(428.f, 102.f),
			D2D1::Point2F(497.f, 101.f), D2D1::Point2F(806.f, 381.f) };
		const auto arcSegs = buildPen(arcPts).penSegments();
		double kMin = 1e9, kMax = 0.f;
		double kFirst = 0.f, kLast = 0.f;
		for (size_t i = 0; i < arcSegs.size(); i++) {
			for (int j = 0; j <= 40; j++) {
				const double k = penCurvature(arcSegs[i], (float)j / 40.f);
				if (i == 0 && j == 0) kFirst = k;
				if (i + 1 == arcSegs.size() && j == 40) kLast = k;
				kMin = std::min(kMin, k);
				kMax = std::max(kMax, k);
			}
		}
		// 前两个锚点与末锚点的外接圆：四点近似共圆时，参考曲线就是这条圆弧
		const double ax = arcPts[0].x, ay = arcPts[0].y;
		const double bx = arcPts[1].x, by = arcPts[1].y;
		const double cx = arcPts[3].x, cy = arcPts[3].y;
		const double dd = 2.0 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
		const double ox = ((ax * ax + ay * ay) * (by - cy) + (bx * bx + by * by) * (cy - ay)
			+ (cx * cx + cy * cy) * (ay - by)) / dd;
		const double oy = ((ax * ax + ay * ay) * (cx - bx) + (bx * bx + by * by) * (ax - cx)
			+ (cx * cx + cy * cy) * (bx - ax)) / dd;
		const double radius = std::hypot(ax - ox, ay - oy);
		double dev = 0.f;
		for (const auto& sg : arcSegs)
			for (int j = 1; j <= 40; j++) {
				const double px = bezX(sg, (float)j / 40.f), py = bezY(sg, (float)j / 40.f);
				dev = std::max(dev, std::abs(std::hypot(px - ox, py - oy) - radius));
			}
		wchar_t buf[256]{};
		swprintf_s(buf, L"钢笔曲线 切线迭代求解 κ∈[%.5f,%.5f] 端 κ %.5f/%.5f 偏离外接圆 %.2fpx",
			kMin, kMax, kFirst, kLast, dev);
		check(kMin >= 0.f && kMax < 0.005f && dev < 30.f, buf);

		// 共线锚点必须仍是直线
		const std::vector<D2D1_POINT_2F> linePts{
			D2D1::Point2F(0.f, 0.f), D2D1::Point2F(100.f, 0.f),
			D2D1::Point2F(200.f, 0.f), D2D1::Point2F(300.f, 0.f) };
		const auto lineSegs = buildPen(linePts).penSegments();
		double lineDev = 0.f;
		for (const auto& sg : lineSegs)
			for (int j = 1; j <= 40; j++)
				lineDev = std::max(lineDev, std::fabs(bezY(sg, (float)j / 40.f)));
		swprintf_s(buf, L"钢笔曲线 共线锚点保持直线 法向偏移 %.3fpx", lineDev);
		check(lineDev < 0.05f, buf);

		// 预览跟手：同一笔连续下点，已定型段应当完全不动
		ArrowPolyState warm = buildPen(arcPts);
		const auto still = warm.penSegments();
		warm.penPreview = true;
		warm.penPreviewPt = D2D1::Point2F(806.f, 389.f);
		warm.penCacheValid = false;
		const auto moved = warm.penSegments();
		double drift = 0.f;
		for (size_t i = 0; i + 1 < still.size() && i + 1 < moved.size(); i++)
			for (int j = 0; j <= 20; j++) {
				drift = std::max(drift, std::hypot(
					bezX(moved[i], (float)j / 20.f) - bezX(still[i], (float)j / 20.f),
					bezY(moved[i], (float)j / 20.f) - bezY(still[i], (float)j / 20.f)));
			}
		swprintf_s(buf, L"钢笔曲线 预览移动 8px 定型段最大位移 %.2fpx", drift);
		check(drift < 1.0f, buf);
	}

	if (g_log)
	{
		fwprintf(g_log, L"\n失败项：%d\n", g_fail);
		fclose(g_log);
	}
	return g_fail;
}

