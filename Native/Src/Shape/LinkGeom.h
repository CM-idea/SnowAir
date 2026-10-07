#pragma once
#include <d2d1.h>
#include <algorithm>
#include <cfloat>
#include <cmath>

// 连线的**纯几何**：形状中心 → 某方向的边缘交点、点是否在形状里。
// 单独一个头文件是为了能脱离 Ling/窗口直接编探针（_layout_probe/linkgeom_probe.cpp）。
// 所有函数都在"形状自己的局部坐标系"里算（矩形轴对齐、椭圆轴对齐），
// 旋转由调用方（ShapeRect/ShapeEllipse）自己换算。
namespace LinkGeom
{
	inline bool insideRect(const D2D1_RECT_F& r, float x, float y)
	{
		return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
	}

	// (x,y) 是否在椭圆里（绝对坐标）
	inline bool insideEllipse(const D2D1_POINT_2F& c, float rx, float ry, float x, float y)
	{
		if (rx < 0.5f || ry < 0.5f) return false;
		const float nx = (x - c.x) / rx;
		const float ny = (y - c.y) / ry;
		return nx * nx + ny * ny <= 1.f;
	}

	// 从矩形中心朝 angleRad 方向的边缘交点（**相对中心**的坐标）。
	// hw/hh = 半宽半高；radius[4] = 四角圆角（TL/TR/BR/BL，和 ShapeRect::radius 同序）。
	// 交点落在圆角方块里时换成圆弧上的交点，于是连线端点贴着圆角收（不会停在"假角"上）
	inline bool edgeOnRect(float hw, float hh, const float radius[4], float angleRad, D2D1_POINT_2F& out)
	{
		if (hw < 0.5f || hh < 0.5f) return false;
		const float ux = std::cos(angleRad), uy = std::sin(angleRad);
		float t = FLT_MAX;
		if (std::fabs(ux) > 1e-6f) t = hw / std::fabs(ux);
		if (std::fabs(uy) > 1e-6f) t = std::min(t, hh / std::fabs(uy));
		if (t == FLT_MAX) return false;
		float lx = ux * t, ly = uy * t;

		const float sx = lx < 0.f ? -1.f : 1.f;
		const float sy = ly < 0.f ? -1.f : 1.f;
		// 角序：0=TL 1=TR 2=BL 3=BR（左半 + 上半 = TL …）
		const int slot = (sx < 0.f ? 0 : 1) + (sy < 0.f ? 0 : 2);
		// (std::min) 加括号是为了防 windows.h 的 min/max 宏（本头文件可能被没定义 NOMINMAX 的 TU 包含）
		const float r = (std::clamp)(radius[slot], 0.f, (std::min)(hw, hh));
		if (r > 0.5f && std::fabs(lx) > hw - r && std::fabs(ly) > hh - r) {
			// 射线与"圆角圆弧所在的圆"求交：|t·u − a| = r，取正根
			const float ax = sx * (hw - r), ay = sy * (hh - r);
			const float b = ux * ax + uy * ay;
			const float cc = ax * ax + ay * ay - r * r;
			const float disc = b * b - cc;
			if (disc >= 0.f) {
				const float tt = b + std::sqrt(disc);
				if (tt > 0.f) { lx = ux * tt; ly = uy * tt; }
			}
		}
		out = D2D1_POINT_2F{ lx, ly };
		return true;
	}

	// 从椭圆中心朝 angleRad 方向的边缘交点（**相对中心**的坐标）
	inline bool edgeOnEllipse(float rx, float ry, float angleRad, D2D1_POINT_2F& out)
	{
		if (rx < 0.5f || ry < 0.5f) return false;
		const float u = std::cos(angleRad), v = std::sin(angleRad);
		const float k = std::sqrt((u / rx) * (u / rx) + (v / ry) * (v / ry));
		if (k < 1e-6f) return false;
		out = D2D1_POINT_2F{ u / k, v / k };
		return true;
	}

	// 把"相对中心"的局部点按角度转回世界坐标
	inline D2D1_POINT_2F rotateAbout(const D2D1_POINT_2F& c, const D2D1_POINT_2F& local, float angleRad)
	{
		const float ca = std::cos(angleRad), sa = std::sin(angleRad);
		return D2D1_POINT_2F{
			c.x + local.x * ca - local.y * sa,
			c.y + local.x * sa + local.y * ca };
	}
}
