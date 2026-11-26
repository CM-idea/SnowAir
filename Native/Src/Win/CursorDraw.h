#pragma once
// 系统光标的唯一提取 / 叠加实现：MP4、GIF、录制预览都用这一份。
//
// 以前三条路各写一套：GDI 的 DrawIconEx、手搓 AND 掩码补 alpha、还有直接 DestroyIcon。
// 单色光标（工字、十字）在 DrawIconEx + 32bpp DIB 上 alpha 全是 0，掩码补 alpha 又会把
// "反色像素"整条抹掉 —— 这就是"某些情况下录屏里不显示鼠标"的根因。
// 这里统一走 CursorImage::Extract（项目里截图那条路已经用它），并把反色像素记下来，
// 叠加时按**目标底色**取反，白底黑线 / 黑底白线都正确。

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>
#include "CursorImage.h"

namespace CursorDraw
{
	struct Snapshot
	{
		HCURSOR cur{ nullptr };
		POINT pos{};                 // 热点在桌面坐标里的位置（与 pix 同一时刻）
		bool visible{ false };       // CURSOR_SHOWING：放大动画期间会被临时关掉
		int w{ 0 }, h{ 0 };
		POINT hot{};
		std::vector<BYTE> pix;       // straight BGRA，top-down
		std::vector<BYTE> xorMask;   // 1 = 该像素按底色反色（单色光标）。
		bool valid{ false };
	};

	inline void clear(Snapshot& s) { s = Snapshot{}; }

	// 取当前系统光标；返回 false 表示此刻没有可见光标（例如单击放大期间被临时藏起）。
	// 同一帧内只取一次，镜头圆心、光标叠加都用这一份快照，避免帧内光标状态不一致。
	// requireVisible=false：只借光标形状算本体中心（单击放大隐藏光标时，镜头圆心不能跳）。
	inline bool capture(Snapshot& out, bool requireVisible = true)
	{
		clear(out);
		CURSORINFO ci{ sizeof(ci) };
		if (!GetCursorInfo(&ci) || !ci.hCursor) return false;
		out.pos = ci.ptScreenPos;
		out.visible = (ci.flags & CURSOR_SHOWING) != 0;
		if (requireVisible && !out.visible) return false;
		ICONINFO ii{};
		if (!GetIconInfo(ci.hCursor, &ii)) return false;
		int w = 0, h = 0;
		const bool sized = CursorImage::ChooseSize(ii, &w, &h);
		HDC screen = GetDC(nullptr);
		HDC mem = screen ? CreateCompatibleDC(screen) : nullptr;
		bool ok = false;
		if (sized && mem) {
			ok = CursorImage::Extract(mem, ii, w, h, out.pix, &out.xorMask)
				&& out.pix.size() == (size_t)w * h * 4;
			if (ok) {
				out.w = w;
				out.h = h;
				out.hot = POINT{ (LONG)ii.xHotspot, (LONG)ii.yHotspot };
				out.cur = ci.hCursor;
				out.valid = true;
			}
		}
		if (ii.hbmColor) DeleteObject(ii.hbmColor);
		if (ii.hbmMask) DeleteObject(ii.hbmMask);
		if (mem) DeleteDC(mem);
		if (screen) ReleaseDC(nullptr, screen);
		return ok;
	}

	// 采集线程每帧用这个：光标形状不变时只刷新位置/可见性，不再重新解析位图。
	// （GetIconInfo 每帧会新建两张位图 + 一个 DIB，60fps 下纯属白花；形状才是缓存的键。）
	inline void bodyOffset(const Snapshot& s, float& dx, float& dy);

	class Tracker
	{
	public:
		const Snapshot* update(bool requireVisible = true)
		{
			CURSORINFO ci{ sizeof(ci) };
			if (!GetCursorInfo(&ci) || !ci.hCursor) {
				snap_ = Snapshot{};
				cur_ = nullptr;
				return nullptr;
			}
			const bool visible = (ci.flags & CURSOR_SHOWING) != 0;
			if (ci.hCursor != cur_ || !snap_.valid) {
				Snapshot fresh;
				// 两次 GetCursorInfo 之间光标可能又换了形状：以 fresh 里的句柄为准，
				// 对不上就当这一帧没有光标，下一帧再取，别把旧位图贴到新光标上。
				if (!capture(fresh, false) || fresh.cur != ci.hCursor) {
					snap_ = Snapshot{};
					cur_ = nullptr;
					return nullptr;
				}
				snap_ = std::move(fresh);
				cur_ = ci.hCursor;
				// 修正：光标被 ShowCursor(FALSE) 临时藏起来时（单击放大动画），
				// GetCursorInfo 可能给回另一个光标句柄 —— 不能因此把镜头圆心挪到热点。
				// 本体偏移按形状缓存，藏光标期间继续用上一次算出来的值。
				CursorDraw::bodyOffset(snap_, lastOx_, lastOy_);
				haveOffset_ = true;
			}
			snap_.pos = ci.ptScreenPos;
			snap_.visible = visible;
			if (requireVisible && !visible) return nullptr;
			return snap_.valid ? &snap_ : nullptr;
		}

