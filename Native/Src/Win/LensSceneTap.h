#pragma once
// 「局部放大」屏幕预览的取景源：采集线程把**干净帧**（还没叠光标 / 镜头 / 单击特效的那一帧）
// 里的一小块交给 UI 线程，屏幕上的圆形镜头直接用它放大。
//
// 为什么不再 BitBlt 屏幕：
//   · DWM 合成下 GDI 读屏要先把整屏扁平化，实测单次 16ms 量级（和刷新率同步），
//     8ms 的镜头线程根本跑不到 60fps，表现就是镜头跟不上光标、画面拖影；
//   · 读到的叠加层还受窗口层级/分层状态影响，必须靠 CAPTUREBLT 才"稳定"，
//     而 CAPTUREBLT 又会把镜头自己的上一帧也读进来，形成反馈。
// 采集线程本来就有一帧干净画面，预览直接采样它就是零额外成本，而且屏幕预览与录制
// 结果用的是同一帧、同一套放大算法 —— 所见即所得。
//
// 采集帧率低于预览帧率：UI 每次登记一个带余量的取景矩形，光标在余量内移动时
// 仍能从同一帧里取到正确位置；超出余量（快速甩动）就返回 false，由调用方抓一帧兜底。

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

namespace LensScene
{
	inline RECT inflate(const RECT& r, int margin)
	{
		return RECT{ r.left - margin, r.top - margin, r.right + margin, r.bottom + margin };
	}

	class Tap
	{
	public:
		// 录制开始 / 停止：停止后立刻不再向预览提供旧帧（镜头回落到抓屏兜底），
		// 避免录制结束后镜头里还停着最后一帧。
		void setLive(bool live)
		{
			live_.store(live, std::memory_order_relaxed);
			if (!live) {
				std::lock_guard<std::mutex> lock(mutex_);
				haveValid_ = false;
				pixels_.clear();
				covered_ = RECT{};
			}
		}

		// 采集线程：当前请求的取景矩形（桌面坐标）。没有 UI 在采样 / 镜头没显示时返回 false。
		bool wanted(RECT& r)
		{
			if (!live_.load(std::memory_order_relaxed)) return false;
			std::lock_guard<std::mutex> lock(mutex_);
			if (!wantValid_) return false;
			if (GetTickCount64() - lastSampleMs_ > 400) return false;
			r = want_;
			return true;
		}

		// 采集线程：已经有 top-down BGRA 像素时直接提交（整屏取景走这条，支持采集帧任意区域）。
		void publishTopDown(const RECT& covered, std::vector<BYTE>&& pixels)
		{
			if (!live_.load(std::memory_order_relaxed)) return;
			std::lock_guard<std::mutex> lock(mutex_);
			if (GetTickCount64() - lastSampleMs_ > 400) return;
			if (covered.right <= covered.left || covered.bottom <= covered.top) return;
			if (pixels.size() != (size_t)(covered.right - covered.left) * (covered.bottom - covered.top) * 4)
				return;
			pixels_ = std::move(pixels);
			covered_ = covered;
			haveValid_ = true;
			publishedMs_ = GetTickCount64();
		}

		// 采集线程：把当前请求区域从一帧里拷出来。origin 是帧左上角的桌面坐标；
		// bottomUp 表示帧的行序（MediaFoundation 的 RGB32 输入是自下而上）。
		void publish(const BYTE* frame, int w, int h, int originX, int originY, bool bottomUp)
		{
			if (!frame || w <= 0 || h <= 0 || !live_.load(std::memory_order_relaxed)) return;
			std::lock_guard<std::mutex> lock(mutex_);
			if (!wantValid_) return;
			// 镜头没显示（没有 UI 采样）时不必每帧白拷一份
			if (GetTickCount64() - lastSampleMs_ > 400) return;
			const RECT r{
				(std::max)(want_.left, (LONG)originX),
				(std::max)(want_.top, (LONG)originY),
				(std::min)(want_.right, (LONG)(originX + w)),
				(std::min)(want_.bottom, (LONG)(originY + h)) };
			if (r.right <= r.left || r.bottom <= r.top) return;
			const int rw = (int)(r.right - r.left);
			const int rh = (int)(r.bottom - r.top);
			pixels_.resize((size_t)rw * rh * 4);
			const int left = (int)(r.left - originX);
			const int top = (int)(r.top - originY);
			for (int y = 0; y < rh; ++y) {
				const int row = bottomUp ? (h - 1 - (top + y)) : (top + y);
				const BYTE* s = frame + ((size_t)row * w + left) * 4;
				memcpy(pixels_.data() + (size_t)y * rw * 4, s, (size_t)rw * 4);
			}
			covered_ = r;
			haveValid_ = true;
			publishedMs_ = GetTickCount64();
		}

		// UI 线程：取 need 覆盖的像素（top-down BGRA）。取景范围不够（光标走得比采集快、
		// 或预览刚开始）返回 false；无论成功与否都会把「下一帧要取的范围」更新成 need 加一圈余量。
		// 超出采集帧边缘的部分按 0 填（录制侧镜头在边缘外同样是 0），不做整块回退。
		bool sample(const RECT& need, int& x, int& y, std::vector<BYTE>& out)
		{
			if (!live_.load(std::memory_order_relaxed)) return false;
			const int margin = (std::max)(64, (int)(need.right - need.left));
			const RECT want = inflate(need, margin);
			std::lock_guard<std::mutex> lock(mutex_);
			want_ = want;
			wantValid_ = true;
			lastSampleMs_ = GetTickCount64();
			if (!haveValid_) return false;
			const RECT inter{
				(std::max)(need.left, covered_.left),
				(std::max)(need.top, covered_.top),
				(std::min)(need.right, covered_.right),
				(std::min)(need.bottom, covered_.bottom) };
			const int nw = (int)(need.right - need.left);
			const int nh = (int)(need.bottom - need.top);
			if (nw <= 0 || nh <= 0) return false;
			if (inter.right <= inter.left || inter.bottom <= inter.top) return false;
			const int cw = (int)(covered_.right - covered_.left);
			out.assign((size_t)nw * nh * 4, 0);
			const int dx = (int)(inter.left - need.left);
			const int dy = (int)(inter.top - need.top);
			for (int row = 0; row < (int)(inter.bottom - inter.top); ++row) {
				const BYTE* s = pixels_.data()
					+ ((size_t)(inter.top - covered_.top + row) * cw + (inter.left - covered_.left)) * 4;
				memcpy(out.data() + ((size_t)(dy + row) * nw + dx) * 4,
					s, (size_t)(inter.right - inter.left) * 4);
			}
			x = need.left;
			y = need.top;
			return true;
		}

	private:
		std::mutex mutex_;
		std::atomic<bool> live_{ false };
		RECT want_{};
		bool wantValid_{ false };
		RECT covered_{};
		std::vector<BYTE> pixels_;
		bool haveValid_{ false };
		ULONGLONG lastSampleMs_{ 0 };
		ULONGLONG publishedMs_{ 0 };
	};
}
