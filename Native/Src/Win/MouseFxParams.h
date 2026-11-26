#pragma once
// 「鼠标单击效果」的统一参数：屏幕预览（SelectionChrome）、录制补画（MagnifierDraw）、
// 设置页单击预览三处必须完全一致，否则同一个效果在屏幕 / 视频 / 预览里的大小与粗细会不同。
//
// 单色波纹：半径从 8 线性扩到 50（逻辑像素，按所在屏 dpi 缩放），520ms 渐隐；
// 线宽 5px → 2px，整体透明度 50% → 0。

#include <windows.h>
#include <algorithm>

namespace MouseFx
{
	constexpr ULONGLONG rippleLifeMs{ 520 };
	constexpr float rippleRadiusStart{ 8.f };
	constexpr float rippleRadiusEnd{ 50.f };
	constexpr float rippleWidthStart{ 5.f };
	constexpr float rippleWidthEnd{ 2.f };
	constexpr float rippleOpacity{ 0.5f };

	// 高亮提醒：光标下垫的半透明圆（逻辑半径 25 → 直径 50），固定 20% 不透明度
	constexpr float highlightRadius{ 25.f };
	constexpr float highlightAlpha{ 0.20f };

	// 线条跟随：整条渐隐笔迹 50% 半透明（与演示画布的渐隐画笔同形）
	constexpr float trailOpacity{ 0.5f };

	// k = 已过时间 / rippleLifeMs，范围 0..1
	inline float rippleRadius(float k) { return rippleRadiusStart + (rippleRadiusEnd - rippleRadiusStart) * k; }
	inline float rippleWidth(float k) { return (std::max)(1.f, rippleWidthStart + (rippleWidthEnd - rippleWidthStart) * k); }
	inline float rippleAlpha(float k) { return (1.f - k) * rippleOpacity; }
}
