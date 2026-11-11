#include "pch.h"
#include "SelfTest.h"
#include "ConfigMerge.h"
#include "Shape/RoundBox.h"
#include "Shape/LinkGeom.h"
#include "Shape/ShapeBase.h"
#include "Shape/ArrowPoly.h"
#include "Win/PixLens.h"
#include "Win/CursorImage.h"
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

	// —— 放大镜落位：只避让、不记忆（默认右下，放不下才换边，放得下立刻回右下） ——
	{
		const float W = 128.f, H = 190.f, G = 12.f;
		const float AL = 0.f, AT = 0.f, AR = 2560.f, AB = 1080.f;
		auto inside = [&](const PixLensBox& b) {
			return b.x >= AL - 0.5f && b.y >= AT - 0.5f
				&& b.x + W <= AR + 0.5f && b.y + H <= AB + 0.5f;
		};
		// 外壳是「取样点旁边的取色窗」，压住光标就等于挡住了要看的那一格
		auto coversCursor = [&](const PixLensBox& b, float cx, float cy) {
			return cx > b.x && cx < b.x + W && cy > b.y && cy < b.y + H;
		};
		auto run = [&](float cx, float cy, int wantQuad, const wchar_t* name) {
			PixLensBox b{};
			const int q = pixLensQuadrant(cx, cy, W, H, G, AL, AT, AR, AB, &b);
			wchar_t buf[256]{};
			swprintf_s(buf, L"放大镜落位 %s 象限=%d 盒=(%.0f,%.0f)", name, q, b.x, b.y);
			check(q == wantQuad && inside(b) && !coversCursor(b, cx, cy), buf);
		};

		run(1280.f, 540.f, 0, L"屏幕中央→右下");
		run(2540.f, 540.f, 1, L"右边→左下");
		run(1280.f, 1060.f, 2, L"下边→右上");
		run(2540.f, 1060.f, 3, L"右下角→左上");
		run(5.f, 5.f, 0, L"左上角→右下");
		run(1280.f, 5.f, 0, L"上边→右下");
		run(5.f, 540.f, 0, L"左边→右下");
		// 只避让不记忆：从右下角挪回屏幕中央必须立刻回右下
		run(2540.f, 1060.f, 3, L"右下角（避让）");
		run(1280.f, 540.f, 0, L"离开边缘立刻回右下");

		// 全屏扫描：任何位置外壳都要整块留在显示器内，且不压住取样点
		bool sweepOk = true;
		for (float cx = 1.f; cx < 2560.f; cx += 37.f)
			for (float cy = 1.f; cy < 1080.f; cy += 41.f) {
				PixLensBox b{};
				pixLensQuadrant(cx, cy, W, H, G, AL, AT, AR, AB, &b);
				if (!inside(b) || coversCursor(b, cx, cy)) sweepOk = false;
			}
		check(sweepOk, L"放大镜落位 全屏扫描均不出界、不压光标");

		// 显示器比外壳还小：没有完整解，返回 -1，但仍要夹在区域内
		PixLensBox tiny{};
		const int tq = pixLensQuadrant(50.f, 50.f, W, H, G, 0.f, 0.f, 100.f, 100.f, &tiny);
		check(tq == -1 && tiny.x >= 0.f && tiny.y >= 0.f && tiny.x <= 100.f && tiny.y <= 100.f,
			L"放大镜落位 区域过小时夹回区域内");
	}

	// —— 光标位图解析：单色（工字）与彩色光标都必须能解出可见像素 ——
	{
		HDC screenDc = GetDC(nullptr);
		HDC memDc = screenDc ? CreateCompatibleDC(screenDc) : nullptr;
		auto tryCursor = [&](LPCWSTR id, const wchar_t* name) {
			HCURSOR cur = LoadCursorW(nullptr, id);
			ICONINFO ii{};
			int w = 0, h = 0;
			std::vector<BYTE> px;
			const bool ok = cur && GetIconInfo(cur, &ii)
				&& CursorImage::ChooseSize(ii, &w, &h)
				&& memDc && CursorImage::Extract(memDc, ii, w, h, px)
				&& px.size() == (size_t)w * h * 4;
			if (ii.hbmColor) DeleteObject(ii.hbmColor);
			if (ii.hbmMask) DeleteObject(ii.hbmMask);
			wchar_t buf[192]{};
			swprintf_s(buf, L"光标位图 %s 解析 %dx%d 且有不透明像素", name, w, h);
			check(ok, buf);
		};
		tryCursor(IDC_IBEAM, L"工字");
		tryCursor(IDC_ARROW, L"箭头");
		tryCursor(IDC_HAND, L"手型");
		tryCursor(IDC_SIZEALL, L"移动");
		if (memDc) DeleteDC(memDc);
		if (screenDc) ReleaseDC(nullptr, screenDc);
	}

	// —— 配置三路合并：不同键的修改共存、同键最后写入者赢、删除生效 ——
	{
		using namespace winrt::Windows::Data::Json;
		const auto parse = [](const wchar_t* s) { return JsonObject::Parse(s); };
		auto latest = parse(LR"({"common":{"lang":"zh","autoStart":true},"features":{"tray":1},"other":"keep"})");
		auto base = parse(LR"({"common":{"lang":"zh","autoStart":true},"features":{"tray":1}})");
		auto mine = parse(LR"({"common":{"lang":"en","autoStart":true},"features":{"tray":2},"toolPin":{"x":1}})");
		ConfigMerge::MergeObjects(latest, base, mine);
		check(std::wstring{ latest.GetNamedObject(L"common").GetNamedString(L"lang", L"") } == L"en",
			L"配置合并 自己改的键生效");
		check(latest.GetNamedObject(L"features").GetNamedNumber(L"tray") == 2,
			L"配置合并 同键最后写入者赢");
		check(std::wstring{ latest.GetNamedString(L"other", L"") } == L"keep",
			L"配置合并 保留对方/其它键");
		check(latest.GetNamedObject(L"toolPin").GetNamedNumber(L"x") == 1,
			L"配置合并 新增对象键");

		auto latest2 = parse(LR"({"features":{"a":1},"keep":true})");
		auto base2 = parse(LR"({"features":{"a":1}})");
		auto mine2 = parse(LR"({"keep":true})");
		ConfigMerge::MergeObjects(latest2, base2, mine2);
		check(!latest2.HasKey(L"features") && latest2.GetNamedBoolean(L"keep"),
			L"配置合并 删除的键生效");
	}

	if (g_log)
	{
		fwprintf(g_log, L"\n失败项：%d\n", g_fail);
		fclose(g_log);
	}
	return g_fail;
}
