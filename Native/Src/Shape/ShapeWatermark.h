#pragma once
#include "ShapeArea.h"

class ShapeWatermark : public ShapeArea
{
public:
	ShapeWatermark(AnnotHost* win);
	std::unique_ptr<ShapeBase> clone(AnnotHost* host) const override
	{
		auto p = std::make_unique<ShapeWatermark>(*this);
		p->win = host;
		p->isUndo = false;
		return p;
	}
	void paint(ID2D1DeviceContext* ctx) override;
	bool isViewportFilter() override { return true; };
	bool hitErase(const float x, const float y) override { (void)x; (void)y; return false; };
	void mouseMove(const float x, const float y) override { (void)x; (void)y; hoverDraggerIndex = -1; };
	void setViewport(const D2D1_RECT_F& vp);
	void applyStyle();
	// 已输入文字的水印：切到其它工具时保留，不被视口滤镜清理掉
	bool sticky() const { return sticky_; }
private:
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
	Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
	float fontSize{ 25.f };
	float angleDeg{ 45.f };
	std::wstring text;
	bool sticky_{ false };
};
