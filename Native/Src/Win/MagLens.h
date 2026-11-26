#pragma once
// 「局部放大」：跟随光标的 230x230 圆形放大镜（鼠标在圆心）。
//
// 这一层只负责「把镜头搬到屏幕上」：
//   · 窗口是 layered + UpdateLayeredWindow 原子提交（位置和新画面同一次提交），
//     圆形由内容位图逐像素 alpha 完成，**不设窗口 region**；
//   · 取景内容由场景源提供（录制时是采集线程的干净帧，其余场合回落到 ScreenCapture），
//     渲染与录制共用 MagnifierDraw::render —— 屏幕上看到的就是录出来的；
//   · 镜头内容里不再画光标：屏幕上是系统光标，录制侧由 CursorDraw 在镜头之后按 1:1 补画；
//   · 每帧先 DwmFlush 对齐合成节拍，避免 8ms 定时器与 60Hz 合成错拍产生抖动。
//
// 注意：镜头窗口自身带 WDA_EXCLUDEFROMCAPTURE，任何屏幕捕获都拿不到它，
// 所以它不会进入自己的取景源，也不会出现在录屏 / 截图里（录制侧会自己再画一个镜头）。

#include <windows.h>
#include <dwmapi.h>
#include <timeapi.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include "MagnifierDraw.h"
#include "ScreenCapture.h"

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "dwmapi.lib")

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

class MagLens
{
public:
	// need：镜头取景需要的桌面矩形；x/y 返回画面左上角的桌面坐标；out 是 top-down BGRA。
	// 返回 false 时 MagLens 会自己抓一帧兜底（录制开始前 / 设置页预览 / 光标移出录制区）。
	using SceneProvider = std::function<bool(const RECT& need, int& x, int& y, std::vector<BYTE>& out)>;

	static MagLens& get()
	{
		static MagLens* inst = new MagLens();
		return *inst;
	}

	// center：镜头圆心（屏幕坐标，已含光标本体偏移）。大小按 dpi 换算成物理像素。
	void showAt(POINT center, float dpi, float sizeLogical = 230.f)
	{
		startThread();
		const int size = (std::max)(32, (int)std::lround(sizeLogical * (dpi > 0.f ? dpi : 1.f)));
		std::lock_guard<std::mutex> lock(mutex_);
		wantCenter_ = center;
		wantSize_ = size;
		wanted_ = true;
	}

	void hide()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		wanted_ = false;
	}

	void setZoom(float zoom)
	{
		zoom_ = std::clamp(zoom, 1.2f, 4.0f);
	}

	// 场景源由 CapVideo 在开始录制时挂上、停止录制时摘掉（nullptr）。
	void setSceneProvider(SceneProvider fn)
	{
		// 与 tick 里的调用共用一把锁：摘掉 provider 会等到正在取景的那一次返回，
		// 保证之后的 CapVideo / 采集帧销毁不会撞上镜头线程。
		std::lock_guard<std::mutex> lock(providerMutex_);
		provider_ = std::move(fn);
		hasProvider_ = (provider_ != nullptr);
	}

	void setBelow(HWND h)
	{
		if (below_.load(std::memory_order_relaxed) == h && !topmost_.load(std::memory_order_relaxed)) return;
		below_ = h;
		topmost_ = false;
		zDirty_ = true;
	}

	void raiseTop()
	{
		if (topmost_.load(std::memory_order_relaxed) && below_.load(std::memory_order_relaxed) == nullptr) return;
		below_ = nullptr;
		topmost_ = true;
		zDirty_ = true;
	}

	bool visible() const { return wanted_.load(); }

