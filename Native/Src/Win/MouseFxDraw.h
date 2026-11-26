#pragma once
// 「鼠标轨迹效果」里不放大镜的那两种 —— 高亮提醒 / 线条跟随 —— 的录制侧重画。
//
// 屏幕上的顺序是：桌面/标注 < 镜头 < 高亮/线条 < 光标 < 单击特效。
// 所以这两种效果不能留在采集帧里（否则会被镜头一起放大、而且被镜头压在下面），
// 而是和单击层一样放进"排除捕获"的 SelectionChrome，由采集线程在镜头之后按同一份
// 时间戳重新画一遍。为了让屏幕和视频完全一致，重画不再手搓几何：采集线程用
// WIC + D2D 软件渲染目标跑同一个 LaserStroke::paint，像素算法与 UI 完全相同。

#include <windows.h>
#include <d2d1.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>
#include "../Shape/LaserStroke.h"
#include "MouseFxParams.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace MouseFxDraw
{
	using Microsoft::WRL::ComPtr;

	struct Frame
	{
		bool enabled{ false };          // 只有"效果层独立、不进采集帧"时才需要重画
		bool highlight{ false };
		bool trail{ false };
		UINT32 color{ 0 };              // 0xRRGGBBAA，与 Setting::getMouseHighlightColor 同编码
		float dpi{ 1.f };
		POINT highlightCenter{};        // 屏幕坐标（光标本体中心）
		LaserStroke::Trail trailData;   // 屏幕坐标
	};

	inline void splitColor(UINT32 rgba, float& r, float& g, float& b)
	{
		r = ((rgba >> 24) & 0xFFu) / 255.f;
		g = ((rgba >> 16) & 0xFFu) / 255.f;
		b = ((rgba >> 8) & 0xFFu) / 255.f;
	}

	struct Factories
	{
		ComPtr<IWICImagingFactory> wic;
		ComPtr<ID2D1Factory> d2d;
		bool tried{ false };
		bool ok{ false };
	};

	inline Factories& factories()
	{
		static thread_local Factories f;
		return f;
	}

	inline bool ensureFactories()
	{
		Factories& f = factories();
		if (f.ok) return true;
		if (f.tried) return false;
		f.tried = true;
		// 采集线程可能已经 CoInitialize 过（MP4 那条），也可能没有（GIF 那条）
		const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
		if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return false;
		if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
				IID_PPV_ARGS(&f.wic))) || !f.wic) return false;
		if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
				__uuidof(ID2D1Factory), nullptr,
				reinterpret_cast<void**>(f.d2d.GetAddressOf()))) || !f.d2d) return false;
		f.ok = true;
		return true;
	}

	// 生成 top-down、预乘 alpha 的效果层。ox/oy 是它在桌面坐标里的左上角。
	// 没有可画内容或环境不可用时返回 false（调用方跳过即可）。
	inline bool render(std::vector<BYTE>& out, int& ox, int& oy, int& ow, int& oh,
		const Frame& f, int64_t now)
	{
		if (!f.enabled || (!f.highlight && !f.trail)) return false;
		const float hlRadius = MouseFx::highlightRadius * (f.dpi > 0.f ? f.dpi : 1.f);

		int left = (std::numeric_limits<int>::max)();
		int top = (std::numeric_limits<int>::max)();
		int right = (std::numeric_limits<int>::min)();
		int bottom = (std::numeric_limits<int>::min)();
		auto include = [&](int l, int t, int r, int b) {
			left = (std::min)(left, l);
			top = (std::min)(top, t);
			right = (std::max)(right, r);
			bottom = (std::max)(bottom, b);
		};
		if (f.highlight) {
			include(f.highlightCenter.x - (int)std::ceil(hlRadius) - 2,
				f.highlightCenter.y - (int)std::ceil(hlRadius) - 2,
				f.highlightCenter.x + (int)std::ceil(hlRadius) + 2,
				f.highlightCenter.y + (int)std::ceil(hlRadius) + 2);
		}
		if (f.trail && f.trailData.points.size() >= 2) {
			const int pad = (int)std::ceil((std::max)(1.f, f.trailData.width)) + 2;
			for (const auto& p : f.trailData.points)
				include((int)std::floor(p.x) - pad, (int)std::floor(p.y) - pad,
					(int)std::ceil(p.x) + pad, (int)std::ceil(p.y) + pad);
		}
		if (right <= left || bottom <= top) return false;
		// 防止异常轨迹把位图撑到整屏：镜头附近 1024 像素足够覆盖一条 240ms 的笔迹
		constexpr int kMaxDim = 1024;
		if (right - left > kMaxDim) right = left + kMaxDim;
		if (bottom - top > kMaxDim) bottom = top + kMaxDim;
		const int w = right - left;
		const int h = bottom - top;
		if (w <= 0 || h <= 0) return false;

		if (!ensureFactories()) return false;

		Factories& fct = factories();
		if (!fct.wic || !fct.d2d) return false;
		{
			IWICImagingFactory* wic = fct.wic.Get();
			ID2D1Factory* d2d = fct.d2d.Get();

			ComPtr<IWICBitmap> bitmap;
			if (FAILED(wic->CreateBitmap((UINT)w, (UINT)h, GUID_WICPixelFormat32bppPBGRA,
					WICBitmapCacheOnLoad, &bitmap)) || !bitmap) return false;
			D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
				D2D1_RENDER_TARGET_TYPE_DEFAULT,
				D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
				96.f, 96.f);
			ComPtr<ID2D1RenderTarget> rt;
			if (FAILED(d2d->CreateWicBitmapRenderTarget(bitmap.Get(), props, &rt)) || !rt)
				return false;
			rt->BeginDraw();
			rt->Clear(D2D1::ColorF(0.f, 0.f, 0.f, 0.f));
			float r = 1.f, g = 1.f, b = 1.f;
			splitColor(f.color, r, g, b);
			if (f.highlight) {
				ComPtr<ID2D1SolidColorBrush> brush;
				rt->CreateSolidColorBrush(D2D1::ColorF(r, g, b, MouseFx::highlightAlpha), &brush);
				if (brush) {
					rt->FillEllipse(D2D1::Ellipse(
						D2D1::Point2F((float)(f.highlightCenter.x - left), (float)(f.highlightCenter.y - top)),
						hlRadius, hlRadius), brush.Get());
				}
			}
			if (f.trail && f.trailData.points.size() >= 2) {
				LaserStroke::Trail local = f.trailData;
				for (auto& p : local.points) {
					p.x -= (float)left;
					p.y -= (float)top;
				}
				ComPtr<ID2D1SolidColorBrush> brush;
				rt->CreateSolidColorBrush(D2D1::ColorF(r, g, b, 1.f), &brush);
				if (brush) {
					brush->SetOpacity(MouseFx::trailOpacity);
					LaserStroke::paint(rt.Get(), brush.Get(), local, now);
				}
			}
			if (FAILED(rt->EndDraw())) return false;

			WICRect rc{ 0, 0, w, h };
			ComPtr<IWICBitmapLock> lock;
			if (FAILED(bitmap->Lock(&rc, WICBitmapLockRead, &lock)) || !lock) return false;
			UINT stride = 0, bytes = 0;
			BYTE* bits = nullptr;
			if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&bytes, &bits))
				|| !bits) return false;
			out.resize((size_t)w * h * 4);
			for (int y = 0; y < h; ++y)
				memcpy(out.data() + (size_t)y * w * 4, bits + (size_t)y * stride, (size_t)w * 4);
		}
		ox = left;
		oy = top;
		ow = w;
		oh = h;
		return true;
	}

	// 把预乘 BGRA 效果层叠到帧上（镜头之后、光标之前）
	inline void blend(std::vector<BYTE>& dst, int w, int h, bool bottomUp,
		int originX, int originY, const BYTE* src, int ox, int oy, int ow, int oh)
	{
		if (!src || w <= 0 || h <= 0 || ow <= 0 || oh <= 0) return;
		if (dst.size() < (size_t)w * h * 4) return;
		for (int y = 0; y < oh; ++y) {
			const int fy = oy + y;
			if (fy < 0 || fy >= h) continue;
			const BYTE* srow = src + (size_t)y * ow * 4;
			BYTE* drow = dst.data() + (size_t)(bottomUp ? h - 1 - fy : fy) * w * 4;
			for (int x = 0; x < ow; ++x) {
				const int fx = ox + x;
				if (fx < 0 || fx >= w) continue;
				const BYTE* s = srow + (size_t)x * 4;
				const unsigned a = s[3];
				if (!a) continue;
				BYTE* d = drow + (size_t)fx * 4;
				const unsigned inv = 255u - a;
				d[0] = (BYTE)((std::min)(255u, s[0] + (d[0] * inv) / 255u));
				d[1] = (BYTE)((std::min)(255u, s[1] + (d[1] * inv) / 255u));
				d[2] = (BYTE)((std::min)(255u, s[2] + (d[2] * inv) / 255u));
				d[3] = 255;
			}
		}
	}
}


