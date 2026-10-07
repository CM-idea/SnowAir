#pragma once
#include <include/Ling.h>
#include "ShapeBase.h"
#include "ArrowPoly.h"

class ShapeArrow : public ShapeBase
{
public:
	ShapeArrow(AnnotHost* win);
	~ShapeArrow();
	void paint(ID2D1DeviceContext* ctx) override;
	void paintDragger(ID2D1DeviceContext* ctx) override;
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
	float shaftAngle() const;   // 起点→终点方向（弧度）。线上文字一律横排，现在没人用它
	float stylePrimarySize() const override { return strokeWidth; }
private:
	void rebuild();
	void makeBigPath();
	void ensureMinLength();
	float arrowSize() const;
	void ensureStrokeStyle();
	void drawPolyline(ID2D1DeviceContext* ctx);
	void constrainToEightDirections(const float anchorX, const float anchorY, const float mouseX, const float mouseY, float& targetX, float& targetY);
	bool isBig() const;
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
	bool isDash{ false };
	bool isRound{ false };
	bool creating{ false };
};
