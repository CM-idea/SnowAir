#pragma once
#include <include/Ling.h>
#include "ShapeBase.h"
#include "ArrowPoly.h"

// 箭头属性栏「线段」：与箭头共用折线控点
class ShapeLine : public ShapeBase
{
public:
	ShapeLine(AnnotHost* win);
	~ShapeLine();
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
	// —— 连线（线段这一侧，逻辑都在 ShapeLink）——
	bool hasLink() const override;
	bool linkStartPoint(D2D1_POINT_2F& out) const override;
	bool linkButtonPos(D2D1_POINT_2F& out) const override;
	bool hitLinkButton(const float x, const float y, const float dpi) const override;
	void syncLinks() override;
	// 「线段中间打字」：线段没有大箭头形态；折线长度中点（图像坐标）
	bool bigHead() const { return false; }
	bool shaftMidPoint(float& mx, float& my) const;
	float shaftAngle() const;   // 起点→终点方向（弧度）。线上文字一律横排，现在没人用它
	float stylePrimarySize() const override { return strokeWidth; }
private:
	void ensureStrokeStyle();
	void ensureMinLength();
	void constrainToEightDirections(const float anchorX, const float anchorY, const float mouseX, const float mouseY, float& targetX, float& targetY);
	bool hitBody(float x, float y);
	int hoverFromHit(const ArrowHitInfo& h) const;
private:
	ArrowPolyState poly;
	ArrowHitInfo lastHit{};
	Microsoft::WRL::ComPtr<ID2D1StrokeStyle> strokeStyle;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
	float strokeWidth{ 1.f };
	bool isDash{ false };
	bool isRound{ false };
	bool creating{ false };   // 本次按下是不是"新画一条"（决定连线端点离开形状时脱不脱开）
};
