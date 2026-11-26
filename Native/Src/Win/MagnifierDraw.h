#pragma once
// 「局部放大」的唯一渲染实现：屏幕上的镜头预览（MagLens）和录进视频的镜头（采集线程）
// 都调这里，保证"屏幕上看到的"和"录出来的"是同一套像素。
//
// 分工：
//   · render()  从一块**正方形干净源图**（中心=镜头圆心）生成整个镜头：圆形内容 + 圆环 + 逐像素 alpha；
//   · blit()    把镜头按 alpha 叠到一帧上（帧可以是 top-down 或 bottom-up）；
//   · draw()    录制侧的便捷入口：从帧里裁出源图 → render → blit；
//   · drawClickOverlay() 单击波纹 / 放大光标（在镜头之后画，所以不会被二次放大）。
//
// 源图里**没有系统光标**：光标由 CursorDraw 在镜头之后按 1:1 补画（屏幕上是系统光标本身），
// 这样预览和录制里光标大小一致，也不会出现"镜头里再套一个光标"。

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include <windows.h>
#include "MouseFxParams.h"

namespace MagnifierDraw
{
	struct Spec
	{
		POINT center{};      // 镜头圆心（屏幕坐标，已含光标本体偏移）
		int sizePx{ 230 };   // 镜头直径（物理像素）
		float zoom{ 1.5f };
	};

	inline int radiusOf(const Spec& spec) { return (std::max)(4, spec.sizePx / 2); }
	inline int borderOf(const Spec& spec) { return (std::max)(1, spec.sizePx / 100); }

	// 源图半径：比"直径 / 倍率"多留一个像素，圆周双线性采样时四邻域也完整。
	inline int sourceRadius(const Spec& spec)
	{
		const float zoom = (std::max)(1.2f, spec.zoom);
		return (std::max)(2, (int)std::ceil((float)radiusOf(spec) / zoom) + 1);
	}
	inline int sourceSize(const Spec& spec) { return sourceRadius(spec) * 2 + 1; }

	// 镜头需要的那块正方形桌面矩形（left/top 即圆心 - 半径）
	inline RECT sourceRect(const Spec& spec)
	{
		const int r = sourceRadius(spec);
		return RECT{ spec.center.x - r, spec.center.y - r,
			spec.center.x + r + 1, spec.center.y + r + 1 };
	}

	inline BYTE* pixelAt(std::vector<BYTE>& dst, int w, int h, int x, int y, bool bottomUp)
	{
		const int row = bottomUp ? h - 1 - y : y;
		return dst.data() + ((size_t)row * w + x) * 4;
	}

	// 源图统一按 top-down 采样；浮点坐标落在四个相邻像素之间时做双线性插值。
	inline void sampleBgra(const BYTE* src, int w, int h, float x, float y, BYTE out[4])
	{
		const int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
		const int x1 = (std::min)(x0 + 1, w - 1), y1 = (std::min)(y0 + 1, h - 1);
		const int cx0 = std::clamp(x0, 0, w - 1), cy0 = std::clamp(y0, 0, h - 1);
		const float tx = x - x0, ty = y - y0;
		const BYTE* p00 = src + ((size_t)cy0 * w + cx0) * 4;
		const BYTE* p01 = src + ((size_t)cy0 * w + x1) * 4;
		const BYTE* p10 = src + ((size_t)y1 * w + cx0) * 4;
		const BYTE* p11 = src + ((size_t)y1 * w + x1) * 4;
		for (int c = 0; c < 4; ++c) {
			const float a = p00[c] * (1.f - tx) + p01[c] * tx;
			const float b = p10[c] * (1.f - tx) + p11[c] * tx;
			out[c] = (BYTE)std::lround(a * (1.f - ty) + b * ty);
		}
	}

	inline void blendPixel(BYTE* d, const BYTE src[4], float opacity = 1.f)
	{
		const float a = (src[3] / 255.f) * opacity;
		if (a <= 0.f) return;
		d[0] = (BYTE)std::lround(src[0] * a + d[0] * (1.f - a));
		d[1] = (BYTE)std::lround(src[1] * a + d[1] * (1.f - a));
		d[2] = (BYTE)std::lround(src[2] * a + d[2] * (1.f - a));
		d[3] = 255;
	}