private:
	MagLens() = default;

	void startThread()
	{
		if (started_.exchange(true)) return;
		std::thread([this]() { run(); }).detach();
	}

	void run()
	{
		const HINSTANCE inst = GetModuleHandleW(nullptr);
		WNDCLASSEXW wc{ sizeof(wc) };
		wc.lpfnWndProc = &MagLens::wndProc;
		wc.hInstance = inst;
		// 镜头窗口永远是光标下最顶层的那个：不设类光标，并且 WM_SETCURSOR 原样返回，
		// 否则它会把下层窗口的十字/画笔光标改回箭头 —— 表现为光标在十字和箭头之间闪。
		wc.hCursor = nullptr;
		wc.lpszClassName = L"SnowAirMagLensHost";
		RegisterClassExW(&wc);
		hwnd_ = CreateWindowExW(
			WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE
			| WS_EX_TRANSPARENT | WS_EX_LAYERED,
			wc.lpszClassName, L"", WS_POPUP, 0, 0, 230, 230,
			nullptr, nullptr, inst, this);
		if (!hwnd_) return;
		SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE);
		if (HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr))
			SetWindowPos(hwnd_, tray, 0, 0, 0, 0,
				SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		timeBeginPeriod(1);

		// 不用 WM_TIMER：定时器会被消息队列合并，8ms/16ms 交替送达；镜头直接按
		// 「画一帧 → 等这次合成完成」的节奏跑，每次 UpdateLayeredWindow 都稳定落在下一拍。
		MSG msg{};
		for (;;) {
			while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
				if (msg.message == WM_QUIT) { timeEndPeriod(1); return; }
				TranslateMessage(&msg);
				DispatchMessageW(&msg);
			}
			if (!wanted_.load(std::memory_order_relaxed)) {
				tick();          // 收起窗口
				Sleep(10);
				continue;
			}
			tick();
			DwmFlush();          // 等这一帧真的被合成，下一次更新固定在下一拍
		}
	}

	void tick()
	{
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if (!wanted_.load()) {
				if (shown_) { ShowWindow(hwnd_, SW_HIDE); shown_ = false; }
				lastCenter_ = POINT{ INT_MIN, INT_MIN };
				return;
			}
		}

		applyZOrder();
		const bool hasProvider = hasProvider_.load(std::memory_order_relaxed);

		POINT center{};
		int size = 0;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if (!wanted_.load()) {
				if (shown_) { ShowWindow(hwnd_, SW_HIDE); shown_ = false; }
				return;
			}
			center = wantCenter_;
			size = wantSize_;
		}

		const float zoom = std::clamp(zoom_.load(std::memory_order_relaxed), 1.2f, 4.0f);
		if (size != lensSize_) {
			lensSize_ = (std::max)(32, size);
			createContentBitmap(lensSize_);
		}
		if (!contentDC_ || !contentBits_) return;

		const ULONGLONG now = GetTickCount64();
		MagnifierDraw::Spec spec{ center, lensSize_, zoom };
		const RECT need = MagnifierDraw::sourceRect(spec);
		const int srcW = (int)(need.right - need.left);
		const int srcH = (int)(need.bottom - need.top);
		std::vector<BYTE> scene;
		int sceneX = need.left, sceneY = need.top;
		bool haveScene = false;
		if (hasProvider) {
			std::lock_guard<std::mutex> lock(providerMutex_);
			if (provider_)
				haveScene = provider_(need, sceneX, sceneY, scene)
					&& sceneX == need.left && sceneY == need.top
					&& scene.size() == (size_t)srcW * srcH * 4;
		}
		// 没有采集帧（设置页预览 / 录制开始前 / 镜头在别的显示器上）时自己抓一屏。
		// 抓屏一次十几毫秒，不能每帧都抓：按 around-cursor 的一块缓存起来，
		// 60fps 的镜头只是在缓存里平移取样 —— 既跟手，又不会被抓屏卡住节拍。
		if (!haveScene) {
			const bool cacheHit = !fallbackPix_.empty()
				&& need.left >= fallbackRect_.left && need.top >= fallbackRect_.top
				&& need.right <= fallbackRect_.right && need.bottom <= fallbackRect_.bottom
				&& now - fallbackAt_ < 200;
			if (!cacheHit) {
				const int margin = (std::max)(64, srcW / 2);
				const RECT grab{ need.left - margin, need.top - margin,
					need.right + margin, need.bottom + margin };
				const int gw = (int)(grab.right - grab.left);
				const int gh = (int)(grab.bottom - grab.top);
				auto pix = ScreenCapture::captureOrGdi(grab.left, grab.top, gw, gh);
				if (pix.size() == (size_t)gw * gh * 4) {
					fallbackPix_ = std::move(pix);
					fallbackRect_ = grab;
					fallbackAt_ = now;
				}
			}
			if (!fallbackPix_.empty()
				&& need.left >= fallbackRect_.left && need.top >= fallbackRect_.top
				&& need.right <= fallbackRect_.right && need.bottom <= fallbackRect_.bottom) {
				const int fw = (int)(fallbackRect_.right - fallbackRect_.left);
				scene.resize((size_t)srcW * srcH * 4);
				for (int row = 0; row < srcH; ++row) {
					const BYTE* s = fallbackPix_.data()
						+ ((size_t)(need.top - fallbackRect_.top + row) * fw
							+ (need.left - fallbackRect_.left)) * 4;
					memcpy(scene.data() + (size_t)row * srcW * 4, s, (size_t)srcW * 4);
				}
				haveScene = true;
			}
		}
		// 抓不到（锁屏 / 受保护内容）就保留上一帧：
		// 比闪一下黑块好，也不改变这一帧的窗口位置。
		if (haveScene) {
			MagnifierDraw::render(static_cast<BYTE*>(contentBits_), lensSize_, spec,
				scene.data(), srcW);
			haveContent_ = true;
		}
		lastCenter_ = center;
		lastZoom_ = zoom;

		const int x = center.x - lensSize_ / 2;
		const int y = center.y - lensSize_ / 2;
		POINT dst{ x, y };
		SIZE sz{ lensSize_, lensSize_ };
		POINT src{ 0, 0 };
		BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
		UpdateLayeredWindow(hwnd_, nullptr, &dst, &sz, contentDC_, &src, 0, &bf, ULW_ALPHA);
		if (!shown_) { ShowWindow(hwnd_, SW_SHOWNOACTIVATE); shown_ = true; }
	}

	void applyZOrder()
	{
		if (!zDirty_.exchange(false)) return;
		HWND below = below_.load(std::memory_order_relaxed);
		const bool topmost = topmost_.load(std::memory_order_relaxed);
		if (topmost || !below || !IsWindow(below)) {
			SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
				SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
			topmost_ = true;
			below_ = nullptr;
		}
		else {
			SetWindowPos(hwnd_, below, 0, 0, 0, 0,
				SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		}
	}

	void createContentBitmap(int size)
	{
		if (contentDC_ && contentOld_) SelectObject(contentDC_, contentOld_);
		contentOld_ = nullptr;
		if (contentBmp_) { DeleteObject(contentBmp_); contentBmp_ = nullptr; }
		if (contentDC_) { DeleteDC(contentDC_); contentDC_ = nullptr; }
		contentBits_ = nullptr;
		HDC screen = GetDC(nullptr);
		contentDC_ = CreateCompatibleDC(screen);
		if (screen) ReleaseDC(nullptr, screen);
		if (!contentDC_) return;
		BITMAPINFO bi{};
		bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bi.bmiHeader.biWidth = size;
		bi.bmiHeader.biHeight = -size;
		bi.bmiHeader.biPlanes = 1;
		bi.bmiHeader.biBitCount = 32;
		bi.bmiHeader.biCompression = BI_RGB;
		contentBmp_ = CreateDIBSection(contentDC_, &bi, DIB_RGB_COLORS, &contentBits_, nullptr, 0);
		if (contentBmp_) contentOld_ = SelectObject(contentDC_, contentBmp_);
	}

	static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l)
	{
		auto* self = reinterpret_cast<MagLens*>(GetWindowLongPtrW(h, GWLP_USERDATA));
		if (m == WM_CREATE) {
			auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
			self = reinterpret_cast<MagLens*>(cs->lpCreateParams);
			SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
		}
		if (self) {
			if (m == WM_ERASEBKGND) return 1;
			if (m == WM_SETCURSOR) return TRUE;   // 不改光标（见 run() 里 wc.hCursor 的说明）
			if (m == WM_NCHITTEST) return HTTRANSPARENT;
			if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
			if (m == WM_TIMER && w == 1) { self->tick(); return 0; }
			if (m == WM_PAINT) {
				PAINTSTRUCT ps{};
				BeginPaint(h, &ps);
				EndPaint(h, &ps);
				return 0;
			}
		}
		return DefWindowProcW(h, m, w, l);
	}

	std::mutex mutex_;
	POINT wantCenter_{};
	int wantSize_{ 0 };
	std::atomic<bool> wanted_{ false };
	std::atomic<float> zoom_{ 1.5f };
	std::atomic<bool> started_{ false };
	std::atomic<bool> hasProvider_{ false };
	std::mutex providerMutex_;
	SceneProvider provider_;
	std::atomic<HWND> below_{ nullptr };
	std::atomic<bool> topmost_{ true };
	std::atomic<bool> zDirty_{ false };
	HWND hwnd_{ nullptr };
	HDC contentDC_{ nullptr };
	HBITMAP contentBmp_{ nullptr };
	HGDIOBJ contentOld_{ nullptr };
	void* contentBits_{ nullptr };
	int lensSize_{ 0 };
	bool shown_{ false };
	bool haveContent_{ false };
	POINT lastCenter_{ INT_MIN, INT_MIN };
	float lastZoom_{ 0.f };
	// 抓屏兜底缓存：镜头在缓存块里平移取样，避免每帧一次十几毫秒的屏幕 BitBlt
	std::vector<BYTE> fallbackPix_;
	RECT fallbackRect_{};
	ULONGLONG fallbackAt_{ 0 };
};
