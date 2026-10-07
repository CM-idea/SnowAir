#pragma once
#include <include/Ling.h>
#include <functional>
#include "ShapeBase.h"
class ShapeEllipse : public ShapeBase
{
public:
	ShapeEllipse(AnnotHost* win);
	~ShapeEllipse();
	void paint(ID2D1DeviceContext* ctx) override;
	void paintDragger(ID2D1DeviceContext* ctx) override;
	void mouseDrag(const float x, const float y) override;
	void mouseDown(const float x, const float y) override;
	void mouseUp(const float x, const float y) override;
	void mouseMove(const float x, const float y) override;
	void mouseWheel(const float x, const float y, const short delta) override;
	void setCursor() override;
	bool hitErase(const float x, const float y) override;
	// —— 连线：椭圆可以当连线目标（见 ShapeLink）——
	bool isLinkTarget() const override { return true; }
	bool hitLinkBody(const float x, const float y) const override;
	D2D1_POINT_2F linkCenter() const override { return centerPt(); }
	float linkRotation() const override { return angle; }
	bool linkEdgePoint(const float localAngle, D2D1_POINT_2F& out) const override;
	float stylePrimarySize() const override { return strokeWidth; }
private:
	void updateDraggers();
	void syncEllipseFromRect();
	void toLocal(float x, float y, float& lx, float& ly) const;
	D2D1_POINT_2F centerPt() const;
	void withRotation(ID2D1DeviceContext* ctx, const std::function<void()>& draw);
private:
	std::vector<D2D1_RECT_F> draggers;
	D2D1_RECT_F rect{ 0,0,0,0 };
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
	Microsoft::WRL::ComPtr<ID2D1StrokeStyle> strokeStyle;
	float strokeWidth{ 1.f }, pressX{ 0.f }, pressY{ 0.f };
	float cx{ 0.f }, cy{ 0.f }, rx{ 0.f }, ry{ 0.f };
	float angle{ 0.f };
	// 角外一圈旋转：光标按角挑字形，拖动按增量转
	int rotateCorner{ 0 };
	float rotateStartAngle{ 0.f }, rotateStartAtan{ 0.f };
	bool isFill{ false };
	bool isDash{ false };
};