	// 从正方形干净源图生成长宽都是 sizePx 的镜头（top-down BGRA，圆外 alpha = 0）。
	inline void render(BYTE* out, int outSize, const Spec& spec,
		const BYTE* square, int squareSize)
	{
		if (!out || !square || outSize <= 0 || squareSize <= 0) return;
		const int radius = radiusOf(spec);
		const int border = borderOf(spec);
		const float zoom = (std::max)(1.2f, spec.zoom);
		const int half = outSize / 2;
		const int srcR = sourceRadius(spec);
		const float center = (float)half;
		const float rOuter = (float)radius;
		const float rInner = (float)radius - (float)border;
		const float r2 = (float)radius * radius;
		// 正方形源图的中心像素下标正好是 srcR（sourceSize = 2*srcR+1）
		const float srcCenter = (float)srcR;

		for (int y = 0; y < outSize; ++y) {
			const int dy = y - half;
			BYTE* row = out + (size_t)y * outSize * 4;
			for (int x = 0; x < outSize; ++x) {
				BYTE* q = row + (size_t)x * 4;
				const int dx = x - half;
				const float d2 = (float)(dx * dx + dy * dy);
				if (d2 > r2) { q[0] = q[1] = q[2] = q[3] = 0; continue; }
				BYTE s[4]{};
				sampleBgra(square, squareSize, squareSize,
					srcCenter + (float)dx / zoom, srcCenter + (float)dy / zoom, s);
				s[3] = 255;
				const float dist = std::sqrt(d2);
				if (dist > rInner) {
					const float a = std::clamp(
						(std::min)(dist - rInner + 0.5f, rOuter - dist + 0.5f), 0.f, 1.f);
					q[0] = (BYTE)std::lround(s[0] * (1.f - a) + 245.f * a);
					q[1] = (BYTE)std::lround(s[1] * (1.f - a) + 245.f * a);
					q[2] = (BYTE)std::lround(s[2] * (1.f - a) + 245.f * a);
					q[3] = 255;
					continue;
				}
				q[0] = s[0]; q[1] = s[1]; q[2] = s[2]; q[3] = 255;
			}
		}
		(void)center;
	}

