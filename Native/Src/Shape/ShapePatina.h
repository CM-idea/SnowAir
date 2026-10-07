#pragma once
#include "ShapeArea.h"

class ShapePatina : public ShapeArea
{
public:
	ShapePatina(AnnotHost* win);
	void paint(ID2D1DeviceContext* ctx) override;
	bool isViewportFilter() override { return true; };
	bool hitErase(const float x, const float y) override { (void)x; (void)y; return false; };
	void mouseMove(const float x, const float y) override { (void)x; (void)y; hoverDraggerIndex = -1; };
	void setViewport(const D2D1_RECT_F& vp);
	void applyStyle();
private:
	void rebuild();
	Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushBusy;
	int strength{ 15 };
	bool useGreen{ true };
	bool showWm{ true };
	int wmPlan{ 0 };
	int wmSize{ 20 };
	bool dirty{ true };
};
