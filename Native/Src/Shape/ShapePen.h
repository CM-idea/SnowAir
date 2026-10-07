#pragma once
#include <include/Ling.h>
#include <cstdint>
#include <functional>
#include <vector>
#include "ShapeBase.h"
#include "LaserStroke.h"

// 自由画笔：落笔折线；选中态与矩形同一套控点（选框 + 四角 + 旋转柄）
class ShapePen : public ShapeBase
{
public:
	ShapePen(AnnotHost* win);
	~ShapePen();
	void paint(ID2D1DeviceContext* ctx) override;
	void paintDragger(ID2D1DeviceContext* ctx) override;
	void mouseDrag(const float x, const float y) override;
	void mouseDown(const float x, const float y) override;
	void mouseUp(const float x, const float y) override;
	void mouseMove(const float x, const float y) override;
	void mouseWheel(const float x, const float y, const short delta) override;
	void setCursor() override;
	bool hitErase(const float x, const float y) override;
	bool isEphemeral() override { return isFade; }
	// 渐隐画笔：定时器按点寿命收细，整段消失后返回 true 由宿主移除
	bool tickFade();
	bool needsFadeTick() const;
	float stylePrimarySize() const override { return strokeWidth; }
private:
	void ensureStrokeStyle();
	void rebuildPath();
	void rebuildDashSpine();
	void rebuildLaserPath(int64_t now);
	void appendSample(float x, float y, bool force = false);
	int64_t nowMs() const;
	void syncRectFromPts();
	void scalePtsToRect(const D2D1_RECT_F& oldR, const D2D1_RECT_F& newR);
	void updateDraggers();
	D2D1_POINT_2F boxCenter() const;
	void toLocal(float x, float y, float& lx, float& ly) const;
	void withRotation(ID2D1DeviceContext* ctx, const std::function<void()>& draw);
private:
	std::vector<D2D1_POINT_2F> pts; // 未旋转局部坐标（与 rect 同空间）
	std::vector<int64_t> times; // 相对落笔起点 ms（软笔速度）
	std::vector<D2D1_RECT_F> draggers;
	D2D1_RECT_F rect{ 0,0,0,0 };
	Microsoft::WRL::ComPtr<ID2D1PathGeometry> path; // 实线=填充轮廓；虚线=脊线
	Microsoft::WRL::ComPtr<ID2D1StrokeStyle> strokeStyle;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
	float strokeWidth{ 3.f };
	float pressX{ 0.f }, pressY{ 0.f };
	float boxAngle{ 0.f }; // 弧度，顶部为 0
	float smoothX{ 0.f }, smoothY{ 0.f }; // EMA 跟手点（硬笔/虚线）
	// 角外一圈旋转：光标按角挑字形，拖动按增量转
	int rotateCorner{ 0 };
	float rotateStartAngle{ 0.f }, rotateStartAtan{ 0.f };
	int64_t clock0{ 0 };
	bool isDash{ false };
	bool isSoft{ false };
	bool isFade{ false };
	bool fadeDone{ false };
	bool pathFilled{ false }; // 实线走 FillGeometry
	bool settled{ false }; // 落笔结束后才用矩形系控点
	// 渐隐画笔：逐点带时间戳的轨迹 + 每帧按寿命重建的填充轮廓
	LaserStroke::Trail laser;
	Microsoft::WRL::ComPtr<ID2D1PathGeometry> laserPath;
};
