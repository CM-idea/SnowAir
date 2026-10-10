#include "pch.h"
#include "Win/AnnotHost.h"
#include "Tool/ToolSub.h"
#include "Tool/IconCodes.h"
#include "History.h"
#include "ShapeMosaic.h"
#include <cmath>

using Microsoft::WRL::ComPtr;

ShapeMosaic::ShapeMosaic(AnnotHost* win) : ShapeArea(win)
{
	auto toolSub = win->toolSub.get();
	auto d2d = Ling::D2D::get();
	// 主题绿半透明占位（拖拽/旋转过程中效果位图尚未生成）
	auto c = Ling::Color(Icon::ColorActive).getD2DColor();
	c.a = 0.35f;
	d2d->deviceContext->CreateSolidColorBrush(c, brush.GetAddressOf());
	strokeWidth = toolSub->getSliderVal();
	isBlur = toolSub->isMosaicBlur;
}

void ShapeMosaic::applyStyle()
{
	if (!win->toolSub) return;
	strokeWidth = win->toolSub->getSliderVal();
	isBlur = win->toolSub->isMosaicBlur;
	buildEffectBitmap();
}

D2D1_RECT_F ShapeMosaic::sampleAabb() const
{
	if (std::fabs(angle) < 1e-6f) return rect;
	const float cx = (rect.left + rect.right) * 0.5f;
	const float cy = (rect.top + rect.bottom) * 0.5f;
	const float ca = std::cos(angle), sa = std::sin(angle);
	const float xs[4] = { rect.left, rect.right, rect.right, rect.left };
	const float ys[4] = { rect.top, rect.top, rect.bottom, rect.bottom };
	float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
	for (int i = 0; i < 4; ++i) {
		const float dx = xs[i] - cx, dy = ys[i] - cy;
		const float x = cx + dx * ca - dy * sa;
		const float y = cy + dx * sa + dy * ca;
		minX = std::min(minX, x); minY = std::min(minY, y);
		maxX = std::max(maxX, x); maxY = std::max(maxY, y);
	}
	return D2D1::RectF(minX, minY, maxX, maxY);
}

void ShapeMosaic::paint(ID2D1DeviceContext* ctx)
{
	auto geo = makeAreaGeometry(false);
	if (!geo) return;

	if (!mosaicBrush) {
		// 占位色块：直接填旋转后的几何，跟着选框一起转/移
		ctx->FillGeometry(geo.Get(), brush.Get());
		return;
	}

	// 只旋转滤镜窗口（裁剪），贴图像素保持轴对齐，不跟着拧
	ComPtr<ID2D1Layer> layer;
	ctx->CreateLayer(nullptr, layer.GetAddressOf());
	if (layer) {
		ctx->PushLayer(
			D2D1::LayerParameters(D2D1::InfiniteRect(), geo.Get(),
				D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix()),
			layer.Get());
	}
	const auto fill = (mosaicSample.right > mosaicSample.left) ? mosaicSample : rect;
	ctx->FillRectangle(fill, mosaicBrush.Get());
	if (layer) ctx->PopLayer();
}

void ShapeMosaic::onGeometryChanged()
{
	resetMosaic();
}

void ShapeMosaic::onGeometrySettled()
{
	buildEffectBitmap();
}

void ShapeMosaic::resetMosaic()
{
	mosaicBitmap.Reset();
	mosaicBrush.Reset();
	mosaicOrigin = { 0.f, 0.f };
	mosaicSample = {};
}

