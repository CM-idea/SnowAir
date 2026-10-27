#pragma once
#include <include/Ling.h>
#include "ShapeBase.h"
#include "ArrowPoly.h"

class ShapeArrow : public ShapeBase
{
public:
	// headOverride：强制指定箭头头部样式（-1 = 跟随属性栏）。
	// dimension=true：「尺寸」——固定标注箭头样式 + 只有两个端点（不提供中间加折点的控点）
	explicit ShapeArrow(AnnotHost* win, int headOverride = -1, bool dimension = false);
	~ShapeArrow();
	void paint(ID2D1DeviceContext* ctx) override;
	void paintDragger(ID2D1DeviceContext* ctx) override;
	void applyStyle() override;
	void syncToolFromShape() const override;
	UINT32 styleColorValue() const override;
	void mouseDrag(const float x, const float y) override;
	void mouseDown(const float x, const float y) override;
	void mouseUp(const float x, const float y) override;
	void mouseMove(const float x, const float y) override;
	void mouseWheel(const float x, const float y, const short delta) override;
	void setCursor() override;
	bool hitErase(const float x, const float y) override;
	bool toggleFreeEndpoint(float x, float y);
	void resetFree();
	// —— 连线（箭头这一侧，逻辑都在 ShapeLink）——
	bool hasLink() const override;
	bool linkStartPoint(D2D1_POINT_2F& out) const override;
	bool linkButtonPos(D2D1_POINT_2F& out) const override;
	bool hitLinkButton(const float x, const float y, const float dpi) const override;
	void syncLinks() override;
	// 「线段中间打字」：大箭头不参与；折线长度中点（图像坐标）
	bool bigHead() const;
	bool shaftMidPoint(float& mx, float& my) const;
	float shaftLength() const;   // 折线总长（图像像素）——「尺寸」用它换算物理尺寸
	float shaftAngle() const;   // 起点→终点方向（弧度）。线上文字一律横排，现在没人用它
	float stylePrimarySize() const override { return strokeWidth; }
	std::vector<float> editSnapshot() const override;
	void applyEditSnapshot(const std::vector<float>& state) override;
	bool translateBy(float dx, float dy) override;
	bool isDimension() const { return isDimension_; }
private:
	void rebuild();
	void makeBigPath();
	void ensureMinLength();
	float arrowSize() const;
	void ensureStrokeStyle();
	void drawPolyline(ID2D1DeviceContext* ctx, D2D1_POINT_2F* tipFrom = nullptr, float headBack = 0.f);
	void constrainToEightDirections(const float anchorX, const float anchorY, const float mouseX, const float mouseY, float& targetX, float& targetY);
	bool isBig() const;
	bool isFun() const;
	bool hitBody(float x, float y);
	int hoverFromHit(const ArrowHitInfo& h) const;
private:
	ArrowPolyState poly;
	ArrowHitInfo lastHit{};
	Microsoft::WRL::ComPtr<ID2D1PathGeometry> bigPath;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
	Microsoft::WRL::ComPtr<ID2D1StrokeStyle> strokeStyle;
	Microsoft::WRL::ComPtr<ID2D1StrokeStyle> headStrokeStyle;
	float strokeWidth{ 0 };
	int headStyle{ 0 };
	// 趣味箭头：每条箭头的圈给一点随机缩放/旋转，避免千篇一律
	float funLoopScale{ 1.f };
	float funLoopRot{ 0.f };
	bool isDash{ false };
	bool isRound{ false };
	bool creating{ false };
	bool isDimension_{ false };   // 尺寸：两点控件（不加折点）
};