	// 把镜头叠到一帧上。x/y 是镜头左上角的桌面坐标；origin 是帧左上角的桌面坐标。
	inline void blit(std::vector<BYTE>& dst, int w, int h, int originX, int originY,
		bool bottomUp, int x, int y, const BYTE* lens, int lensSize)
	{
		if (!lens || w <= 0 || h <= 0 || lensSize <= 0) return;
		if (dst.size() < (size_t)w * h * 4) return;
		const int left = x - originX;
		const int top = y - originY;
		for (int row = 0; row < lensSize; ++row) {
			const int fy = top + row;
			if (fy < 0 || fy >= h) continue;
			const BYTE* srcRow = lens + (size_t)row * lensSize * 4;
			BYTE* dstRow = dst.data() + (size_t)(bottomUp ? h - 1 - fy : fy) * w * 4;
			for (int col = 0; col < lensSize; ++col) {
				const int fx = left + col;
				if (fx < 0 || fx >= w) continue;
				const BYTE* s = srcRow + (size_t)col * 4;
				if (!s[3]) continue;
				BYTE* d = dstRow + (size_t)fx * 4;
				const unsigned a = s[3];
				if (a == 255) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255; }
				else {
					d[0] = (BYTE)((s[0] * a + d[0] * (255 - a)) / 255);
					d[1] = (BYTE)((s[1] * a + d[1] * (255 - a)) / 255);
					d[2] = (BYTE)((s[2] * a + d[2] * (255 - a)) / 255);
					d[3] = 255;
				}
			}
		}
	}

	// 录制侧入口：先从帧里裁出源图（避免边读边写），再 render，最后叠回帧。
	inline void draw(std::vector<BYTE>& dst, int w, int h, int originX, int originY,
		const Spec& spec, bool bottomUp = true)
	{
		if (w <= 0 || h <= 0 || spec.sizePx <= 0) return;
		const int srcD = sourceSize(spec);
		const RECT sq = sourceRect(spec);
		std::vector<BYTE> square((size_t)srcD * srcD * 4, 0);
		for (int row = 0; row < srcD; ++row) {
			const int fy = sq.top + row - originY;
			if (fy < 0 || fy >= h) continue;
			const BYTE* srcRow = dst.data() + (size_t)(bottomUp ? h - 1 - fy : fy) * w * 4;
			for (int col = 0; col < srcD; ++col) {
				const int fx = sq.left + col - originX;
				if (fx < 0 || fx >= w) continue;
				memcpy(square.data() + ((size_t)row * srcD + col) * 4,
					srcRow + (size_t)fx * 4, 4);
			}
		}
		std::vector<BYTE> lens((size_t)spec.sizePx * spec.sizePx * 4, 0);
		render(lens.data(), spec.sizePx, spec, square.data(), srcD);
		blit(dst, w, h, originX, originY, bottomUp,
			spec.center.x - spec.sizePx / 2, spec.center.y - spec.sizePx / 2,
			lens.data(), spec.sizePx);
	}

	// ---------------- 单击层（波纹 / 放大光标） ----------------
	struct ClickFx
	{
		POINT screenPt{};
		ULONGLONG t{ 0 };
	};
	// WinCap 的单击层被排除在桌面捕获外；采集侧用这份状态在镜头之后补画。
	struct ClickOverlayFrame
	{
		bool enabled{ false };
		bool ripple{ false };
		bool zoom{ false };
		UINT32 color{ 0 };
		float dpi{ 1.f };
		std::vector<ClickFx> clicks;
		bool cursorValid{ false };
		POINT cursorPos{};
		POINT cursorHotspot{};
		int cursorW{ 0 }, cursorH{ 0 };
		std::vector<BYTE> cursorPix;   // top-down BGRA，straight alpha
	};

	inline void drawClickOverlay(std::vector<BYTE>& dst, int w, int h,
		int originX, int originY, const ClickOverlayFrame& state, bool bottomUp = true)
	{
		if (!state.enabled || w <= 0 || h <= 0 || dst.size() < (size_t)w * h * 4) return;
		const ULONGLONG now = GetTickCount64();
		if (state.ripple) {
			const BYTE b = (BYTE)((state.color >> 8) & 0xFFu);
			const BYTE g = (BYTE)((state.color >> 16) & 0xFFu);
			const BYTE r = (BYTE)((state.color >> 24) & 0xFFu);
			for (const auto& fx : state.clicks) {
				if (now < fx.t) continue;
				const ULONGLONG age = now - fx.t;
				if (age >= MouseFx::rippleLifeMs) continue;
				const float k = (float)age / (float)MouseFx::rippleLifeMs;
				const float radius = MouseFx::rippleRadius(k) * state.dpi;
				const float thickness = MouseFx::rippleWidth(k) * state.dpi;
				const float cx = (float)(fx.screenPt.x - originX);
				const float cy = (float)(fx.screenPt.y - originY);
				const float extent = radius + thickness * 0.5f + 1.f;
				const int left = (std::max)(0, (int)std::floor(cx - extent));
				const int right = (std::min)(w - 1, (int)std::ceil(cx + extent));
				const int top = (std::max)(0, (int)std::floor(cy - extent));
				const int bottom = (std::min)(h - 1, (int)std::ceil(cy + extent));
				const float opacity = MouseFx::rippleAlpha(k);
				for (int y = top; y <= bottom; ++y) {
					for (int x = left; x <= right; ++x) {
						const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
						const float dist = std::sqrt(dx * dx + dy * dy);
						const float coverage = std::clamp(thickness * 0.5f + 0.5f - std::fabs(dist - radius), 0.f, 1.f);
						const BYTE s[4]{ b, g, r, 255 };
						blendPixel(pixelAt(dst, w, h, x, y, bottomUp), s, opacity * coverage);
					}
				}
			}
		}

		if (!state.zoom || !state.cursorValid || state.clicks.empty()
			|| state.cursorW <= 0 || state.cursorH <= 0
			|| state.cursorPix.size() < (size_t)state.cursorW * state.cursorH * 4) return;
		if (now < state.clicks.back().t) return;
		const ULONGLONG age = now - state.clicks.back().t;
		if (age >= 320) return;
		const float scale = 1.f + 0.8f * std::sin((float)age / 320.f * 3.14159265f);
		const float left = state.cursorPos.x - originX - state.cursorHotspot.x * scale;
		const float top = state.cursorPos.y - originY - state.cursorHotspot.y * scale;
		const int right = (int)std::ceil(left + state.cursorW * scale);
		const int bottom = (int)std::ceil(top + state.cursorH * scale);
		for (int y = (std::max)(0, (int)std::floor(top)); y < (std::min)(h, bottom); ++y) {
			for (int x = (std::max)(0, (int)std::floor(left)); x < (std::min)(w, right); ++x) {
				const float sx = (x + 0.5f - left) / scale - 0.5f;
				const float sy = (y + 0.5f - top) / scale - 0.5f;
				if (sx < 0.f || sy < 0.f || sx > state.cursorW - 1.f || sy > state.cursorH - 1.f) continue;
				BYTE s[4]{};
				sampleBgra(state.cursorPix.data(), state.cursorW, state.cursorH, sx, sy, s);
				blendPixel(pixelAt(dst, w, h, x, y, bottomUp), s);
			}
		}
	}
}
