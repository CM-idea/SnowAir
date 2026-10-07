#include "pch.h"
#include "Win/AnnotHost.h"
#include "Tool/ToolSub.h"
#include "ShapeWatermark.h"

ShapeWatermark::ShapeWatermark(AnnotHost* win) : ShapeArea(win)
{
	Ling::D2D::get()->deviceContext->CreateSolidColorBrush(
		win->toolSub->getSelectedColor(), brush.GetAddressOf());
	applyStyle();
}

void ShapeWatermark::setViewport(const D2D1_RECT_F& vp)
{
	rect = vp;
	updateDraggers();
}

void ShapeWatermark::applyStyle()
{
	if (!win->toolSub) return;
	fontSize = std::max(12.f, win->toolSub->getSliderVal() / std::max(0.01f, win->dpi));
	angleDeg = win->toolSub->watermarkAngle;
	text = win->toolSub->watermarkText;
	if (text.empty()) text = L"请输入文字...";
	if (brush) {
		auto c = win->toolSub->getSelectedColor();
		c.a = 0.2f;
		brush->SetColor(c);
	}
	format.Reset();
	Ling::D2D::get()->dwriteFactory->CreateTextFormat(
		L"Microsoft YaHei UI", nullptr,
		DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
		fontSize, L"zh-cn", format.GetAddressOf());
}

void ShapeWatermark::paint(ID2D1DeviceContext* ctx)
{
	if (!format || !brush) return;
	const float rw = rect.right - rect.left;
	const float rh = rect.bottom - rect.top;
	if (rw < 2.f || rh < 2.f) return;

	// 按字宽步长 + 交错行
	DWRITE_TEXT_METRICS metrics{};
	Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
	Ling::D2D::get()->dwriteFactory->CreateTextLayout(
		text.c_str(), (UINT32)text.size(), format.Get(), 4096.f, fontSize * 2.f, layout.GetAddressOf());
	float textW = fontSize * 4.f;
	if (layout && SUCCEEDED(layout->GetMetrics(&metrics)))
		textW = metrics.widthIncludingTrailingWhitespace;
	const float stepX = textW + fontSize * 1.6f;
	const float stepY = fontSize * 2.1f;
	const float half = std::hypot(rw, rh) * 0.5f + std::max(stepX, stepY);
	const float cx = (rect.left + rect.right) * 0.5f;
	const float cy = (rect.top + rect.bottom) * 0.5f;

	Microsoft::WRL::ComPtr<ID2D1Layer> layer;
	if (FAILED(ctx->CreateLayer(nullptr, layer.GetAddressOf()))) return;
	ctx->PushLayer(D2D1::LayerParameters(rect), layer.Get());
	D2D1_MATRIX_3X2_F old{};
	ctx->GetTransform(&old);
	ctx->SetTransform(D2D1::Matrix3x2F::Rotation(-angleDeg, { cx, cy }) * old);
	int row = 0;
	for (float y = -half; y < half; y += stepY, ++row) {
		const float rowOffset = (row % 2) * (stepX * 0.5f);
		for (float x = -half + rowOffset; x < half; x += stepX) {
			ctx->DrawText(text.c_str(), (UINT32)text.size(), format.Get(),
				D2D1::RectF(cx + x, cy + y - fontSize * 0.35f, cx + x + stepX, cy + y + fontSize * 1.2f),
				brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
		}
	}
	ctx->SetTransform(old);
	ctx->PopLayer();
}
