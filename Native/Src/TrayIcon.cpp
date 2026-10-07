#include "pch.h"
#include <wincodec.h>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <cmath>
#include "TrayIcon.h"
#include "Setting.h"
#include "Tool/IconCodes.h"

using Microsoft::WRL::ComPtr;

namespace TrayIcon {
namespace {

std::unordered_map<int, HICON> g_cache;   // key = style*1024 + size
std::wstring g_customPath;                // 上次用于生成自定义图标的路径
uint32_t g_customColor = 0;               // 上次用于生成"自定义颜色"图标的时间色
bool g_customColorValid = false;
bool g_shown = true;                      // 托盘图标当前是否挂着（initTray 时已 NIM_ADD）

// 系统是否为浅色：AppsUseLightTheme != 0
bool sysIsLight()
{
	DWORD light = 1;
	HKEY hKey = nullptr;
	if (RegOpenKeyExW(HKEY_CURRENT_USER,
		L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
		0, KEY_READ, &hKey) == ERROR_SUCCESS) {
		DWORD size = sizeof(DWORD);
		RegQueryValueExW(hKey, L"AppsUseLightTheme", nullptr, nullptr,
			reinterpret_cast<LPBYTE>(&light), &size);
		RegCloseKey(hKey);
	}
	return light != 0;
}

// BGRA 像素（自上而下）→ 32bpp DIB（HICON 用非预乘 alpha，掩码全 0）
HBITMAP makeColorDib(int size, const std::vector<BYTE>& bgra)
{
	BITMAPV5HEADER bi{};
	bi.bV5Size = sizeof(bi);
	bi.bV5Width = size;
	bi.bV5Height = -size;
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

HICON dibToIcon(HBITMAP color, int size)
{
	if (!color) return nullptr;
	const UINT maskStride = (UINT)(((size + 15) / 16) * 2);
	std::vector<BYTE> maskBits((size_t)maskStride * size, 0);
	HBITMAP mask = CreateBitmap(size, size, 1, 1, maskBits.data());
	ICONINFO ii{};
	ii.fIcon = TRUE;
	ii.hbmColor = color;
	ii.hbmMask = mask;
	HICON ic = CreateIconIndirect(&ii);
	DeleteObject(color);
	if (mask) DeleteObject(mask);
	return ic;
}

// 图标字库里的 Logo 字形，按 tint 上色 → HICON。
// 注意：图标字形的 em 盒比实际图形大一圈，直接按目标尺寸渲染会"小一圈"（四周留白）。
// 做法：先在放大位图上渲染，取墨迹包围盒，再等比缩放填满目标尺寸。
HICON buildGlyphIcon(uint32_t tint, int size)
{
	auto d2d = Ling::D2D::get();
	if (!d2d || !d2d->dwriteFactory || !d2d->d2dFactory) return nullptr;

	const int big = (std::max)(size * 3, 96);

	ComPtr<IWICImagingFactory> wic;
	if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(&wic)))) return nullptr;
	ComPtr<IWICBitmap> target;
	if (FAILED(wic->CreateBitmap(big, big, GUID_WICPixelFormat32bppPBGRA,
		WICBitmapCacheOnLoad, &target))) return nullptr;
	ComPtr<ID2D1RenderTarget> rt;
	if (FAILED(d2d->d2dFactory->CreateWicBitmapRenderTarget(
		target.Get(), D2D1::RenderTargetProperties(), rt.GetAddressOf()))) return nullptr;

	IDWriteTextFormat* format = d2d->getTextFormat(L"font_family");
	if (!format) return nullptr;
	const wchar_t glyph[2]{ Icon::Logo[0], 0 };
	ComPtr<IDWriteTextLayout> layout;
	if (FAILED(d2d->dwriteFactory->CreateTextLayout(glyph, 1, format,
		(float)big, (float)big, layout.GetAddressOf())) || !layout) return nullptr;
	layout->SetFontSize((float)big * 0.9f, { 0, 1 });
	layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
	layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

	rt->BeginDraw();
	rt->Clear(D2D1::ColorF(0, 0.f));
	rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
	rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
	ComPtr<ID2D1SolidColorBrush> brush;
	rt->CreateSolidColorBrush(Ling::Color(tint).getD2DColor(), brush.GetAddressOf());
	rt->DrawTextLayout({ 0.f, 0.f }, layout.Get(), brush.Get());
	if (FAILED(rt->EndDraw())) return nullptr;

