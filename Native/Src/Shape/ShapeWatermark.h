#pragma once
#include "ShapeArea.h"

class ShapeWatermark : public ShapeArea
{
public:
	ShapeWatermark(AnnotHost* win);
	void paint(ID2D1DeviceContext* ctx) override;
	bool isViewportFilter() override { return true; };
	bool hitErase(const float x, const float y) override { (void)x; (void)y; return false; };
	void mouseMove(const float x, const float y) override { (void)x; (void)y; hoverDraggerIndex = -1; };
	void setViewport(const D2D1_RECT_F& vp);
	void applyStyle();
private:
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
	Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
	float fontSize{ 25.f };
	float angleDeg{ 45.f };
	std::wstring text;
};