		// 镜头圆心偏移：光标形状不变（含被临时藏起）时始终用同一个值，镜头不会因单击放大而移位。
		bool bodyOffset(float& dx, float& dy) const
		{
			dx = lastOx_;
			dy = lastOy_;
			return haveOffset_;
		}

	private:
		HCURSOR cur_{ nullptr };
		Snapshot snap_;
		float lastOx_{ 0.f };
		float lastOy_{ 0.f };
		bool haveOffset_{ false };
	};

	// 光标本体中心相对热点的偏移（镜头圆心用本体中心，而不是指针尖角）。
	// 取的是不透明像素包围盒中心，和 Util::cursorBodyOffset 同一口径，但不再重复取一次光标。
	inline void bodyOffset(const Snapshot& s, float& dx, float& dy)
	{
		dx = 0.f;
		dy = 0.f;
		if (!s.valid || s.w <= 0 || s.h <= 0) return;
		int minX = s.w, minY = s.h, maxX = -1, maxY = -1;
		for (int y = 0; y < s.h; ++y) {
			const BYTE* row = s.pix.data() + (size_t)y * s.w * 4;
			for (int x = 0; x < s.w; ++x) {
				if (!row[(size_t)x * 4 + 3]) continue;
				if (x < minX) minX = x;
				if (x > maxX) maxX = x;
				if (y < minY) minY = y;
				if (y > maxY) maxY = y;
			}
		}
		if (maxX < minX || maxY < minY) return;
		dx = (minX + maxX) * 0.5f - (float)s.hot.x;
		dy = (minY + maxY) * 0.5f - (float)s.hot.y;
	}

	// 把光标按 zoom 叠到一帧上（镜头之后调用，保证 1:1 且只出现一份）。
	// origin 是帧左上角的桌面坐标；bottomUp 表示帧的行序。
	inline void blend(std::vector<BYTE>& dst, int w, int h, int originX, int originY,
		bool bottomUp, POINT screenPos, const Snapshot& s, float zoom)
	{
		if (!s.valid || w <= 0 || h <= 0 || s.w <= 0 || s.h <= 0) return;
		if (dst.size() < (size_t)w * h * 4) return;
		if (!(zoom > 0.f)) zoom = 1.f;
		const int dw = (std::max)(1, (int)std::lround(s.w * zoom));
		const int dh = (std::max)(1, (int)std::lround(s.h * zoom));
		const int left = screenPos.x - originX - (int)std::lround(s.hot.x * zoom);
		const int top = screenPos.y - originY - (int)std::lround(s.hot.y * zoom);
		for (int y = 0; y < dh; ++y) {
			const int fy = top + y;
			if (fy < 0 || fy >= h) continue;
			BYTE* dstRow = dst.data() + (size_t)(bottomUp ? h - 1 - fy : fy) * w * 4;
			const int sy = (std::min)(s.h - 1, (int)((long long)y * s.h / dh));
			for (int x = 0; x < dw; ++x) {
				const int fx = left + x;
				if (fx < 0 || fx >= w) continue;
				const int sx = (std::min)(s.w - 1, (int)((long long)x * s.w / dw));
				const size_t si = (size_t)sy * s.w + sx;
				const BYTE* src = s.pix.data() + si * 4;
				BYTE* d = dstRow + (size_t)fx * 4;
				if (!src[3]) continue;
				if (!s.xorMask.empty() && s.xorMask[si]) {
					// 单色光标的"反色"像素：用这一帧的底色取反，透明光标才不会在浅色背景上消失
					d[0] = (BYTE)(255 - d[0]);
					d[1] = (BYTE)(255 - d[1]);
					d[2] = (BYTE)(255 - d[2]);
					d[3] = 255;
					continue;
				}
				const unsigned a = src[3];
				if (a == 255) {
					d[0] = src[0]; d[1] = src[1]; d[2] = src[2];
				}
				else {
					d[0] = (BYTE)((src[0] * a + d[0] * (255 - a)) / 255);
					d[1] = (BYTE)((src[1] * a + d[1] * (255 - a)) / 255);
					d[2] = (BYTE)((src[2] * a + d[2] * (255 - a)) / 255);
				}
				d[3] = 255;
			}
		}
	}
}
