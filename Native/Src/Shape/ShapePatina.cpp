#include "pch.h"
#include "Win/AnnotHost.h"
#include "Tool/ToolSub.h"
#include "History.h"
#include "PatinaEffect.h"
#include "ShapePatina.h"

using Microsoft::WRL::ComPtr;

ShapePatina::ShapePatina(AnnotHost* win) : ShapeArea(win)
{
	Ling::D2D::get()->deviceContext->CreateSolidColorBrush(
		D2D1::ColorF(0.2f, 0.2f, 0.2f, 0.25f), brushBusy.GetAddressOf());
	applyStyle();
}

void ShapePatina::setViewport(const D2D1_RECT_F& vp)
{
	if (rect.left != vp.left || rect.top != vp.top || rect.right != vp.right || rect.bottom != vp.bottom)
		dirty = true;
	rect = vp;
	updateDraggers();
	if (dirty) rebuild();
}

void ShapePatina::applyStyle()
{
	if (!win->toolSub) return;
	const int ns = (int)std::round(win->toolSub->getSliderVal() / std::max(0.01f, win->dpi));
	const bool ng = win->toolSub->isPatinaGreen;
	const bool nw = win->toolSub->isPatinaWatermark;
	const int np = win->toolSub->patinaPlan;
	const int nws = win->toolSub->patinaWmSize;
	if (ns != strength || ng != useGreen || nw != showWm || np != wmPlan || nws != wmSize)
		dirty = true;
	strength = ns;
	useGreen = ng;
	showWm = nw;
	wmPlan = np;
	wmSize = nws;
	if (dirty) rebuild();
}

void ShapePatina::rebuild()
{
	dirty = false;
	bitmap.Reset();
	const int left = (int)std::floor(rect.left);
	const int top = (int)std::floor(rect.top);
	const int right = (int)std::ceil(rect.right);
	const int bottom = (int)std::ceil(rect.bottom);
	if (right - left < 2 || bottom - top < 2 || !win->screenImg) return;

	const UINT32 bw = (UINT32)(right - left);
	const UINT32 bh = (UINT32)(bottom - top);
	auto d2d = Ling::D2D::get();
	auto ctx = d2d->deviceContext.Get();

	D2D1_BITMAP_PROPERTIES1 targetProps{
		.pixelFormat{ D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED) },
		.dpiX{ 96.f }, .dpiY{ 96.f },
		.bitmapOptions{ D2D1_BITMAP_OPTIONS_TARGET }
	};
	ComPtr<ID2D1Bitmap1> target;
	if (FAILED(ctx->CreateBitmap(D2D1::SizeU(bw, bh), nullptr, 0, &targetProps, target.GetAddressOf())))
		return;
	ctx->SetTarget(target.Get());
	ctx->SetTransform(D2D1::Matrix3x2F::Translation(-(float)left, -(float)top));
	ctx->BeginDraw();
	ctx->Clear(D2D1::ColorF(1.f, 1.f, 1.f, 1.f));
	ctx->DrawBitmap(win->screenImg.Get(), D2D1::RectF(0, 0, win->w, win->h));
	for (auto& shape : win->history->shapes) {
		auto* cur = shape.get();
		if (cur == this) break;
		if (!cur->isUndo && !cur->isViewportFilter()) cur->paint(ctx);
	}
	ctx->EndDraw();
	ctx->SetTransform(D2D1::Matrix3x2F::Identity());
	ctx->SetTarget(nullptr);

	D2D1_BITMAP_PROPERTIES1 cpuProps{
		.pixelFormat{ target->GetPixelFormat() },
		.dpiX{ 96.f }, .dpiY{ 96.f },
		.bitmapOptions{ D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW }
	};
	ComPtr<ID2D1Bitmap1> cpu;
	if (FAILED(ctx->CreateBitmap(D2D1::SizeU(bw, bh), nullptr, 0, &cpuProps, cpu.GetAddressOf())))
		return;
	if (FAILED(cpu->CopyFromBitmap(nullptr, target.Get(), nullptr))) return;
	D2D1_MAPPED_RECT mapped{};
	if (FAILED(cpu->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return;
	std::vector<BYTE> pixels((size_t)mapped.pitch * bh);
	CopyMemory(pixels.data(), mapped.bits, pixels.size());
	cpu->Unmap();

	PatinaEffect::Style st;
	st.strength = strength;
	st.green = useGreen;
	st.watermark = showWm;
	st.plan = wmPlan;
	st.wmSize = wmSize;
	st.seed = L"SnowAir-patina";
	PatinaEffect::apply(pixels, bw, bh, mapped.pitch, st);

	auto props = D2D1::BitmapProperties(
		D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
	ctx->CreateBitmap(D2D1::SizeU(bw, bh), pixels.data(), mapped.pitch, &props, bitmap.GetAddressOf());
}

void ShapePatina::paint(ID2D1DeviceContext* ctx)
{
	if (dirty) rebuild();
	if (bitmap) {
		ctx->DrawBitmap(bitmap.Get(), rect);
		return;
	}
	if (brushBusy) ctx->FillRectangle(rect, brushBusy.Get());
}
