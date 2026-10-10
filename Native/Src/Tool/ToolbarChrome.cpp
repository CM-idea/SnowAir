#include "pch.h"
#include "ToolbarChrome.h"
#include "ToolbarTheme.h"
#include <functional>
#include <cmath>

using namespace Microsoft::WRL;

namespace ToolbarChrome {

void applyPropBubbleContentPad(Ling::Node* contentNode, bool tipDown)
{
	if (!contentNode) return;
	const auto pad = propBubbleContentPad(tipDown);
	contentNode->setPosition(Ling::Edge::Top, pad.top);
	contentNode->setPosition(Ling::Edge::Bottom, pad.bottom);
}

namespace {

constexpr float kAaPad{ 1.f };     // 窗内留 1px 给 AA，不改布局间距
constexpr float kSuperSample{ 2.f }; // 2× 绘制再高质量缩小 → 圆角更柔

void enableSmooth(ID2D1DeviceContext* ctx)
{
	ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
	ctx->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
}

ComPtr<ID2D1SolidColorBrush> makeBrush(ID2D1DeviceContext* ctx, UINT32 rgba)
{
	ComPtr<ID2D1SolidColorBrush> brush;
	if (!ctx) return brush;
	ctx->CreateSolidColorBrush(Ling::Color(rgba).getD2DColor(), brush.GetAddressOf());
	return brush;
}

float clampRadius(const D2D1_RECT_F& bar, float radiusPx)
{
	const float maxR = (std::min)((bar.right - bar.left) * 0.5f, (bar.bottom - bar.top) * 0.5f);
	return (std::min)((std::max)(0.f, radiusPx), maxR);
}

D2D1_RECT_F insetRect(const D2D1_RECT_F& bar, float inset)
{
	return { bar.left + inset, bar.top + inset, bar.right - inset, bar.bottom - inset };
}

// 在 2× 位图上抗锯齿绘制，再立方插值贴回 → 边缘柔和且无投影
void blitSoft(ID2D1DeviceContext* ctx, const D2D1_RECT_F& dest,
	const std::function<void(ID2D1DeviceContext*, float /*scale*/)>& paint)
{
	const float dw = dest.right - dest.left;
	const float dh = dest.bottom - dest.top;
	if (dw < 1.f || dh < 1.f) return;
	const UINT tw = (UINT)std::ceil(dw * kSuperSample);
	const UINT th = (UINT)std::ceil(dh * kSuperSample);
	if (tw == 0 || th == 0) return;

	D2D1_BITMAP_PROPERTIES1 props{};
	props.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
	props.dpiX = 96.f;
	props.dpiY = 96.f;
	props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
	ComPtr<ID2D1Bitmap1> bmp;
	if (FAILED(ctx->CreateBitmap(D2D1::SizeU(tw, th), nullptr, 0, &props, bmp.GetAddressOf()))) {
		paint(ctx, 1.f);
		return;
	}

	ComPtr<ID2D1Image> prevTarget;
	ctx->GetTarget(prevTarget.GetAddressOf());
	D2D1_MATRIX_3X2_F prevXf{};
	ctx->GetTransform(&prevXf);

	ctx->SetTarget(bmp.Get());
	ctx->SetTransform(D2D1::Matrix3x2F::Identity());
	enableSmooth(ctx);
	ctx->Clear(D2D1::ColorF(0, 0.f));
	paint(ctx, kSuperSample);

	ctx->SetTarget(prevTarget.Get());
	ctx->SetTransform(prevXf);
	enableSmooth(ctx);
	ctx->DrawBitmap(bmp.Get(), dest, 1.f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
}

ComPtr<ID2D1PathGeometry> makeBubblePath(ID2D1Factory* factory, const D2D1_RECT_F& bar,
	float radiusPx, float caretX, float tipY, float baseY)
{
	// 单一闭合路径（圆角矩形 + 三角），Fill/Draw 都用它 → 接缝处不会多一条描边
	ComPtr<ID2D1PathGeometry> result;
	if (!factory) return result;
	const float rad = clampRadius(bar, radiusPx);
	const float hb = (std::max)(std::fabs(tipY - baseY), 1.f);
	const float ax = (std::clamp)(caretX, bar.left + rad + hb, bar.right - rad - hb);
	const float L = bar.left, T = bar.top, R = bar.right, B = bar.bottom;
	const bool tipUp = tipY < baseY;

	factory->CreatePathGeometry(result.GetAddressOf());
	ComPtr<ID2D1GeometrySink> sink;
	result->Open(sink.GetAddressOf());
	auto arcTo = [&](float x, float y) {
		sink->AddArc(D2D1::ArcSegment(
			D2D1::Point2F(x, y), D2D1::SizeF(rad, rad), 0.f,
			D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
	};

	if (tipUp) {
		sink->BeginFigure({ ax, tipY }, D2D1_FIGURE_BEGIN_FILLED);
		sink->AddLine({ ax + hb, T });
		sink->AddLine({ R - rad, T });
		arcTo(R, T + rad);
		sink->AddLine({ R, B - rad });
		arcTo(R - rad, B);
		sink->AddLine({ L + rad, B });
		arcTo(L, B - rad);
		sink->AddLine({ L, T + rad });
		arcTo(L + rad, T);
		sink->AddLine({ ax - hb, T });
	}
	else {
		sink->BeginFigure({ ax, tipY }, D2D1_FIGURE_BEGIN_FILLED);
		sink->AddLine({ ax - hb, B });
		sink->AddLine({ L + rad, B });
		arcTo(L, B - rad);
		sink->AddLine({ L, T + rad });
		arcTo(L + rad, T);
		sink->AddLine({ R - rad, T });
		arcTo(R, T + rad);
		sink->AddLine({ R, B - rad });
		arcTo(R - rad, B);
		sink->AddLine({ ax + hb, B });
	}
	sink->EndFigure(D2D1_FIGURE_END_CLOSED);
	sink->Close();
	return result;
}

void paintRoundDirect(ID2D1DeviceContext* ctx, const D2D1_RECT_F& bar, float radiusPx,
	ID2D1Brush* bg, float borderWidthPx, float scale)
{
	const D2D1_RECT_F r{
		bar.left * scale, bar.top * scale, bar.right * scale, bar.bottom * scale
	};
	const float rad = radiusPx * scale;
	const float bw = borderWidthPx * scale;
	ctx->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), bg);
	if (bw <= 0.f) return;
	auto stroke = makeBrush(ctx, ToolbarTheme::border);
	if (!stroke) return;
	const float hw = bw * 0.5f;
	const D2D1_RECT_F inset{ r.left + hw, r.top + hw, r.right - hw, r.bottom - hw };
	const float ir = (std::max)(0.f, rad - hw);
	ctx->DrawRoundedRectangle(D2D1::RoundedRect(inset, ir, ir), stroke.Get(), bw);
}

} // namespace

void paintRoundBar(ID2D1DeviceContext* ctx, const D2D1_RECT_F& bar, float radiusPx,
	ID2D1Brush* bg, float borderWidthPx)
{
	if (!ctx || !bg) return;
	enableSmooth(ctx);
	const auto drawBar = insetRect(bar, kAaPad);
	const float r = clampRadius(drawBar, radiusPx);
	// 局部坐标画进超采样位图（左上为 0）
	const D2D1_RECT_F local{ 0, 0, drawBar.right - drawBar.left, drawBar.bottom - drawBar.top };
	blitSoft(ctx, drawBar, [&](ID2D1DeviceContext* c, float scale) {
		paintRoundDirect(c, local, r, bg, borderWidthPx, scale);
	});
}

void paintInfoBar(ID2D1DeviceContext* ctx, const D2D1_RECT_F& bar, float radiusPx,
	ID2D1Brush* bg)
{
	if (!ctx || !bg) return;
	// 信息栏本身无投影；「圆角/阴影」指选区效果，不是栏的 drop shadow
	paintRoundBar(ctx, bar, radiusPx, bg, 0.f);
}

void paintBarWithCaret(ID2D1DeviceContext* ctx, const D2D1_RECT_F& bar, float radiusPx,
	ID2D1Brush* bg, float borderWidthPx, float caretX, float tipY, float baseY)
{
	if (!ctx || !bg) return;
	enableSmooth(ctx);
	const auto drawBar = insetRect(bar, kAaPad);
	const float r = clampRadius(drawBar, radiusPx);
	// 位图盖住圆角条 + 箭头尖（尖端可在 bar 外）；tip/base 用绝对坐标转局部，上下箭头同一套
	const float destTop = (std::min)(drawBar.top, (std::min)(tipY, baseY));
	const float destBottom = (std::max)(drawBar.bottom, (std::max)(tipY, baseY));
	const D2D1_RECT_F dest{ drawBar.left, destTop, drawBar.right, destBottom };
	const float ox = dest.left;
	const float oy = dest.top;
	const D2D1_RECT_F localBar{
		drawBar.left - ox, drawBar.top - oy, drawBar.right - ox, drawBar.bottom - oy
	};
	const float localCaretX = caretX - ox;
	const float localTipY = tipY - oy;
	const float localBaseY = baseY - oy;

	auto d2d = Ling::D2D::get();
	blitSoft(ctx, dest, [&](ID2D1DeviceContext* c, float scale) {
		const D2D1_RECT_F scaledBar{
			localBar.left * scale, localBar.top * scale,
			localBar.right * scale, localBar.bottom * scale
		};
		auto geo = makeBubblePath(d2d->d2dFactory.Get(), scaledBar, r * scale,
			localCaretX * scale, localTipY * scale, localBaseY * scale);
		if (!geo) {
			paintRoundDirect(c, localBar, r, bg, borderWidthPx, scale);
			return;
		}
		c->FillGeometry(geo.Get(), bg);
		if (borderWidthPx > 0.f) {
			auto stroke = makeBrush(c, ToolbarTheme::border);
			if (stroke) c->DrawGeometry(geo.Get(), stroke.Get(), borderWidthPx * scale);
		}
	});
}

void paintPropBubble(ID2D1DeviceContext* ctx, float winW, float winH, float dpi,
	ID2D1Brush* bg, float caretX, bool tipDown, float borderWidthPx)
{
	if (!ctx || !bg) return;
	const float pad = ToolbarTheme::shadowPad * dpi;
	const float caret = ToolbarTheme::caretSize * dpi;
	const float left = pad;
	const float right = std::floor(winW) - pad;
	const float radius = ToolbarTheme::borderRadius * dpi;
	const float bw = borderWidthPx * dpi;
	if (tipDown) {
		const float barBottom = std::floor(winH) - pad - caret;
		paintBarWithCaret(ctx, { left, pad, right, barBottom }, radius, bg, bw,
			caretX, std::floor(winH) - pad, barBottom);
	}
	else {
		const float barTop = pad + caret;
		paintBarWithCaret(ctx, { left, barTop, right, std::floor(winH) - pad }, radius, bg, bw,
			caretX, pad, barTop);
	}
}

} // namespace ToolbarChrome
