#pragma once
#include <d2d1.h>
#include <cstdint>
#include <vector>

// 渐隐画笔（激光笔）：轨迹点带时间戳，粗细按点龄收细，队尾始终呈锥形；
// 整条轨迹合成一个几何后一次填充，重叠处不叠加透明度。
namespace LaserStroke
{
	// 寿命：最新点从全粗衰减到消失的时间
	inline constexpr int64_t kLifeMs{ 600 };
	// 队列上限：仅作内存保护，拖尾长度由寿命决定
	inline constexpr int kMaxPoints{ 512 };
	// 跟手平滑：新采样点向落点靠拢的比例
	inline constexpr float kFollow{ 0.6f };

	struct Point
	{
		float x{ 0.f };
		float y{ 0.f };
		int64_t t{ 0 };          // 绝对时间戳 ms
		bool connected{ false }; // 是否与上一点相连
	};

	struct Trail
	{
		std::vector<Point> points;
		float width{ 4.f };      // 笔迹直径（图像像素，已含 dpi）
		// 上一个原始落点，用于过滤原地重复采样
		bool hasLast{ false };
		float lastX{ 0.f };
		float lastY{ 0.f };

		int pointCount() const { return (int)points.size(); }
	};

	// 落点：淘汰过期点、过滤重复采样、平滑后入队
	void addPoint(Trail& trail, float x, float y, int64_t now);
	// 是否仍有可见像素（点数不足或最新点已过期即为不可见）
	bool visible(const Trail& trail, int64_t now);
	// 按当前时刻一次填充整条轨迹（相邻段取并集）
	void paint(ID2D1DeviceContext* ctx, ID2D1Brush* brush, const Trail& trail, int64_t now);
}
