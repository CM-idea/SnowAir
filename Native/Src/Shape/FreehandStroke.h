#pragma once
#include <d2d1.h>
#include <wrl/client.h>
#include <vector>
#include <cstdint>

// 软笔速度/转折→压力变粗细；硬笔等宽圆头；一次填充
namespace FreehandStroke
{
	Microsoft::WRL::ComPtr<ID2D1PathGeometry> outline(
		ID2D1Factory* factory,
		const std::vector<D2D1_POINT_2F>& pts,
		float strokeWidth,
		bool softPen,
		const std::vector<int64_t>& times = {});
}
