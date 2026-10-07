#include "pch.h"
#include "AnnotCursor.h"
#include <wincodec.h>
#include <unordered_map>
#include <vector>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace AnnotCursor {
namespace {

// 图标字体（iconfont 项目 5228143）里的四角旋转字形：左上 / 右上 / 右下 / 左下旋转
// 按角槽位取值：slot 0..3 = TL / TR / BR / BL
constexpr wchar_t kGlyph[4] = { L'\ue667', L'\ue668', L'\ue669', L'\ue66a' };

// 角序号（0/2/4/6）→ 角槽位（0..3）
int cornerSlot(int corner)
{
	switch (corner) {
	case 0: return 0;
	case 2: return 1;
	case 4: return 2;
	default: return 3;
	}
}

constexpr int kAngleSteps = 24;                              // 15° 一档：够顺滑，缓存也有界
constexpr float kTwoPi = 6.283185307179586f;
// 位图按系统光标尺寸（通常 32×32），但字形只画这么点：
// 图标字形的 em 盒几乎撑满字号，按 1.0 画会比系统光标胖一圈
constexpr float kGlyphScale = 0.60f;                         // 32px 位图 → 字形约 19px
constexpr float kOutlinePx = 0.8f;                           // 黑描边偏移

std::unordered_map<int, HCURSOR> g_cache;

HBITMAP makeColorDib(int size, const std::vector<BYTE>& bgra)
{
	BITMAPV5HEADER bi{};
	bi.bV5Size = sizeof(bi);
	bi.bV5Width = size;
	bi.bV5Height = -size;                                    // 自上而下，与 WIC 行序一致
	bi.bV5Planes = 1;
	bi.bV5BitCount = 32;
	bi.bV5Compression = BI_BITFIELDS;
	bi.bV5RedMask = 0x00FF0000;
	bi.bV5GreenMask = 0x0000FF00;
	bi.bV5BlueMask = 0x000000FF;
	bi.bV5AlphaMask = 0xFF000000;
	void* bits = nullptr;
	HDC screen = GetDC(nullptr);
	HBITMAP bmp = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
	ReleaseDC(nullptr, screen);
	if (!bmp || !bits) {
		if (bmp) DeleteObject(bmp);
		return nullptr;
	}
	memcpy(bits, bgra.data(), bgra.size());
	return bmp;
}

HCURSOR build(wchar_t code, int angleStep)
{
	const int size = std::max(16, GetSystemMetrics(SM_CXCURSOR));
	auto d2d = Ling::D2D::get();
	if (!d2d || !d2d->dwriteFactory || !d2d->d2dFactory) return nullptr;

	ComPtr<IWICImagingFactory> wic;
	if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(&wic)))) return nullptr;
	ComPtr<IWICBitmap> target;
	if (FAILED(wic->CreateBitmap(size, size, GUID_WICPixelFormat32bppPBGRA,
		WICBitmapCacheOnLoad, &target))) return nullptr;

	ComPtr<ID2D1RenderTarget> rt;
	if (FAILED(d2d->d2dFactory->CreateWicBitmapRenderTarget(
		target.Get(), D2D1::RenderTargetProperties(), rt.GetAddressOf()))) return nullptr;

	// 图标字形：复用 Ling 的自定义字体集合，family = font_family
	IDWriteTextFormat* format = d2d->getTextFormat(L"font_family");
	if (!format) return nullptr;
	const wchar_t glyph[2]{ code, 0 };
	ComPtr<IDWriteTextLayout> layout;
	if (FAILED(d2d->dwriteFactory->CreateTextLayout(glyph, 1, format,
		(float)size, (float)size, layout.GetAddressOf())) || !layout) return nullptr;
	layout->SetFontSize((float)size * kGlyphScale, { 0, 1 });
	layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
	layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

	rt->BeginDraw();
	rt->Clear(D2D1::ColorF(0, 0.f));
	rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
	rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
	// 光标要压在任意底色上：黑描边打底 + 白字形
	const float deg = (float)angleStep * 360.f / (float)kAngleSteps;
	const D2D1_POINT_2F c{ size * 0.5f, size * 0.5f };
	rt->SetTransform(D2D1::Matrix3x2F::Rotation(deg, c));
	ComPtr<ID2D1SolidColorBrush> brush;
	rt->CreateSolidColorBrush(D2D1::ColorF(0.f, 0.f, 0.f, 1.f), brush.GetAddressOf());
	const D2D1_POINT_2F offsets[4]{
		{ -kOutlinePx, 0.f }, { kOutlinePx, 0.f }, { 0.f, -kOutlinePx }, { 0.f, kOutlinePx }
	};
	for (const auto& o : offsets) rt->DrawTextLayout(o, layout.Get(), brush.Get());
	brush->SetColor(D2D1::ColorF(1.f, 1.f, 1.f, 1.f));
	rt->DrawTextLayout({ 0.f, 0.f }, layout.Get(), brush.Get());
	if (FAILED(rt->EndDraw())) return nullptr;

	// PBGRA → BGRA（HICON 用非预乘 alpha）→ DIB → HICON，热点取中心
	ComPtr<IWICFormatConverter> conv;
	if (FAILED(wic->CreateFormatConverter(conv.GetAddressOf()))) return nullptr;
	if (FAILED(conv->Initialize(target.Get(), GUID_WICPixelFormat32bppBGRA,
		WICBitmapDitherTypeNone, nullptr, 0.f, WICBitmapPaletteTypeCustom))) return nullptr;
	const UINT stride = (UINT)size * 4;
	std::vector<BYTE> pixels((size_t)stride * size);
	if (FAILED(conv->CopyPixels(nullptr, stride, (UINT)pixels.size(), pixels.data()))) return nullptr;

	HBITMAP color = makeColorDib(size, pixels);
	if (!color) return nullptr;
	// 掩码全 0：不透明区域由 alpha 通道决定
	const UINT maskStride = (UINT)(((size + 15) / 16) * 2);
	std::vector<BYTE> maskBits((size_t)maskStride * size, 0);
	HBITMAP mask = CreateBitmap(size, size, 1, 1, maskBits.data());

	ICONINFO ii{};
	ii.fIcon = FALSE;                                        // 光标
	ii.hbmColor = color;
	ii.hbmMask = mask;
	// 热点必须显式设成位图中心：CreateIconIndirect 对光标会用 xHotspot/yHotspot，
	// 默认 0,0 = 左上角 → 字形会整体偏到指针右下 16px（看着"离角很远"）
	ii.xHotspot = (DWORD)(size / 2);
	ii.yHotspot = (DWORD)(size / 2);
	HCURSOR cur = reinterpret_cast<HCURSOR>(CreateIconIndirect(&ii));
	DeleteObject(color);
	if (mask) DeleteObject(mask);
	return cur;
}

} // namespace

HCURSOR glyph(wchar_t code, float angleRad)
{
	int step = (int)std::lround((double)angleRad / kTwoPi * kAngleSteps);
	step = ((step % kAngleSteps) + kAngleSteps) % kAngleSteps;
	const int key = ((int)code << 8) | step;
	auto it = g_cache.find(key);
	if (it != g_cache.end()) return it->second;
	if (g_cache.size() > 200) {
		for (auto& kv : g_cache) if (kv.second) DestroyCursor(kv.second);
		g_cache.clear();
	}
	HCURSOR cur = build(code, step);
	if (cur) g_cache.emplace(key, cur);
	return cur;
}

HCURSOR rotate(int corner, float angleRad)
{
	return glyph(kGlyph[cornerSlot(corner)], angleRad);
}

} // namespace AnnotCursor
