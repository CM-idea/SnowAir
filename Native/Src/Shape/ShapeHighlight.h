#pragma once
#include "ShapeArea.h"

class ShapeHighlight : public ShapeArea
{
public:
	ShapeHighlight(AnnotHost* win);
	void paint(ID2D1DeviceContext* ctx) override;
	void applyStyle();
	const D2D1_RECT_F& hole() const { return rect; }
	bool ellipseHole() const { return isEllipse; }
	bool bordered() const { return showBorder; }
	float borderWidth() const { return strokeWidth; }
	float stylePrimarySize() const override { return strokeWidth; }
	ID2D1SolidColorBrush* borderBrush() const { return brushBorder.Get(); }
	// 高亮矩形支持 AI 式四角圆角点（椭圆高亮没有圆角可言）
	bool cornerDotsEnabled() const override { return !isEllipse; }
	static D2D1_RECT_F viewportOf(AnnotHost* win);
	static void paintMerged(ID2D1DeviceContext* ctx, AnnotHost* win,
		const std::vector<ShapeHighlight*>& holes);
private:
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushBorder;
	float strokeWidth{ 2.f };
	bool isEllipse{ false };
	bool showBorder{ false };
};
