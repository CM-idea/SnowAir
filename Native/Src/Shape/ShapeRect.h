#pragma once
#include <include/Ling.h>
#include <functional>
#include "ShapeBase.h"
class ShapeRect : public ShapeBase
{
public:
	ShapeRect(AnnotHost* win);
	~ShapeRect();
	void paint(ID2D1DeviceContext* ctx) override;
	void paintDragger(ID2D1DeviceContext* ctx) override;
	void mouseDrag(const float x, const float y) override;
	void mouseDown(const float x, const float y) override;
	void mouseUp(const float x, const float y) override;
	void mouseMove(const float x, const float y) override;
	void mouseWheel(const float x, const float y, const short delta) override;
	void setCursor() override;
	bool hitErase(const float x, const float y) override;
	// —— 连线：矩形可以当连线目标（见 ShapeLink）——
	bool isLinkTarget() const override { return true; }
	bool hitLinkBody(const float x, const float y) const override;
	D2D1_POINT_2F linkCenter() const override;
	float linkRotation() const override { return angle; }
	bool linkEdgePoint(const float localAngle, D2D1_POINT_2F& out) const override;
	float stylePrimarySize() const override { return strokeWidth; }
private:
	void updateDraggers();
	void toLocal(float x, float y, float& lx, float& ly) const;
	D2D1_POINT_2F center() const;
	void withRotation(ID2D1DeviceContext* ctx, const std::function<void()>& draw);
	// 圆角：四角独立（对齐 AI 的 live corner widget）
	Microsoft::WRL::ComPtr<ID2D1PathGeometry> buildShape() const;
private:
	std::vector<D2D1_RECT_F> draggers;
	D2D1_RECT_F rect{ 0,0,0,0 };
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
	Microsoft::WRL::ComPtr<ID2D1StrokeStyle> strokeStyle;
	float strokeWidth{ 1.f }, pressX{ 0.f }, pressY{ 0.f };
	float angle{ 0.f }; // 弧度，顶部为 0（对齐 QT）
	// 角外一圈旋转：光标按角挑字形，拖动按增量转
	int rotateCorner{ 0 };
	float rotateStartAngle{ 0.f }, rotateStartAtan{ 0.f };
	// 四角圆角半径 TL/TR/BR/BL（物理像素）
	float radius[4]{ 0.f, 0.f, 0.f, 0.f };
	// 圆角点：dotCorner = 已被"单击选中"的角（0/2/4/6），-1 = 无
	int dotCorner{ -1 };
	int dotPressIndex{ -1 }; // 本次按下的圆角点（10..13），mouseUp 判定"单击 or 拖动"
	bool dotDragAll{ true };
	float dotStart[4]{ 0.f, 0.f, 0.f, 0.f };
	float dotPressX{ 0.f }, dotPressY{ 0.f };
	bool isFill{ false };
	bool isDash{ false };
};
