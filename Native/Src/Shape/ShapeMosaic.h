#pragma once
#include <include/Ling.h>
#include "ShapeArea.h"

class ShapeMosaic : public ShapeArea
{
public:
	ShapeMosaic(AnnotHost* win);
	void paint(ID2D1DeviceContext* ctx) override;
	void applyStyle();
	float stylePrimarySize() const override { return strokeWidth; }
	// 重新采样并生成效果位图：下面的标注被移动/改样式后必须调用，
	// 否则滤镜里还留着旧内容（表现为"下层标注移走了，模糊没跟着动"）
	void rebuildEffect() { buildEffectBitmap(); }
protected:
	void onGeometryChanged() override;
	void onGeometrySettled() override;
private:
	void resetMosaic();
	void buildEffectBitmap();
	Microsoft::WRL::ComPtr<ID2D1Bitmap> createEffectBitmap();
	D2D1_RECT_F sampleAabb() const; // 旋转后轴对齐包围盒
	void mosaicPixels(BYTE* bits, UINT32 pitch, UINT32 width, UINT32 height, int blockSize);
	void blurPixels(BYTE* bits, UINT32 pitch, UINT32 width, UINT32 height, int radius);
private:
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
	Microsoft::WRL::ComPtr<ID2D1Bitmap> mosaicBitmap;
	Microsoft::WRL::ComPtr<ID2D1BitmapBrush> mosaicBrush;
	D2D1_POINT_2F mosaicOrigin{ 0.f, 0.f };
	D2D1_RECT_F mosaicSample{ 0, 0, 0, 0 }; // 含 pad 的取样区（轴对齐）
	float strokeWidth{ 1.f };
	bool isBlur{ false };
};
