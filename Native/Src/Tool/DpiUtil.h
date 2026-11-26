#pragma once
#include <Windows.h>
#include <shellscalingapi.h>

// 统一 DPI 取数口：任何显示在屏幕上的 UI 都以它所在显示器为准。
// 返回值是缩放倍数（1.0 = 96 DPI），可直接与逻辑值相乘。
namespace DpiUtil
{
	inline float px(float logic, float dpi)
	{
		return logic * dpi;
	}

	inline float dpiForMonitor(HMONITOR monitor)
	{
		if (!monitor) return 1.f;
		UINT x = 96, y = 96;
		if (SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &x, &y)) && x > 0)
			return x / 96.f;
		return 1.f;
	}

	inline float dpiForWindow(HWND hwnd)
	{
		if (!hwnd) return 1.f;
		const UINT d = GetDpiForWindow(hwnd);
		if (d > 0) return d / 96.f;
		return dpiForMonitor(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST));
	}

	inline float dpiForRect(const RECT& screenRect)
	{
		return dpiForMonitor(MonitorFromRect(&screenRect, MONITOR_DEFAULTTONEAREST));
	}

	inline float dpiForPoint(const POINT& screenPt)
	{
		return dpiForMonitor(MonitorFromPoint(screenPt, MONITOR_DEFAULTTONEAREST));
	}
}
