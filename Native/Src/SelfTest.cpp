#include "pch.h"
#include "SelfTest.h"
#include "Shape/RoundBox.h"
#include "Shape/LinkGeom.h"
#include "Shape/ShapeBase.h"
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

	if (g_log)
	{
		fwprintf(g_log, L"\n失败项：%d\n", g_fail);
		fclose(g_log);
	}
	return g_fail;
}