void ShapeMosaic::buildEffectBitmap()
{
	if (rect.right <= rect.left || rect.bottom <= rect.top) return;
	mosaicBitmap = createEffectBitmap();
	mosaicBrush.Reset();
	if (!mosaicBitmap) return;
	auto bitmapBrushProps = D2D1::BitmapBrushProperties(
		D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP,
		D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
	auto brushProps = D2D1::BrushProperties();
	Ling::D2D::get()->deviceContext->CreateBitmapBrush(
		mosaicBitmap.Get(), &bitmapBrushProps, &brushProps, mosaicBrush.GetAddressOf());
	if (mosaicBrush)
		mosaicBrush->SetTransform(D2D1::Matrix3x2F::Translation(mosaicOrigin.x, mosaicOrigin.y));
}

ComPtr<ID2D1Bitmap> ShapeMosaic::createEffectBitmap()
{
	ComPtr<ID2D1Bitmap> result;
	if (win->w <= 0 || win->h <= 0 || !win->screenImg) return result;

	auto d2d = Ling::D2D::get();
	auto ctx = d2d->deviceContext.Get();
	const float logical = std::max(1.f, strokeWidth / std::max(0.01f, win->dpi));
	const float t = std::clamp(logical, 0.f, 100.f);
	const int blockSize = std::max(4, (int)std::round((t / 100.f) * 28.f));
	const int blurRadius = std::max(1, std::min(24, (int)std::round(t / 8.f)));
	const int pad = isBlur ? blurRadius * 2 : blockSize;

	// 旋转时按旋转矩形的 AABB 取样（内容仍轴对齐）
	const auto aabb = sampleAabb();
	const int winW = (int)win->w, winH = (int)win->h;
	int left = std::max(0, std::min((int)std::floor(aabb.left) - pad, winW));
	int top = std::max(0, std::min((int)std::floor(aabb.top) - pad, winH));
	int right = std::max(0, std::min((int)std::ceil(aabb.right) + pad + 1, winW));
	int bottom = std::max(0, std::min((int)std::ceil(aabb.bottom) + pad + 1, winH));
	if (left >= right || top >= bottom) return result;

	mosaicOrigin = { (float)left, (float)top };
	mosaicSample = D2D1::RectF((float)left, (float)top, (float)right, (float)bottom);
	auto localSize = D2D1::SizeU((UINT32)(right - left), (UINT32)(bottom - top));

	D2D1_BITMAP_PROPERTIES1 targetProps{
		.pixelFormat{ D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED) },
		.dpiX{ 96.0f }, .dpiY{ 96.0f },
		.bitmapOptions{ D2D1_BITMAP_OPTIONS_TARGET }
	};
	ComPtr<ID2D1Bitmap1> targetBitmap;
	if (FAILED(ctx->CreateBitmap(localSize, nullptr, 0, &targetProps, targetBitmap.GetAddressOf())))
		return result;

	ctx->SetTarget(targetBitmap.Get());
	ctx->SetTransform(D2D1::Matrix3x2F::Translation(-mosaicOrigin.x, -mosaicOrigin.y));
	ctx->BeginDraw();
	ctx->Clear(D2D1::ColorF(0, 0.0f));
	ctx->DrawBitmap(win->screenImg.Get(), D2D1::RectF(0, 0, win->w, win->h));
	for (auto& shape : win->history->shapes) {
		auto cur = shape.get();
		if (cur == this) break;
		if (!cur->isUndo) cur->paint(ctx);
	}
	HRESULT hr = ctx->EndDraw();
	ctx->SetTransform(D2D1::Matrix3x2F::Identity());
	ctx->SetTarget(nullptr);
	if (FAILED(hr)) return result;

	D2D1_BITMAP_PROPERTIES1 cpuProps{
		.pixelFormat{ targetBitmap->GetPixelFormat() },
		.dpiX{ 96.0f }, .dpiY{ 96.0f },
		.bitmapOptions{ D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW }
	};
	ComPtr<ID2D1Bitmap1> cpuBitmap;
	if (FAILED(ctx->CreateBitmap(localSize, nullptr, 0, &cpuProps, cpuBitmap.GetAddressOf())))
		return result;
	if (FAILED(cpuBitmap->CopyFromBitmap(nullptr, targetBitmap.Get(), nullptr)))
		return result;

	D2D1_MAPPED_RECT mapped{};
	if (FAILED(cpuBitmap->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return result;
	std::vector<BYTE> pixels((size_t)mapped.pitch * localSize.height);
	CopyMemory(pixels.data(), mapped.bits, pixels.size());
	cpuBitmap->Unmap();

	if (isBlur) blurPixels(pixels.data(), mapped.pitch, localSize.width, localSize.height, blurRadius);
	else mosaicPixels(pixels.data(), mapped.pitch, localSize.width, localSize.height, blockSize);

	auto bitmapProps = D2D1::BitmapProperties(
		D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
	ctx->CreateBitmap(localSize, pixels.data(), mapped.pitch, &bitmapProps, result.GetAddressOf());
	return result;
}

void ShapeMosaic::mosaicPixels(BYTE* bits, UINT32 pitch, UINT32 width, UINT32 height, int blockSize)
{
	if (!bits || blockSize <= 0 || width == 0 || height == 0) return;
	for (UINT32 y = 0; y < height; y += blockSize) {
		UINT32 yEnd = std::min(y + (UINT32)blockSize, height);
		for (UINT32 x = 0; x < width; x += blockSize) {
			UINT32 xEnd = std::min(x + (UINT32)blockSize, width);
			unsigned long long bSum{ 0 }, gSum{ 0 }, rSum{ 0 }, aSum{ 0 }, count{ 0 };
			for (UINT32 yy = y; yy < yEnd; ++yy) {
				auto row = bits + yy * pitch;
				for (UINT32 xx = x; xx < xEnd; ++xx) {
					auto pixel = row + xx * 4;
					bSum += pixel[0]; gSum += pixel[1]; rSum += pixel[2]; aSum += pixel[3];
					++count;
				}
			}
			if (count == 0) continue;
			BYTE b = (BYTE)(bSum / count), g = (BYTE)(gSum / count);
			BYTE r = (BYTE)(rSum / count), a = (BYTE)(aSum / count);
			for (UINT32 yy = y; yy < yEnd; ++yy) {
				auto row = bits + yy * pitch;
				for (UINT32 xx = x; xx < xEnd; ++xx) {
					auto pixel = row + xx * 4;
					pixel[0] = b; pixel[1] = g; pixel[2] = r; pixel[3] = a;
				}
			}
		}
	}
}

// 两趟可分盒模糊，对齐 Tauri2 boxBlur（2 pass）
void ShapeMosaic::blurPixels(BYTE* bits, UINT32 pitch, UINT32 width, UINT32 height, int radius)
{
	if (!bits || radius < 1 || width == 0 || height == 0) return;
	const int r = std::min(radius, 24);
	std::vector<BYTE> tmp((size_t)pitch * height);
	auto passH = [&](const BYTE* src, BYTE* dst) {
		for (UINT32 y = 0; y < height; ++y) {
			for (UINT32 x = 0; x < width; ++x) {
				unsigned long long b = 0, g = 0, rr = 0, a = 0;
				int n = 0;
				for (int dx = -r; dx <= r; ++dx) {
					int xx = (int)x + dx;
					if (xx < 0) xx = 0;
					if (xx >= (int)width) xx = (int)width - 1;
					auto p = src + y * pitch + (UINT32)xx * 4;
					b += p[0]; g += p[1]; rr += p[2]; a += p[3];
					++n;
				}
				auto o = dst + y * pitch + x * 4;
				o[0] = (BYTE)(b / n); o[1] = (BYTE)(g / n);
				o[2] = (BYTE)(rr / n); o[3] = (BYTE)(a / n);
			}
		}
	};
	auto passV = [&](const BYTE* src, BYTE* dst) {
		for (UINT32 y = 0; y < height; ++y) {
			for (UINT32 x = 0; x < width; ++x) {
				unsigned long long b = 0, g = 0, rr = 0, a = 0;
				int n = 0;
				for (int dy = -r; dy <= r; ++dy) {
					int yy = (int)y + dy;
					if (yy < 0) yy = 0;
					if (yy >= (int)height) yy = (int)height - 1;
					auto p = src + (UINT32)yy * pitch + x * 4;
					b += p[0]; g += p[1]; rr += p[2]; a += p[3];
					++n;
				}
				auto o = dst + y * pitch + x * 4;
				o[0] = (BYTE)(b / n); o[1] = (BYTE)(g / n);
				o[2] = (BYTE)(rr / n); o[3] = (BYTE)(a / n);
			}
		}
	};
	passH(bits, tmp.data());
	passV(tmp.data(), bits);
	passH(bits, tmp.data());
	passV(tmp.data(), bits);
}