	ComPtr<IWICFormatConverter> conv;
	if (FAILED(wic->CreateFormatConverter(conv.GetAddressOf()))) return nullptr;
	if (FAILED(conv->Initialize(target.Get(), GUID_WICPixelFormat32bppBGRA,
		WICBitmapDitherTypeNone, nullptr, 0.f, WICBitmapPaletteTypeCustom))) return nullptr;
	const UINT stride = (UINT)big * 4;
	std::vector<BYTE> px((size_t)stride * big);
	if (FAILED(conv->CopyPixels(nullptr, stride, (UINT)px.size(), px.data()))) return nullptr;

	// 墨迹包围盒（alpha > 8）
	int minX = big, minY = big, maxX = -1, maxY = -1;
	for (int y = 0; y < big; y++) {
		const BYTE* row = &px[(size_t)y * stride];
		for (int x = 0; x < big; x++) {
			if (row[x * 4 + 3] > 8) {
				if (x < minX) minX = x;
				if (x > maxX) maxX = x;
				if (y < minY) minY = y;
				if (y > maxY) maxY = y;
			}
		}
	}
	if (maxX < minX || maxY < minY) return nullptr;
	const UINT bw = (UINT)(maxX - minX + 1);
	const UINT bh = (UINT)(maxY - minY + 1);

	// 等比缩放到满格，居中放进 size×size 透明画布
	const float k = (float)size / (float)(std::max)(bw, bh);
	const UINT dw = (UINT)(std::max)(1.f, (float)std::lround(bw * k));
	const UINT dh = (UINT)(std::max)(1.f, (float)std::lround(bh * k));

	ComPtr<IWICBitmap> mem;
	if (FAILED(wic->CreateBitmapFromMemory(big, big, GUID_WICPixelFormat32bppBGRA,
		stride, (UINT)px.size(), px.data(), mem.GetAddressOf()))) return nullptr;
	ComPtr<IWICBitmapClipper> clipper;
	if (FAILED(wic->CreateBitmapClipper(clipper.GetAddressOf()))) return nullptr;
	const WICRect rc{ minX, minY, (INT)bw, (INT)bh };
	if (FAILED(clipper->Initialize(mem.Get(), &rc))) return nullptr;
	ComPtr<IWICBitmapScaler> scaler;
	if (FAILED(wic->CreateBitmapScaler(scaler.GetAddressOf()))) return nullptr;
	if (FAILED(scaler->Initialize(clipper.Get(), dw, dh, WICBitmapInterpolationModeFant))) return nullptr;
	std::vector<BYTE> scaled((size_t)dw * dh * 4);
	if (FAILED(scaler->CopyPixels(nullptr, dw * 4, (UINT)scaled.size(), scaled.data()))) return nullptr;

	std::vector<BYTE> canvas((size_t)size * size * 4, 0);
	const UINT ox = (size - dw) / 2, oy = (size - dh) / 2;
	for (UINT y = 0; y < dh; y++)
		memcpy(&canvas[((size_t)(y + oy) * size + ox) * 4], &scaled[(size_t)y * dw * 4], (size_t)dw * 4);
	return dibToIcon(makeColorDib(size, canvas), size);
}

