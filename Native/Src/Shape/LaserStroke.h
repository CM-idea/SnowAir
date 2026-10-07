#pragma once
#include <d2d1.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>

// 渐隐画笔（激光笔）：
//   每个轨迹点带时间戳，按“点龄”逐点收细，
//   不必等整段画完，尾部先消失；两头顶端是圆帽，粗细由 width 决定。
namespace LaserStroke
{
	// 落点后仍保持全粗的时间
	inline constexpr int64_t kHoldMs{ 200 };
	// 单点从全粗收到消失的时间（合计寿命约 1s）
	inline constexpr int64_t kFadeMs{ 780 };
	// 跟手平滑：0.58 = QT kStreamline
	inline constexpr float kStreamline{ 0.58f };
	// 未稳定段（tail）的最大长度，超过就并入 stable
	inline constexpr float kMaxTailLength{ 100.f };

	struct Point
	{
		float x{ 0.f };
		float y{ 0.f };
		// 绝对时间戳 ms（QT 里存在 laser-pointer 的 pressure 字段）
		float t{ 0.f };
	};

	struct Trail
	{
		std::vector<Point> original; // 去重后的原始落点：只用于点数与最新时间
		std::vector<Point> stable;   // 已稳定段（平滑 + 补点后）
		std::vector<Point> tail;     // 未稳定段
		float width{ 4.f };          // 笔迹直径（图像像素，已含 dpi）
		float unit{ 1.f };           // dpi：重采样 / 稳定距离阈值按它缩放
		bool keepHead{ true };       // 绘制中：笔头维持全粗的圆
		bool fresh{ true };
		int64_t closedAt{ 0 };       // 抬笔时刻；0 = 仍在画

		int pointCount() const { return (int)original.size(); }
	};

	// 落点：去重 → 平滑 → 距离过大时按 5px 补点 → tail 过长则稳定
	void addPoint(Trail& trail, float x, float y, int64_t now);
	// 抬笔：稳定尾部、收起笔头圆、记下时刻
	void closeTrail(Trail& trail, int64_t now);
	// 最新点是否还在寿命内（hold + fade）
	bool stillVisible(const Trail& trail, int64_t now);
	// 逐点寿命 → 封闭描边轮廓；全消失 / 点数不足时返回空
	std::vector<Point> outline(const Trail& trail, int64_t now);
	// 轮廓 → 填充几何（中点二次曲线闭合，对齐 QT appendSmoothClosed）
	Microsoft::WRL::ComPtr<ID2D1PathGeometry> buildGeometry(
		ID2D1Factory* factory, const Trail& trail, int64_t now);
}