// 用户选择的图片文件 → HICON（等比缩放后居中放在透明方画布上）
HICON buildFileIcon(const std::wstring& path, int size)
{
	if (path.empty()) return nullptr;
	if (path.size() >= 4 && _wcsicmp(path.c_str() + path.size() - 4, L".ico") == 0) {
		HICON ic = (HICON)LoadImageW(nullptr, path.c_str(), IMAGE_ICON, size, size, LR_LOADFROMFILE);
		if (ic) return ic;
	}
	ComPtr<IWICImagingFactory> wic;
	if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(&wic)))) return nullptr;
	ComPtr<IWICBitmapDecoder> decoder;
	if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
		WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf()))) return nullptr;
	ComPtr<IWICBitmapFrameDecode> frame;
	if (FAILED(decoder->GetFrame(0, frame.GetAddressOf()))) return nullptr;
	UINT sw = 0, sh = 0;
	if (FAILED(frame->GetSize(&sw, &sh)) || !sw || !sh) return nullptr;

	ComPtr<IWICFormatConverter> conv;
	if (FAILED(wic->CreateFormatConverter(conv.GetAddressOf()))) return nullptr;
	if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
		WICBitmapDitherTypeNone, nullptr, 0.f, WICBitmapPaletteTypeCustom))) return nullptr;

	const float k = (float)size / (float)(std::max)(sw, sh);
	const UINT dw = (UINT)(std::max)(1.f, sw * k);
	const UINT dh = (UINT)(std::max)(1.f, sh * k);
	ComPtr<IWICBitmapScaler> scaler;
	if (FAILED(wic->CreateBitmapScaler(scaler.GetAddressOf()))) return nullptr;
	if (FAILED(scaler->Initialize(conv.Get(), dw, dh, WICBitmapInterpolationModeFant))) return nullptr;
	std::vector<BYTE> px((size_t)dw * dh * 4);
	if (FAILED(scaler->CopyPixels(nullptr, dw * 4, (UINT)px.size(), px.data()))) return nullptr;

	std::vector<BYTE> canvas((size_t)size * size * 4, 0);   // 透明底
	const UINT ox = (size - dw) / 2, oy = (size - dh) / 2;
	for (UINT y = 0; y < dh; y++)
		memcpy(&canvas[((size_t)(y + oy) * size + ox) * 4], &px[(size_t)y * dw * 4], (size_t)dw * 4);
	return dibToIcon(makeColorDib(size, canvas), size);
}

void freeCache()
{
	for (auto& kv : g_cache) if (kv.second) DestroyIcon(kv.second);
	g_cache.clear();
}

} // namespace

bool tintFor(int style, uint32_t& argb)
{
	switch ((Style)style) {
	case Style::FollowSystem: argb = sysIsLight() ? 0x0B0C0EFFu : 0xE5E7EBFFu; return true;
	case Style::Theme:        argb = 0x34C759FFu; return true;
	case Style::CustomColor:  argb = Setting::get()->getTrayCustomColor(); return true;
	case Style::CustomIcon:   return false;   // 用图片本身的颜色
	default:                  argb = 0xE5E7EBFFu; return true;
	}
}

HICON iconFor(int style, int size)
{
	// 按系统上报的小图标尺寸（DPI 已计入）生成：正好等于托盘槽尺寸 → 不缩放，最清晰。
	// 给大了会被 shell 缩小变糊，给小了会显得小一圈，所以这里就取它要的那一档。
	if (size <= 0) size = (std::max)(16, GetSystemMetrics(SM_CXSMICON));
	const int key = style * 1024 + size;
	auto it = g_cache.find(key);
	if (it != g_cache.end()) return it->second;

	HICON ic = nullptr;
	if ((Style)style == Style::CustomIcon) {
		ic = buildFileIcon(Setting::get()->getTrayCustomIconPath(), size);
		if (!ic) {   // 没有可用图片：回退到"跟随系统"的内置标
			uint32_t t = 0; tintFor((int)Style::FollowSystem, t);
			ic = buildGlyphIcon(t, size);
		}
	}
	else {
		uint32_t t = 0; tintFor(style, t);
		ic = buildGlyphIcon(t, size);
	}
	if (ic) g_cache.emplace(key, ic);
	return ic;
}

void apply()
{
	auto* s = Setting::get();
	auto* app = Ling::App::get();
	if (!s || !app) return;

	// 自定义图片 / 自定义颜色 变了 → 缓存作废重建
	const std::wstring path = s->getTrayCustomIconPath();
	if (path != g_customPath) { g_customPath = path; freeCache(); }
	const uint32_t cc = s->getTrayCustomColor();
	if (!g_customColorValid || cc != g_customColor) { g_customColorValid = true; g_customColor = cc; freeCache(); }

	if (!s->getTrayEnabled()) {
		if (g_shown) { app->setTrayVisible(false); g_shown = false; }
		return;
	}
	if (!g_shown) { app->setTrayVisible(true); g_shown = true; }
	if (HICON ic = iconFor(s->getTrayIconStyle())) app->setTrayIcon(ic);
}

void dispose()
{
	freeCache();
}

} // namespace TrayIcon
