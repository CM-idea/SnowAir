#include "pch.h"
#include <UIAutomationClient.h>
#include <magnification.h>
#pragma comment(lib, "Magnification.lib")
#pragma comment(lib, "UIAutomationCore.lib")
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#include <include/Ling.h>
#include <mutex>
#include <unordered_map>
#include "CapLong.h"
#include "WinCap.h"
#include "CutMask.h"
#include "WinPin.h"
#include "../Tool/ToolLong.h"
#include "../Tool/ToolbarTheme.h"
#include "../App.h"
#include "../Util.h"
#include "ScreenCapture.h"
#include "../Lang.h"
#include "../Setting.h"
using namespace Microsoft::WRL;

namespace {
	// 定时器 id（同一窗口内唯一即可）
	constexpr UINT autoStepMsgId = 18;     // 自动滚动：按节拍投递滚轮
	constexpr UINT pumpMsgId = 19;         // 预览刷新 / 到底判定
	constexpr UINT previewDragPanMsgId = 20;

	constexpr int pumpTickMs = 60;
	// 自动滚动：匀速。按拍投递很小的滚轮量，让页面连续滑而不是一顿一顿。
	// 投递量与投递频率由“画布实际滚过的像素”闭环校准，因此不同应用（一个 notch 走多远
	// 各不相同）表现出的滚动速度一致。
	constexpr int autoTickMs = 8;                     // 节拍源（真正的投递间隔由算法决定）
	constexpr double autoMinPulseIntervalMs = 8.0;    // 事件最密 125 次/秒，再快对页面没意义
	constexpr double autoTargetViewportPerSec = 1.00; // 目标速度：每秒滚过 1 个视口
	constexpr ULONGLONG autoSpeedSampleMs = 320;      // 稳态下每这么久校准一次速度
	constexpr ULONGLONG autoQuickSampleMs = 16;       // 起步只用一帧左右的窗口校正，肉眼看不到加速段
	constexpr double autoAssumedPxPerUnit = 0.45;     // 给“第一拍”定速用的常见滚轮刻度
	constexpr ULONGLONG autoInjectionFallbackMs = 420; // 投递无效多久后改用真实输入
	constexpr ULONGLONG autoBottomStopMs = 900;       // 画面多久没滚动就认为到底
	constexpr ULONGLONG autoStartGraceMs = 450;       // 起步阶段先给目标应用一点反应时间
	constexpr int kTooFastRejects = 3;                // 连续这么多帧“在动但拼不上”就提示滚太快
	constexpr ULONGLONG kTooFastHintMs = 1600;        // “滚得太快”提示显示多久
	constexpr int previewRefreshMs = 130;
	constexpr int previewDragPanMs = 16;
	constexpr int captureLoopIdleMs = 40;
	constexpr int captureLoopBusyMs = 6;
	constexpr int maxLongHeight = 36000;
	constexpr int maxPreviewTexDim = 8192;
	constexpr ULONG_PTR syntheticWheelMark = 0x534E4F57;
	CapLong* g_mouseHookOwner = nullptr;

	uint64_t frameSignature(const std::vector<BYTE>& data)
	{
		uint64_t h = 1469598103934665603ull;
		const size_t step = std::max<size_t>(4, (data.size() / 4096 / 4) * 4);
		for (size_t i = 0; i + 3 < data.size(); i += step) {
			h = (h ^ data[i]) * 1099511628211ull;
			h = (h ^ data[i + 1]) * 1099511628211ull;
			h = (h ^ data[i + 2]) * 1099511628211ull;
		}
		return h;
	}

	// —— Magnification API 抓帧 ——
	// 回调用 memcpy 直接拿 32bpp BGRA 扫描线，不走 BitBlt/GetDIBits，也不会把截图 UI 拍进去。
	struct MagFrameTarget { std::vector<BYTE> pixels; UINT w{ 0 }, h{ 0 }; bool ready{ false }; };
	thread_local MagFrameTarget g_magFrame;

	BOOL CALLBACK magScalingCallback(HWND, void* srcData, MAGIMAGEHEADER srcHeader,
		void*, MAGIMAGEHEADER, RECT, RECT, HRGN)
	{
		if (!srcData || srcHeader.width == 0 || srcHeader.height == 0) return FALSE;
		auto& t = g_magFrame;
		const size_t rowBytes = (size_t)srcHeader.width * 4;
		t.pixels.resize(rowBytes * srcHeader.height);
		const BYTE* src = static_cast<const BYTE*>(srcData);
		for (UINT r = 0; r < srcHeader.height; ++r)
			memcpy(t.pixels.data() + (size_t)r * rowBytes,
				src + (size_t)r * srcHeader.stride, rowBytes);
		t.w = srcHeader.width;
		t.h = srcHeader.height;
		t.ready = true;
		return TRUE;
	}

	LRESULT CALLBACK magHostProc(HWND h, UINT m, WPARAM w, LPARAM l)
	{
		return DefWindowProcW(h, m, w, l);
	}

	class MagnifierCapture
	{
	public:
		~MagnifierCapture() { destroy(); }

		bool create(const RECT& sel, std::initializer_list<HWND> exclude)
		{
			destroy();
			if (!MagInitialize()) return false;
			inited_ = true;
			const HINSTANCE inst = GetModuleHandleW(nullptr);
			WNDCLASSEXW wc{ sizeof(wc) };
			wc.lpfnWndProc = magHostProc;
			wc.hInstance = inst;
			wc.lpszClassName = L"SnowAirMagnifierHost";
			RegisterClassExW(&wc); // 已注册时失败是正常的
			host_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
				wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
			if (!host_) return false;
			SetLayeredWindowAttributes(host_, 0, 0, LWA_ALPHA);
			const int w = (int)std::max(1L, sel.right - sel.left);
			const int h = (int)std::max(1L, sel.bottom - sel.top);
			mag_ = CreateWindowExW(0, WC_MAGNIFIER, L"", WS_CHILD | WS_VISIBLE,
				0, 0, w, h, host_, nullptr, inst, nullptr);
			if (!mag_ || !MagSetImageScalingCallback(mag_, magScalingCallback)) return false;
			ShowWindow(host_, SW_SHOWNOACTIVATE);
			UpdateWindow(host_);
			// 截图工具自身窗口（全屏遮罩、工具条）在系统层排除，而不是靠挖洞
			std::vector<HWND> live;
			for (HWND hwnd : exclude)
				if (hwnd && IsWindow(hwnd)) live.push_back(hwnd);
			filtered_ = live.empty()
				|| MagSetWindowFilterList(mag_, MW_FILTERMODE_EXCLUDE, (int)live.size(), live.data()) != FALSE;
			return true;
		}

		// 排除列表是否真的被系统接受。不能只看 create() 成功：过滤失败时遮罩会被拍进画面。
		bool hasFilter() const { return filtered_; }

		std::vector<BYTE> capture(const RECT& sel)
		{
			if (!mag_) return {};
			g_magFrame.ready = false;
			if (!MagSetWindowSource(mag_, sel)) return {};
			if (!g_magFrame.ready) {
				MSG msg{};
				for (int i = 0; i < 9 && !g_magFrame.ready; ++i) {
					while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
						TranslateMessage(&msg);
						DispatchMessageW(&msg);
					}
					Sleep(10);
				}
			}
			if (!g_magFrame.ready) return {};
			if ((LONG)g_magFrame.w != sel.right - sel.left || (LONG)g_magFrame.h != sel.bottom - sel.top)
				return {};
			return std::move(g_magFrame.pixels);
		}

		void destroy()
		{
			if (mag_) { MagSetImageScalingCallback(mag_, nullptr); mag_ = nullptr; }
			if (host_) { DestroyWindow(host_); host_ = nullptr; }
			if (inited_) { MagUninitialize(); inited_ = false; }
		}
	private:
		HWND host_{ nullptr }, mag_{ nullptr };
		bool inited_{ false };
		bool filtered_{ false };
	};

	// 每个应用（按进程名）上一次校准好的投递参数：下次在这个应用上开始滚动时直接用，
	// 不需要再从“保守速度”慢慢加速——这就是“一开始就是匀速”的做法。
	struct ScrollRate
	{
		double units{ 0.0 };
		double intervalMs{ 0.0 };
	};
	std::mutex g_scrollRateMutex;
	std::unordered_map<std::wstring, ScrollRate> g_scrollRates;

	// 目标窗口所属进程的可执行文件名（小写）；拿不到时返回空串
	std::wstring processNameOf(HWND hwnd)
	{
		if (!hwnd) return {};
		DWORD pid = 0;
		GetWindowThreadProcessId(hwnd, &pid);
		if (!pid) return {};
		HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		if (!proc) return {};
		wchar_t path[MAX_PATH]{};
		DWORD size = MAX_PATH;
		std::wstring name;
		if (QueryFullProcessImageNameW(proc, 0, path, &size)) {
			std::wstring full(path, size);
			const auto slash = full.find_last_of(L"\\/");
			name = (slash == std::wstring::npos) ? full : full.substr(slash + 1);
			for (auto& ch : name) ch = (wchar_t)::towlower(ch);
		}
		CloseHandle(proc);
		return name;
	}

	// 选区中心下面是目标应用的窗口；从 z 序顶部找第一个不属于本进程、可见、可用的顶层窗口，
	// 再下钻到真正处理滚轮的渲染子窗口。
	HWND scrollTargetAt(POINT screenPt)
	{
		const DWORD selfPid = GetCurrentProcessId();
		HWND target = nullptr;
		for (HWND w = GetTopWindow(nullptr); w; w = GetWindow(w, GW_HWNDNEXT)) {
			DWORD pid = 0;
			GetWindowThreadProcessId(w, &pid);
			if (pid == selfPid || !IsWindowVisible(w) || !IsWindowEnabled(w)) continue;
			const LONG_PTR ex = GetWindowLongPtrW(w, GWL_EXSTYLE);
			constexpr LONG_PTR passThrough = WS_EX_LAYERED | WS_EX_TRANSPARENT;
			if ((ex & passThrough) == passThrough) continue;
			RECT bounds{};
			if (GetWindowRect(w, &bounds) && PtInRect(&bounds, screenPt)) {
				target = w;
				break;
			}
		}
		for (int guard = 0; target && guard < 16; ++guard) {
			POINT client = screenPt;
			if (!ScreenToClient(target, &client)) return target;
			HWND child = ChildWindowFromPointEx(target, client,
				CWP_SKIPINVISIBLE | CWP_SKIPDISABLED | CWP_SKIPTRANSPARENT);
			if (!child || child == target) break;
			target = child;
		}
		return target;
	}
}

// 按位置滚动用的最小状态：文档总高、当前位置百分比、以及“这个容器到底认不认”的探测结果
struct CapLong::UiaScroll
{
	ComPtr<IUIAutomation> automation;
	ComPtr<IUIAutomationScrollPattern> pattern;
	double totalPx{ 0.0 };   // 文档总高（按 viewSize 换算）
	double pct{ -1.0 };      // 我们命令到的位置百分比
	double startPct{ -1.0 }; // 刚拿到时的位置百分比（用来判断容器是否响应）
	bool confirmed{ false };
	bool disabled{ false };
};

CapLong::CapLong(WinCap* win) : win(win)
{
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), textBrush.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x000000, 0.68f), bgBrush.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x14 / 255.f, 0x16 / 255.f, 0x1c / 255.f, 1.f), viewportBgBrush.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x34 / 255.f, 0xC7 / 255.f, 0x59 / 255.f, 0.95f), previewFrameBrush.GetAddressOf());
}

CapLong::~CapLong()
{
	removeMouseHook();
	stopGrabThread();
	stopStitchThread();
}

void CapLong::dispose()
{
	win->killTimer(autoStepMsgId);
	win->killTimer(pumpMsgId);
	win->killTimer(previewDragPanMsgId);
	removeMouseHook();
	stopGrabThread();
	stopStitchThread();
	ensureExcludeFromCapture(false);
	releaseUiaScroll();
	if (tool) tool->close();
}

void CapLong::ensureExcludeFromCapture(bool on)
{
	if (on == excludedFromCapture) return;
	excludedFromCapture = on;
	if (on) App::excludeFromCapture(win->hwnd);
	else App::clearCaptureExclusion(win->hwnd);
}

bool CapLong::showSelViewport() const
{
	if (resultH <= 0) return false;
	return isFinish || isPaused || previewHot || previewPanning
		|| selViewportPanning || isEditMode();
}

float CapLong::viewRatio() const
{
	return std::clamp(inspectRatio, 0.f, 1.f);
}

float CapLong::viewportRatio() const
{
	const int visH = std::min(imgH, resultH);
	const int maxTop = std::max(1, resultH - visH);
	return std::clamp((float)std::clamp(viewportTop, 0, maxTop) / (float)maxTop, 0.f, 1.f);
}

void CapLong::paint(ID2D1DeviceContext* ctx)
{
	paintImgPreview(ctx);
	paintSelViewport(ctx);
	// 拖边 / 拖移选区时工具条会被临时收起，右上角的文字提示也一起收起，
	// 否则它压在选区边上，量尺寸的时候很碍事
	if (!isCapturing && !isFinish && !toolChromeHidden) paintStartHint(ctx);
	// 滚动太快、这一帧拼不上：给个临时提示（不中断采集）
	if (isCapturing && !isFinish) paintTooFastHint(ctx);
	if (isFinish && layoutTextEnd) {
		auto borderRadius{ 4.f * win->dpi };
		ctx->FillRoundedRectangle(D2D1::RoundedRect(stopTextRect, borderRadius, borderRadius), bgBrush.Get());
		ctx->DrawTextLayout(stopTextPos, layoutTextEnd.Get(), textBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
	}
}

void CapLong::paintStartHint(ID2D1DeviceContext* ctx)
{
	if (!ctx || !win->cutMask || !win->cutMask->hasRect()) return;
	if (!hintLayout || hintDpi != win->dpi) {
		hintLayout = Ling::D2D::get()->makeTextLayout(Lang::get(L"long.hintStart"), 13 * win->dpi);
		hintDpi = win->dpi;
	}
	paintHintBadge(ctx, hintLayout.Get());
}

// 滚动太快、这一帧拼不上时的临时提示（不会中断采集，滚慢一点就自己消失）
void CapLong::paintTooFastHint(ID2D1DeviceContext* ctx)
{
	if (!ctx || tooFastUntilMs.load() <= GetTickCount64()) return;
	if (!hintTooFastLayout || hintTooFastDpi != win->dpi) {
		hintTooFastLayout = Ling::D2D::get()->makeTextLayout(Lang::get(L"long.tooFast"), 13 * win->dpi);
		hintTooFastDpi = win->dpi;
	}
	paintHintBadge(ctx, hintTooFastLayout.Get());
}

// 半透明底 + 文字上下左右居中，位置在选框内部的底部居中
void CapLong::paintHintBadge(ID2D1DeviceContext* ctx, IDWriteTextLayout* layout)
{
	if (!ctx || !layout || !win->cutMask || !win->cutMask->hasRect()) return;
	auto& mask = win->cutMask->maskRect;
	if (mask.right <= mask.left || mask.bottom <= mask.top) return;
	DWRITE_TEXT_METRICS tm{};
	if (FAILED(layout->GetMetrics(&tm)) || tm.width <= 0.f || tm.height <= 0.f) return;

	const float pad = 8.f * win->dpi;
	const float padX = pad + 2.f * win->dpi; // 左右比上下多留 2
	// 底的高度按字号算：布局框含行距，直接用会把上下留白撑得比左右大
	const float boxW = tm.width + padX * 2.f;
	const float boxH = 13.f * win->dpi + pad * 2.f;
	// 距选框底边留一点余量，不要压住选区边线
	const float gap = win->cutMask->strokeWidth + 10.f * win->dpi;
	float left = (mask.left + mask.right) * 0.5f - boxW * 0.5f;
	float top = mask.bottom - gap - boxH;
	// 选区太矮时往上顶会超出选区，夹在选区内部
	top = std::max(mask.top + gap, top);

	if (boxW <= (mask.right - mask.left) - gap * 2.f) {
		left = std::clamp(left, mask.left + gap, mask.right - gap - boxW);
	}
	else {
		// 选区比提示还窄：横向限制在屏幕工作区内，避免跑到屏幕外
		POINT probe{ (LONG)mask.left, (LONG)mask.bottom };
		ClientToScreen(win->hwnd, &probe);
		MONITORINFO mi{ sizeof(MONITORINFO) };
		if (GetMonitorInfo(MonitorFromPoint(probe, MONITOR_DEFAULTTONEAREST), &mi)) {
			POINT pt{ (LONG)std::lround(left), 0 };
			ClientToScreen(win->hwnd, &pt);
			const float maxX = (float)std::max<LONG>(mi.rcWork.left, mi.rcWork.right - (LONG)boxW);
			pt.x = (LONG)std::lround(std::clamp((float)pt.x, (float)mi.rcWork.left, maxX));
			ScreenToClient(win->hwnd, &pt);
			left = (float)pt.x;
		}
	}

	const float radius = std::min(6.f * win->dpi, boxH * 0.5f);
	const D2D1_RECT_F box{ left, top, left + boxW, top + boxH };
	ctx->FillRoundedRectangle(D2D1::RoundedRect(box, radius, radius), bgBrush.Get());
	ctx->DrawTextLayout(
		D2D1::Point2F(box.left + (boxW - tm.width) * 0.5f, box.top + (boxH - tm.height) * 0.5f),
		layout, textBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
}

float CapLong::previewFrameH() const
{
	if (previewFullH <= 0.f || resultH <= 0 || imgH <= 0) return 0.f;
	return std::clamp(previewFullH * ((float)imgH / (float)resultH),
		4.f * win->dpi, previewFullH);
}

void CapLong::syncPreviewPanToRatio(float ratio)
{
	const float maxPan = std::max(0.f, previewFullH - previewViewH);
	if (maxPan <= 0.5f) { previewPanY = 0.f; return; }
	ratio = std::clamp(ratio, 0.f, 1.f);
	if (ratio <= 0.001f) { previewPanY = 0.f; return; }
	if (ratio >= 0.999f) { previewPanY = maxPan; return; }
	// 绿框高度通常小于预览窗：不能用 ratio*maxPan（那会把框推到窗口上方），
	// 应按“框在整张缩略图里的实际位置”滚，并留出上下相等的可视余量。
	const float maxTop = std::max(0.f, previewFullH - previewFrameH());
	const float margin = std::max(0.f, previewViewH - previewFrameH()) * 0.5f;
	previewPanY = std::clamp(ratio * maxTop - margin, 0.f, maxPan);
}

void CapLong::updatePreviewLayout()
{
	previewViewRect = {};
	previewFullH = 0.f;
	previewViewH = 0.f;
	if (!imgPreview || !tool) return;
	const auto sz = imgPreview->GetPixelSize();
	previewFullH = (float)sz.height;
	const float drawW = (float)sz.width;
	// 与工具栏到选区的间距一致：strokeWidth + 2×dpi
	const float gap = (win->cutMask ? win->cutMask->strokeWidth : 0.f) + 2.f * win->dpi;
	const float topMargin = 12.f * win->dpi; // 顶部留白，不贴死屏幕上沿

	HMONITOR mon = MonitorFromWindow(tool->hwnd, MONITOR_DEFAULTTONEAREST);
	MONITORINFO mi{ sizeof(mi) };
	GetMonitorInfo(mon, &mi);
	const float maxH = std::max(80.f, (float)tool->y - gap - (float)mi.rcWork.top - topMargin);
	previewViewH = std::min(previewFullH, maxH);
	const float maxPan = std::max(0.f, previewFullH - previewViewH);

	// 悬停/拖预览时冻结贴顶/贴底，否则内容被每帧拖走（看不清）
	const bool userInspecting = previewHot || previewPanning;
	if (userInspecting) {
		previewPanY = std::clamp(previewPanY, 0.f, maxPan);
		inspectRatio = std::clamp(inspectRatio, 0.f, 1.f);
	}
	else if (isCapturing && !isPaused && !isFinish) {
		// 跟随“当前采集视口”在长图里的位置：往上滚比例变小（预览上移），往下滚变大（往下移）。
		inspectRatio = std::clamp(inspectRatio, 0.f, 1.f);
		syncPreviewPanToRatio(inspectRatio);
	}
	else if (previewFollowTop) {
		previewFollowBottom = false;
		inspectRatio = 0.f;
		syncPreviewPanToRatio(inspectRatio);
	}
	else if (previewFollowBottom) {
		inspectRatio = 1.f;
		syncPreviewPanToRatio(inspectRatio);
	}
	else {
		inspectRatio = std::clamp(inspectRatio, 0.f, 1.f);
		syncPreviewPanToRatio(inspectRatio);
	}

	POINT pos{ tool->x, tool->y - (int)previewViewH - (int)gap };
	// 工具栏在选区底部时，预览改贴选区右侧（放不下则左侧），避免盖住选区
	if (win->cutMask) {
		auto& mask = win->cutMask->maskRect;
		POINT maskR{ (LONG)mask.right, (LONG)mask.top };
		POINT maskL{ (LONG)mask.left, (LONG)mask.top };
		ClientToScreen(win->hwnd, &maskR);
		ClientToScreen(win->hwnd, &maskL);
		HMONITOR monPrev = MonitorFromWindow(tool->hwnd, MONITOR_DEFAULTTONEAREST);
		MONITORINFO miPrev{ sizeof(miPrev) };
		GetMonitorInfo(monPrev, &miPrev);
		POINT screenPos{ 0, tool->y - (int)previewViewH - (int)gap };
		if (maskR.x + (int)gap + (int)drawW <= miPrev.rcWork.right)
			screenPos.x = maskR.x + (int)gap;
		else
			screenPos.x = maskL.x - (int)drawW - (int)gap;
		if (screenPos.x < miPrev.rcWork.left) screenPos.x = miPrev.rcWork.left;
		if (screenPos.x + (int)drawW > miPrev.rcWork.right)
			screenPos.x = miPrev.rcWork.right - (int)drawW;
		if (screenPos.y < miPrev.rcWork.top) screenPos.y = miPrev.rcWork.top;
		pos = screenPos;
	}
	ScreenToClient(win->hwnd, &pos);
	previewViewRect = D2D1::RectF((float)pos.x, (float)pos.y, pos.x + drawW, pos.y + previewViewH);
}

void CapLong::paintImgPreview(ID2D1DeviceContext* ctx)
{
	if (!imgPreview || !tool) return;
	updatePreviewLayout();
	if (previewViewH <= 0.f) return;
	const auto sz = imgPreview->GetPixelSize();
	const float rr = std::min(10.f * win->dpi,
		std::min((previewViewRect.right - previewViewRect.left), (previewViewRect.bottom - previewViewRect.top)) * 0.5f);
	Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> clipGeo;
	Ling::D2D::get()->d2dFactory->CreateRoundedRectangleGeometry(
		D2D1::RoundedRect(previewViewRect, rr, rr), clipGeo.GetAddressOf());
	ctx->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clipGeo.Get()), nullptr);
	D2D1_RECT_F dest{
		previewViewRect.left,
		previewViewRect.top - previewPanY,
		previewViewRect.left + (float)sz.width,
		previewViewRect.top - previewPanY + (float)sz.height
	};
	ctx->DrawBitmap(imgPreview.Get(), dest);
	ctx->PopLayer();
	paintPreviewViewFrame(ctx);
}

void CapLong::paintPreviewViewFrame(ID2D1DeviceContext* ctx)
{
	// 悬停/拖/暂停/结束：线框标出选区当前预览对应的长图片段
	if (!previewFrameBrush) return;
	if (!(previewHot || previewPanning || isPaused || isFinish)) return;
	if (previewViewH <= 0.f || resultH <= imgH || !win->cutMask) return;

	const float frameH = previewFrameH();
	if (frameH <= 0.f) return;
	const float maxTop = std::max(0.f, previewFullH - frameH);
	const float frameTop = viewRatio() * maxTop;
	const float y0 = previewViewRect.top + frameTop - previewPanY;
	const float y1 = y0 + frameH;
	const float left = previewViewRect.left;
	const float right = previewViewRect.right;
	const float top = previewViewRect.top;
	const float bottom = previewViewRect.bottom;
	const float cy0 = std::clamp(y0, top, bottom);
	const float cy1 = std::clamp(y1, top, bottom);
	if (cy1 - cy0 < 1.f) return;

	const float rr = std::min(10.f * win->dpi,
		std::min(right - left, bottom - top) * 0.5f);
	Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> clipGeo;
	Ling::D2D::get()->d2dFactory->CreateRoundedRectangleGeometry(
		D2D1::RoundedRect(previewViewRect, rr, rr), clipGeo.GetAddressOf());
	ctx->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clipGeo.Get()), nullptr);
	const float stroke = std::max(1.5f, 1.5f * win->dpi);
	const float boxW = right - left - stroke;
	const float boxH = cy1 - cy0 - stroke;
	// 与外层预览图的圆角**同心**：预览裁切圆角是 rr，线框在它里面再内缩 stroke/2，
	// 所以线框圆角要同步减掉这份内缩；否则线框顶到预览上/下沿时，四角比裁切弧更靠外而被削掉。
	const float inset = stroke * 0.5f;
	const float boxR = std::min(std::max(0.f, rr - inset), std::min(boxW, boxH) * 0.5f);
	ctx->DrawRoundedRectangle(
		D2D1::RoundedRect(
			D2D1::RectF(left + inset, cy0 + inset, right - inset, cy1 - inset),
			boxR, boxR),
		previewFrameBrush.Get(), stroke);
	ctx->PopLayer();
}

void CapLong::paintSelViewport(ID2D1DeviceContext* ctx)
{
	if (!showSelViewport() || !fullPreviewBmp) return;
	auto& sel = win->cutMask->maskRect;
	ctx->FillRectangle(sel, viewportBgBrush.Get());
	if (fullPreviewW <= 0 || fullPreviewSrcH <= 0) return;

	Microsoft::WRL::ComPtr<ID2D1RectangleGeometry> clipGeo;
	Ling::D2D::get()->d2dFactory->CreateRectangleGeometry(sel, clipGeo.GetAddressOf());
	ctx->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clipGeo.Get()), nullptr);
	// 位图就是当前选中那一屏的 1:1 像素，直接贴到选区左上角
	D2D1_RECT_F dest{
		sel.left,
		sel.top,
		sel.left + (float)fullPreviewW,
		sel.top + (float)fullPreviewSrcH
	};
	ctx->DrawBitmap(fullPreviewBmp.Get(), dest, 1.f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
	ctx->PopLayer();
}

bool CapLong::getSelViewMapping(float& scale, int& srcY, int& srcH) const
{
	if (!win->cutMask || resultH <= 0 || imgW <= 0) return false;
	const auto& sel = win->cutMask->maskRect;
	scale = (sel.right - sel.left) / (float)imgW;
	if (scale <= 0.f) return false;
	// 用 floor，避免 lround 多算 1 行导致暂停时画面往上偏
	srcH = std::max(1, (int)std::floor((sel.bottom - sel.top) / scale + 1e-3f));
	srcH = std::min(srcH, resultH);
	if (imgH > 0 && std::abs(srcH - imgH) <= 1)
		srcH = std::min(imgH, resultH);
	const int maxSrcY = std::max(0, resultH - srcH);
	srcY = std::clamp((int)std::lround(viewRatio() * maxSrcY), 0, maxSrcY);
	return true;
}

POINT CapLong::clientToLongImg(POINT client) const
{
	float scale = 1.f;
	int srcY = 0, srcH = 0;
	if (!getSelViewMapping(scale, srcY, srcH) || !win->cutMask) return client;
	const auto& sel = win->cutMask->maskRect;
	POINT out;
	out.x = (LONG)std::lround((client.x - sel.left) / scale);
	out.y = (LONG)std::lround((client.y - sel.top) / scale) + srcY;
	return out;
}

D2D1_POINT_2F CapLong::longImgToClient(float ix, float iy) const
{
	float scale = 1.f;
	int srcY = 0, srcH = 0;
	if (!getSelViewMapping(scale, srcY, srcH) || !win->cutMask)
		return D2D1::Point2F(ix, iy);
	const auto& sel = win->cutMask->maskRect;
	return D2D1::Point2F(sel.left + ix * scale, sel.top + (iy - (float)srcY) * scale);
}

bool CapLong::beginLongAnnotPaint(ID2D1DeviceContext* ctx, const D2D1_MATRIX_3X2_F& parentXform)
{
	if (!ctx || !showSelViewport() || !win->cutMask) return false;
	float scale = 1.f;
	int srcY = 0, srcH = 0;
	if (!getSelViewMapping(scale, srcY, srcH)) return false;
	const auto& sel = win->cutMask->maskRect;
	Microsoft::WRL::ComPtr<ID2D1RectangleGeometry> clipGeo;
	Ling::D2D::get()->d2dFactory->CreateRectangleGeometry(sel, clipGeo.GetAddressOf());
	ctx->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clipGeo.Get()), nullptr);
	const auto xform = D2D1::Matrix3x2F::Scale(scale, scale)
		* D2D1::Matrix3x2F::Translation(sel.left, sel.top - (float)srcY * scale);
	ctx->SetTransform(xform * parentXform);
	return true;
}

bool CapLong::beginLongPreviewAnnotPaint(ID2D1DeviceContext* ctx, const D2D1_MATRIX_3X2_F& parentXform)
{
	// 与选区同一套 paintShapes，只是缩放到侧栏；每帧绘制，标注立即可见
	if (!ctx || !imgPreview || imgW <= 0) return false;
	updatePreviewLayout();
	if (previewViewH <= 0.f) return false;
	const auto sz = imgPreview->GetPixelSize();
	const float scale = (float)sz.width / (float)imgW;
	if (scale <= 0.f) return false;
	const float rr = std::min(10.f * win->dpi,
		std::min((previewViewRect.right - previewViewRect.left),
			(previewViewRect.bottom - previewViewRect.top)) * 0.5f);
	Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> clipGeo;
	if (FAILED(Ling::D2D::get()->d2dFactory->CreateRoundedRectangleGeometry(
			D2D1::RoundedRect(previewViewRect, rr, rr), clipGeo.GetAddressOf())) || !clipGeo)
		return false;
	ctx->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clipGeo.Get()), nullptr);
	const auto xform = D2D1::Matrix3x2F::Scale(scale, scale)
		* D2D1::Matrix3x2F::Translation(previewViewRect.left, previewViewRect.top - previewPanY);
	ctx->SetTransform(xform * parentXform);
	return true;
}

void CapLong::endLongAnnotPaint(ID2D1DeviceContext* ctx, const D2D1_MATRIX_3X2_F& parentXform)
{
	if (!ctx) return;
	ctx->SetTransform(parentXform);
	ctx->PopLayer();
}

bool CapLong::exportPixels(std::vector<BYTE>& out)
{
	if (!isCapturing && !isFinish) return false;
	flushTail();
	std::lock_guard<std::mutex> lock(canvasMutex);
	if (!stitcher.ready() || stitcher.height() <= 0) return false;
	resultH = stitcher.height();
	if (resultH <= 0 || imgW <= 0) return false;
	return win->composeAnnotOnImage(stitcher.data(), imgW, resultH, out);
}

bool CapLong::hitPreview(POINT clientPos) const
{
	if (previewViewH <= 0.f) return false;
	return clientPos.x >= previewViewRect.left && clientPos.x <= previewViewRect.right
		&& clientPos.y >= previewViewRect.top && clientPos.y <= previewViewRect.bottom;
}

bool CapLong::hitSelViewport(POINT clientPos) const
{
	if (!showSelViewport() || !win->cutMask) return false;
	const auto& r = win->cutMask->maskRect;
	return clientPos.x >= (int)r.left && clientPos.x <= (int)r.right
		&& clientPos.y >= (int)r.top && clientPos.y <= (int)r.bottom;
}

bool CapLong::beginSelViewportPan(POINT clientPos)
{
	if (!hitSelViewport(clientPos) || resultH <= imgH) return false;
	selViewportPanning = true;
	selPanLast = clientPos;
	previewFollowBottom = false;
	previewFollowTop = false;
	previewHot = false;
	ensureSelPreviewBmp();
	win->refresh();
	return true;
}

void CapLong::setInspectRatio(float r, bool fromUser)
{
	const float next = std::clamp(r, 0.f, 1.f);
	if (fromUser) {
		previewFollowTop = (next <= 0.001f);
		previewFollowBottom = (next >= 0.999f);
	}
	if (std::abs(next - inspectRatio) < 0.001f && !fromUser) return;
	inspectRatio = next;
	syncPreviewPanToRatio(inspectRatio);
	if (showSelViewport()) ensureSelPreviewBmp();
	win->refresh();
}

void CapLong::nudgeInspectByDelta(float dyPx)
{
	// 仅滚轮：相对滚动（与点击/拖动的绝对跟手分开）
	dyPx *= 0.28f;
	const float oneScreen = std::max(1.f, previewFullH * ((float)std::max(1, imgH) / (float)std::max(1, resultH)));
	setInspectRatio(inspectRatio + dyPx / (oneScreen * 2.2f), true);
}

void CapLong::scrubInspectAtClientY(float clientY)
{
	updatePreviewLayout();
	if (previewViewH <= 0.f || previewFullH <= 0.f || resultH <= 0) return;

	const float frameH = previewFrameH();
	if (frameH <= 0.f) return;
	const float maxTop = std::max(0.f, previewFullH - frameH);
	const float maxPan = std::max(0.f, previewFullH - previewViewH);
	const float localY = clientY - previewViewRect.top;
	float ratio = 0.f;

	if (frameH < previewViewH - 0.5f) {
		// 拖动绿框：把“框在预览窗里能走的行程”直接映射成整张长图的 0~100。
		// 拖到预览窗顶 = 长图最上面，拖到底 = 最下面，不再依赖边缘缓滚。
		const float travel = previewViewH - frameH;
		const float desiredTop = std::clamp(localY - frameH * 0.5f, 0.f, travel);
		ratio = travel > 0.5f ? desiredTop / travel : 0.f;
		// 缩略图自身跟着滚，让绿框顶边正好落在 desiredTop 处
		previewPanY = std::clamp(ratio * maxTop - desiredTop, 0.f, maxPan);
	}
	else {
		// 绿框几乎占满预览窗（长图只比选区长一点）：没有可拖行程，按整窗高度映射
		ratio = std::clamp(localY / std::max(1.f, previewViewH), 0.f, 1.f);
		previewPanY = std::clamp(ratio * maxPan, 0.f, maxPan);
	}

	previewFollowBottom = (ratio >= 0.999f);
	previewFollowTop = (ratio <= 0.001f);
	inspectRatio = ratio;
	if (showSelViewport()) ensureSelPreviewBmp();
	win->refresh();
}

bool CapLong::autoPanPreviewAtEdge(float clientY)
{
	const float maxPan = std::max(0.f, previewFullH - previewViewH);
	if (maxPan < 0.5f || previewViewH <= 0.f) return false;

	const float edge = std::max(36.f * win->dpi, previewViewH * 0.22f);
	const float maxStep = 4.5f * win->dpi; // 缓慢，方便扫完整长图
	const float ly = clientY - previewViewRect.top;
	float dy = 0.f;
	if (ly < edge) {
		const float t = 1.f - std::clamp(ly / edge, 0.f, 1.f);
		dy = -maxStep * (0.35f + 0.65f * t); // 上沿 → 缩略图上滚
	}
	else if (ly > previewViewH - edge) {
		const float t = 1.f - std::clamp((previewViewH - ly) / edge, 0.f, 1.f);
		dy = maxStep * (0.35f + 0.65f * t); // 下沿 → 缩略图下滚
	}
	if (std::abs(dy) < 0.05f) return false;

	const float next = std::clamp(previewPanY + dy, 0.f, maxPan);
	if (std::abs(next - previewPanY) < 0.05f) return false;
	previewPanY = next;
	previewFollowBottom = (previewPanY >= maxPan - 0.5f);
	return true;
}

void CapLong::startPreviewDragPanTimer()
{
	win->killTimer(previewDragPanMsgId);
	win->setTimer(previewDragPanMs, previewDragPanMsgId);
}

void CapLong::stopPreviewDragPanTimer()
{
	win->killTimer(previewDragPanMsgId);
}

void CapLong::ensureSelPreviewBmp()
{
	if (resultH <= 0 || imgW <= 0) return;
	std::unique_lock<std::mutex> lock(canvasMutex, std::try_to_lock);
	if (!lock.owns_lock() || !stitcher.ready()) return;
	const int canvasH = stitcher.height();
	const BYTE* src = stitcher.data();
	if (!src || canvasH <= 0) return;
	resultH = canvasH;

	// 只建“当前可见那一屏”的 1:1 位图：选区大图本来就是按 1:1 显示的，
	// 存整张长图会超过 D2D 纹理上限，降采样后又会把标注背景糊掉。
	float scale = 1.f;
	int srcY = 0, srcH = 0;
	if (!getSelViewMapping(scale, srcY, srcH)) return;
	if (scale <= 0.f || srcH <= 0) return;
	if (srcY + srcH > canvasH) srcH = std::max(1, canvasH - srcY);
	const int bmpW = std::min(maxPreviewTexDim, std::max(1, (int)std::lround((float)imgW * scale)));
	const int bmpH = std::min(maxPreviewTexDim, std::max(1, (int)std::lround((float)srcH * scale)));
	if (fullPreviewBmp) {
		const auto s = fullPreviewBmp->GetPixelSize();
		if ((int)s.width == bmpW && (int)s.height == bmpH
			&& fullPreviewSrcY == srcY && fullPreviewRows == canvasH
			&& fullPreviewStamp == canvasStamp) {
			fullPreviewW = bmpW;
			fullPreviewSrcH = bmpH;
			return;
		}
	}

	std::vector<BYTE> pixels((size_t)bmpW * 4 * bmpH);
	const size_t srcStride = (size_t)imgW * 4;
	if (bmpW == imgW && bmpH == srcH) {
		memcpy(pixels.data(), src + (size_t)srcY * srcStride, (size_t)srcH * srcStride);
	}
	else {
		// 选区与抓帧宽度一般相等（scale=1）；只有 DPI/跨屏极端情况才需要横向重采样。
		const float inv = 1.f / scale;
		for (int y = 0; y < bmpH; ++y) {
			const int sy = std::min(canvasH - 1, srcY + (int)((float)y * inv));
			const BYTE* srcRow = src + (size_t)sy * srcStride;
			BYTE* dst = pixels.data() + (size_t)y * bmpW * 4;
			for (int x = 0; x < bmpW; ++x) {
				const BYTE* p = srcRow + (size_t)std::min(imgW - 1, (int)((float)x * inv)) * 4;
				dst[(size_t)x * 4] = p[0];
				dst[(size_t)x * 4 + 1] = p[1];
				dst[(size_t)x * 4 + 2] = p[2];
				dst[(size_t)x * 4 + 3] = p[3];
			}
		}
	}

	D2D1_BITMAP_PROPERTIES1 props = {
		.pixelFormat{D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)},
		.dpiX{96.0f}, .dpiY{96.0f}, .bitmapOptions{D2D1_BITMAP_OPTIONS_NONE}
	};
	if (fullPreviewBmp) {
		const auto s = fullPreviewBmp->GetPixelSize();
		if ((int)s.width == bmpW && (int)s.height == bmpH) {
			// 尺寸没变：直接覆盖像素，避免反复创建 D2D 位图
			fullPreviewBmp->CopyFromMemory(nullptr, pixels.data(), (UINT32)(bmpW * 4));
		}
		else {
			fullPreviewBmp.Reset();
		}
	}
	if (!fullPreviewBmp) {
		Ling::D2D::get()->deviceContext->CreateBitmap(
			D2D1::SizeU((UINT32)bmpW, (UINT32)bmpH), pixels.data(), (UINT32)(bmpW * 4),
			props, fullPreviewBmp.GetAddressOf());
	}
	fullPreviewW = bmpW;
	fullPreviewSrcY = srcY;
	fullPreviewSrcH = bmpH;
	fullPreviewRows = canvasH;
	fullPreviewStamp = canvasStamp;
}

void CapLong::syncPierceWithPreviewHot()
{
	if (!isCapturing || isPaused || isFinish) return;
	// 悬停/拖右侧预览时要在选区画大图，必须暂时取消挖洞。
	// 只有抓帧线程确实走放大镜（窗口已加入窗口过滤列表）时才能安全恢复：
	// 这时遮罩不会进入抓帧；否则会拍到遮罩，只能保持挖洞。
	if (!manualMagActive) {
		win->hollowWin();
		raiseTool();
		return;
	}
	if (previewHot || previewPanning) {
		win->restoreWin();
		ensureSelPreviewBmp();
	}
	else {
		win->hollowWin();
		raiseTool();
	}
}

void CapLong::setCursor()
{
	POINT cur{};
	GetCursorPos(&cur);
	ScreenToClient(win->hwnd, &cur);
	if (previewHot || previewPanning || selViewportPanning
		|| (isEditMode() && resultH > imgH && hitSelViewport(cur))) {
		SetCursor(LoadCursor(nullptr, IDC_SIZENS));
		return;
	}
	// 未开始滚动：光标与 Adjust 阶段一致，提示选区可拖边 / 可整体移动
	if (!isCapturing && !isFinish && win->cutMask && win->cutMask->hasRect()) {
		switch (win->cutMask->hitTest(cur)) {
		case MaskHit::TopLeft:
		case MaskHit::BottomRight:
			SetCursor(LoadCursor(nullptr, IDC_SIZENWSE));
			return;
		case MaskHit::TopRight:
		case MaskHit::BottomLeft:
			SetCursor(LoadCursor(nullptr, IDC_SIZENESW));
			return;
		case MaskHit::Left:
		case MaskHit::Right:
			SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
			return;
		case MaskHit::Top:
		case MaskHit::Bottom:
			SetCursor(LoadCursor(nullptr, IDC_SIZENS));
			return;
		case MaskHit::Inside:
			SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
			return;
		default:
			break;
		}
	}
	SetCursor(LoadCursor(nullptr, IDC_ARROW));
}

void CapLong::onDown(POINT pos, bool isRight)
{
	if (isRight) return;
	updatePreviewLayout();
	if (hitPreview(pos) && resultH > imgH) {
		previewPanning = true;
		previewHot = true;
		previewFollowBottom = false;
		previewFollowTop = false;
		// 拖动期间由定时器在贴近上下沿时持续缓滚；不先启动会在 onUp 之前一直不触发
		startPreviewDragPanTimer();
		syncPierceWithPreviewHot();
		// 拖动开始前一定要有大图位图，否则 paintSelViewport 会直接跳过，
		// 表现为“拖右侧预览没反应”（尤其刚恢复窗口时）。
		ensureSelPreviewBmp();
		scrubInspectAtClientY((float)pos.y);
	}
}

void CapLong::onMove(POINT pos)
{
	if (selViewportPanning) {
		const float dy = (float)(pos.y - selPanLast.y);
		selPanLast = pos;
		if (std::abs(dy) > 0.01f) nudgeInspectByDelta(dy);
		return;
	}
	if (previewPanning) {
		updatePreviewLayout();
		scrubInspectAtClientY((float)pos.y);
		return;
	}

	updatePreviewLayout();
	const bool hot = hitPreview(pos);
	if (hot != previewHot) {
		previewHot = hot;
		if (hot) {
			// 悬停冻结：采集阶段先对齐到当前滚动视口，避免使用上一帧的旧位置；
			// 暂停/结束阶段则保留用户正在浏览的位置。
			inspectRatio = (isCapturing && !isPaused && !isFinish) ? viewportRatio() : viewRatio();
			syncPreviewPanToRatio(inspectRatio);
		}
		else if (isCapturing && !isPaused && !isFinish) {
			// 悬停浏览结束：回到当前采集视口对应的位置，继续随滚动方向移动
			inspectRatio = viewportRatio();
			previewFollowTop = inspectRatio <= 0.001f;
			previewFollowBottom = inspectRatio >= 0.999f;
			syncPreviewPanToRatio(inspectRatio);
		}
		syncPierceWithPreviewHot();
		if (hot && (isPaused || isFinish || isEditMode())) ensureSelPreviewBmp();
		win->refresh();
	}
}

void CapLong::onUp(POINT pos)
{
	if (selViewportPanning) {
		selViewportPanning = false;
		win->refresh();
		return;
	}
	if (previewPanning) {
		previewPanning = false;
		stopPreviewDragPanTimer();
		syncPierceWithPreviewHot();
		win->refresh();
		return;
	}
	// 未开始的点击不再触发滚动：开始只走空格 / 工具条图标 / 滚轮
}

bool CapLong::onWheel(POINT pos, float space)
{
	bool inside = false;
	if (win->cutMask) {
		const auto& r = win->cutMask->maskRect;
		inside = pos.x >= (int)r.left && pos.x <= (int)r.right
			&& pos.y >= (int)r.top && pos.y <= (int)r.bottom;
	}

	// 编辑标注：滚轮只作用预览画面（选区大图 / 右侧预览），
	// 不再启动手动滚动，也绝不把滚轮转给桌面。
	if (isEditMode()) {
		if (resultH <= imgH) return false;
		updatePreviewLayout();
		if (inside) {
			// 编辑标注时滚轮翻“选区大图”：一格约 80% 屏，而不是缩略图那种细微步进
			const float notch = space / (60.f * std::max(1.f, win->dpi));
			const int visibleH = std::min(imgH, resultH);
			const int maxSrcY = std::max(1, resultH - visibleH);
			setInspectRatio(inspectRatio - notch * 0.8f * (float)visibleH / (float)maxSrcY, true);
			return true;
		}
		if (hitPreview(pos)) {
			previewHot = true;
			previewFollowBottom = false;
			previewFollowTop = false;
			nudgeInspectByDelta(-space * 0.16f);
			return true;
		}
		return false;
	}

	if (!isFinish && win->cutMask && inside) {
		// 选区内滚轮：进入手动滚动，并把这一下滚轮转给页面
		startManualScroll(space);
		return true;
	}
	// 侧栏预览可滚轮浏览；截图视窗不响应滚轮翻页
	updatePreviewLayout();
	if (resultH <= imgH) return false;
	if (!hitPreview(pos)) return false;
	previewHot = true;
	previewFollowBottom = false;
	previewFollowTop = false;
	syncPierceWithPreviewHot();
	ensureSelPreviewBmp();
	nudgeInspectByDelta(-space * 0.16f);
	return true;
}

void CapLong::toggleCapture()
{
	if (isFinish) return;
	if (!isCapturing) { startCapture(); return; }
	// 手动滚动中点按钮：把滚动交给自动（无缝接上）；自动中则是暂停/继续。
	if (manualMode) { switchToAuto(); return; }
	setPaused(!isPaused);
}

void CapLong::startCapture()
{
	if (isFinish) return;
	if (!isCapturing) { startCaptureMode(false); return; }
	if (manualMode) switchToAuto();
	else if (isPaused) setPaused(false);
}

void CapLong::startManualScroll(float wheelSpace)
{
	if (isFinish) return;
	if (!isCapturing) {
		startCaptureMode(true);
		if (!isCapturing || isFinish) return;
	}
	else if (!manualMode) {
		switchToManual();
	}
	if (isPaused) setPaused(false);
	// 这一下滚轮被本窗口收到了（说明页面没收到），按原方向和原幅度补发一次
	int delta = (int)std::lround(wheelSpace * WHEEL_DELTA / (60.f * std::max(1.f, win->dpi)));
	if (delta == 0) delta = wheelSpace >= 0.f ? WHEEL_DELTA : -WHEEL_DELTA;
	sendWheelDelta(delta);
}

void CapLong::switchToAuto()
{
	if (!isCapturing || isFinish || !manualMode) return;
	manualMode = false;
	isPaused = false;
	win->hollowWin();
	ensureExcludeFromCapture(true);
	raiseTool();
	installMouseHook();
	startGrabThread();
	startStitchThread();
	win->setTimer(pumpTickMs, pumpMsgId);
	initUiaScroll();
	startAutoScroll();
	syncToolBtn();
	win->refresh();
}

void CapLong::switchToManual()
{
	if (!isCapturing || isFinish || manualMode) return;
	manualMode = true;
	isPaused = false;
	stopAutoScroll();
	manualUnits = 0;
	manualScaleMs = GetTickCount64();
	if (rateKey_.empty()) {
		if (HWND target = scrollTarget()) rateKey_ = processNameOf(target);
	}
	win->setTimer(pumpTickMs, pumpMsgId);
	win->hollowWin();
	ensureExcludeFromCapture(true);
	raiseTool();
	startGrabThread();
	startStitchThread();
	previewDirty = true; // 切换后立刻刷一次缩略图，让用户看到当前进度
	syncToolBtn();
	win->refresh();
}

void CapLong::sendWheelDelta(int delta)
{
	if (!delta) return;
	INPUT input{};
	input.type = INPUT_MOUSE;
	input.mi.dwFlags = MOUSEEVENTF_WHEEL;
	input.mi.mouseData = (DWORD)delta;
	input.mi.dwExtraInfo = syntheticWheelMark;
	SendInput(1, &input, sizeof(INPUT));
}

void CapLong::resolveScrollTarget()
{
	if (!win->cutMask) return;
	auto& r = win->cutMask->maskRect;
	scrollCenterScreen.x = (LONG)((r.left + r.right) * 0.5f);
	scrollCenterScreen.y = (LONG)((r.top + r.bottom) * 0.5f);
	ClientToScreen(win->hwnd, &scrollCenterScreen);
}

HWND CapLong::scrollTarget()
{
	const ULONGLONG now = GetTickCount64();
	// 目标窗口每 0.5 秒重新找一次：滚动期间每秒要投递上百次，不能每次都枚举窗口
	if (scrollTargetCache && IsWindow(scrollTargetCache) && now - scrollTargetCacheMs < 500)
		return scrollTargetCache;
	resolveScrollTarget();
	scrollTargetCache = scrollTargetAt(scrollCenterScreen);
	scrollTargetCacheMs = now;
	return scrollTargetCache;
}

bool CapLong::postWheelStep(int delta)
{
	if (!win->cutMask || delta == 0) return false;
	const HWND target = scrollTarget();
	if (!target) return false;
	// 投递到目标窗口：不动光标、不抢焦点，用户的鼠标可以停在任意位置。
	const WPARAM wheelParam = MAKEWPARAM(0, (WORD)delta);
	const LPARAM pointParam = MAKELPARAM((short)scrollCenterScreen.x, (short)scrollCenterScreen.y);
	return PostMessageW(target, WM_MOUSEWHEEL, wheelParam, pointParam) != FALSE;
}

bool CapLong::injectWheelStep(int delta)
{
	if (!win->cutMask || delta == 0) return false;
	resolveScrollTarget();
	// 光标已经在选区内时滚轮天然会落到页面上，不必挪动用户的鼠标；
	// 只有光标在选区外才临时挪一下再挪回来。
	POINT cursor{};
	bool moved = false;
	if (GetCursorPos(&cursor)) {
		POINT client = cursor;
		ScreenToClient(win->hwnd, &client);
		const auto& r = win->cutMask->maskRect;
		const bool insideSel = client.x >= r.left && client.x <= r.right
			&& client.y >= r.top && client.y <= r.bottom;
		if (!insideSel) moved = SetCursorPos(scrollCenterScreen.x, scrollCenterScreen.y) != FALSE;
	}
	sendWheelDelta(delta);
	if (moved) SetCursorPos(cursor.x, cursor.y);
	return true;
}

void CapLong::installMouseHook()
{
	if (mouseHook) return;
	g_mouseHookOwner = this;
	mouseHook = SetWindowsHookExW(WH_MOUSE_LL, mouseHookProc, GetModuleHandleW(nullptr), 0);
	if (!mouseHook && g_mouseHookOwner == this) g_mouseHookOwner = nullptr;
}

void CapLong::removeMouseHook()
{
	if (mouseHook) {
		UnhookWindowsHookEx(mouseHook);
		mouseHook = nullptr;
	}
	if (g_mouseHookOwner == this) g_mouseHookOwner = nullptr;
	takeoverPending = false;
}

LRESULT CALLBACK CapLong::mouseHookProc(int code, WPARAM wParam, LPARAM lParam)
{
	if (code == HC_ACTION && g_mouseHookOwner) {
		const MSLLHOOKSTRUCT* info = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
		// 只旁听真实输入：程序自己注入的滚轮带标记，不能当成用户操作
		if (info && (info->flags & LLMHF_INJECTED) == 0 && info->dwExtraInfo != syntheticWheelMark) {
			if (wParam == WM_MOUSEWHEEL)
				g_mouseHookOwner->noteUserWheel(info->pt, GET_WHEEL_DELTA_WPARAM(info->mouseData));
			else if (wParam == WM_MBUTTONDOWN)
				g_mouseHookOwner->noteUserWheel(info->pt, 0);
		}
	}
	return CallNextHookEx(nullptr, code, wParam, lParam);
}

void CapLong::noteUserWheel(POINT screenPt, int wheelDelta)
{
	if (!isCapturing || isFinish) return;
	if (manualMode) {
		// 手动滚动时顺手累计“用户滚了多少滚轮单位”：配合画布实际位移就能算出
		// 这个应用的滚轮刻度，切回自动时直接按正确速度滑，不需要再试探。
		if (wheelDelta) manualUnits += std::abs(wheelDelta);
		return;
	}
	if (!win->cutMask) return;
	requestManualTakeover(screenPt);
}

void CapLong::requestManualTakeover(POINT screenPt)
{
	if (takeoverPending) return;
	POINT client = screenPt;
	ScreenToClient(win->hwnd, &client);
	const auto& r = win->cutMask->maskRect;
	if (client.x < (LONG)r.left || client.x > (LONG)r.right
		|| client.y < (LONG)r.top || client.y > (LONG)r.bottom)
		return;
	updatePreviewLayout();
	if (previewViewH > 0.f && hitPreview(client)) return; // 预览窗滚轮是浏览预览
	takeoverPending = true;
	// 钩子回调就在本线程的消息循环里，切换动作很轻（线程已在跑、只改状态），直接切；
	// 不排到定时器里，省得定时器没触发时一直挂着“待接管”。
	switchToManual();
	takeoverPending = false;
}

void CapLong::onTimerCB(UINT timerId)
{
	// 拖动预览：贴边时持续缓滚缩略图（暂停/结束后也能用）
	if (timerId == previewDragPanMsgId) {
		win->killTimer(previewDragPanMsgId);
		if (!previewPanning) return;
		POINT pt{};
		GetCursorPos(&pt);
		ScreenToClient(win->hwnd, &pt);
		updatePreviewLayout();
		if (autoPanPreviewAtEdge((float)pt.y))
			scrubInspectAtClientY((float)pt.y);
		startPreviewDragPanTimer();
		return;
	}

	if (timerId == autoStepMsgId) {
		autoPulseTick();
		return;
	}
	if (timerId == pumpMsgId) {
		pumpTick();
		return;
	}
}

void CapLong::startAutoScroll()
{
	win->killTimer(autoStepMsgId);
	autoPulseAccum = 0.0;
	autoPulseMs = 0;
	movedPx = 0;
	uiaPxCarry = 0.0;
	autoStartMs = GetTickCount64();
	speedSampleMs = autoStartMs;
	autoUseInjection = false;
	lastMotionMs = autoStartMs;
	// 滚轮回退路径的起步值：先按“常见应用的滚轮刻度”直接算到目标速度，
	// 不在低速上试探；这个应用之前校准过的话，直接用上次收敛好的参数。
	// （能按位置滚动的容器根本走不到这里，它们的速度由位置精确给定。）
	{
		const double targetPx = std::max(60.0, (double)imgH * autoTargetViewportPerSec);
		autoPulseIntervalMs = autoMinPulseIntervalMs;
		autoUnitsPerPulse = std::clamp(
			targetPx / autoAssumedPxPerUnit * autoPulseIntervalMs / 1000.0, 1.0, 64.0);
	}
	autoQuickSamples = 2;
	rateKey_.clear();
	if (HWND target = scrollTarget()) {
		rateKey_ = processNameOf(target);
		if (!rateKey_.empty()) {
			std::lock_guard<std::mutex> lock(g_scrollRateMutex);
			auto it = g_scrollRates.find(rateKey_);
			if (it != g_scrollRates.end() && it->second.units >= 1.0 && it->second.intervalMs >= autoMinPulseIntervalMs) {
				autoUnitsPerPulse = it->second.units;
				autoPulseIntervalMs = it->second.intervalMs;
				autoQuickSamples = 0; // 已知刻度：不用再试
			}
		}
	}
	// 定时器只是“节拍源”，真正发不发、发多少由 autoPulseAccum / autoPulseIntervalMs 决定
	win->setTimer(autoTickMs, autoStepMsgId);
}

// —— 按位置滚动（UIA ScrollPattern）——
// 滚轮事件一定会被目标应用“平滑”一次：它自己的动画有加速段，我们发得再均匀，
// 页面也会先慢后快。所以自动滚动优先直接给“位置”：每拍把滚动位置推进 speed×dt 像素。
// 位置由我们给定，因此从第一拍起就是匀速，没有任何加速段。
// 容器不暴露 ScrollPattern（自绘滚动容器）时，退回滚轮事件流。
bool CapLong::initUiaScroll()
{
	releaseUiaScroll();
	auto impl = std::make_unique<UiaScroll>();
	// 进程内只初始化一次 COM；之后即使借用 UI 线程已有的套间也不再配对 CoUninitialize
	static bool comReady = false;
	if (!comReady) {
		const HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
		if (!SUCCEEDED(coHr) && coHr != RPC_E_CHANGED_MODE) return false;
		comReady = true;
	}
	if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
			IID_PPV_ARGS(impl->automation.GetAddressOf()))) || !impl->automation)
		return false;
	ComPtr<IUIAutomation2> automation2;
	if (SUCCEEDED(impl->automation.As(&automation2)) && automation2) {
		automation2->put_ConnectionTimeout(200);
		automation2->put_TransactionTimeout(200);
	}

	resolveScrollTarget();
	ComPtr<IUIAutomationElement> el;
	if (FAILED(impl->automation->ElementFromPoint(scrollCenterScreen, el.GetAddressOf())) || !el)
		return false;

	const ULONGLONG walkStart = GetTickCount64();
	for (int depth = 0; depth < 10 && el; ++depth) {
		if (GetTickCount64() - walkStart > 250) break; // 跨进程查询总预算，避免开始滚动卡太久
		CONTROLTYPEID type = 0;
		el->get_CurrentControlType(&type);
		const bool scrollableKind = type == UIA_DocumentControlTypeId
			|| type == UIA_PaneControlTypeId
			|| type == UIA_ListControlTypeId
			|| type == UIA_TreeControlTypeId
			|| type == UIA_TableControlTypeId
			|| type == UIA_DataGridControlTypeId
			|| type == UIA_GroupControlTypeId;
		ComPtr<IUIAutomationScrollPattern> pattern;
		if (scrollableKind && SUCCEEDED(el->GetCurrentPatternAs(UIA_ScrollPatternId,
				IID_PPV_ARGS(pattern.GetAddressOf()))) && pattern) {
			BOOL vertical = FALSE;
			double pct = -1.0, view = 0.0;
			RECT bounds{};
			if (SUCCEEDED(pattern->get_CurrentVerticallyScrollable(&vertical)) && vertical
				&& SUCCEEDED(pattern->get_CurrentVerticalScrollPercent(&pct)) && pct >= 0.0
				&& SUCCEEDED(pattern->get_CurrentVerticalViewSize(&view)) && view > 0.0 && view < 100.0
				&& SUCCEEDED(el->get_CurrentBoundingRectangle(&bounds))
				&& bounds.bottom > bounds.top + 32) {
				const double viewportPx = (double)(bounds.bottom - bounds.top);
				impl->totalPx = viewportPx * 100.0 / view;
				impl->pct = pct;
				impl->startPct = pct;
				impl->pattern = pattern;
				uia = std::move(impl);
				return true;
			}
		}
		ComPtr<IUIAutomationTreeWalker> walker;
		impl->automation->get_ControlViewWalker(walker.GetAddressOf());
		ComPtr<IUIAutomationElement> parent;
		if (!walker || FAILED(walker->GetParentElement(el.Get(), parent.GetAddressOf())) || !parent)
			break;
		el = parent;
	}
	return false;
}

void CapLong::releaseUiaScroll()
{
	uia.reset();
}

// 把滚动位置推进 px 像素（正数向下）。返回 false 表示这个容器不可用。
bool CapLong::uiaDrive(double px)
{
	if (!uia || uia->disabled || !uia->pattern || uia->pct < 0.0 || uia->totalPx < 100.0) return false;
	const double next = std::min(100.0, uia->pct + std::max(0.0, px) / uia->totalPx * 100.0);
	if (next <= uia->pct + 1e-9) return true; // 已经到底：位置不再前进
	if (FAILED(uia->pattern->SetScrollPercent(-1.0, next))) {
		uia->disabled = true;
		return false;
	}
	uia->pct = next;
	return true;
}

void CapLong::stopAutoScroll()
{
	win->killTimer(autoStepMsgId);
	autoPulseMs = 0;
	autoPulseAccum = 0.0;
	movedPx = 0;
}

// 自动滚动的节拍处理，两种驱动共用“到底”判定：
//   1) 能拿到滚动位置时直接按位置推进（匀速，没有应用自身的加速动画）；
//   2) 否则退回滚轮事件流：小份高频投递，并按“画布实际滚过的像素”闭环校准速度。
void CapLong::autoPulseTick()
{
	if (!isCapturing || isPaused || isFinish || manualMode) return;
	// 悬停浏览预览时不滚，也不计入时间（恢复后不会补一大跳）
	const ULONGLONG now = GetTickCount64();
	if (isInspecting() || selViewportPanning) {
		autoPulseMs = now;
		return;
	}
	const ULONGLONG sinceMotion = now - lastMotionMs.load();
	double dt = (now - autoPulseMs) / 1000.0;
	autoPulseMs = now;
	dt = std::clamp(dt, 0.002, 0.05);

	// 到底：画面不再滚动（两种驱动共用这一个判据）
	if (now - autoStartMs > autoStartGraceMs && sinceMotion > autoBottomStopMs) {
		stoppedAtEnd = true;
		stopCap();
		return;
	}

	// —— 位置驱动：每拍把位置推进 speed×dt 像素 ——
	// 位置是我们给定的，目标应用没有“自己的动画”可加速，所以从第一拍就是匀速。
	if (uia && !uia->disabled) {
		const double targetPx = std::max(60.0, (double)imgH * autoTargetViewportPerSec);
		uiaPxCarry += targetPx * dt;
		const double step = std::floor(uiaPxCarry);
		if (step >= 1.0) {
			uiaPxCarry -= step;
			uiaDrive(step);
		}
		// 探头：开始后 160ms 看滚动位置有没有真的跟着我们走
		if (!uia->confirmed && now - autoStartMs > 160) {
			double cur = -1.0;
			if (uia->pattern && SUCCEEDED(uia->pattern->get_CurrentVerticalScrollPercent(&cur))
				&& cur > uia->startPct + 1e-4)
				uia->confirmed = true;
			else
				uia->disabled = true; // 这个容器不认百分比：退回滚轮事件流
		}
		if (!uia->disabled) return;
	}

	// —— 滚轮事件流（容器不支持按位置滚动时）——
	// 速度闭环。窗口长度必须用真实经过的时间：给窗口设下限会让短窗口里测出的速度偏低，
	// 控制器就会误判“太慢”而在下一拍成倍加速——这正是起步出现加速段的来源。
	// 另外把“每次投递量 / 事件间隔”按比例直接改到位，不做逐轮逼近。
	const ULONGLONG windowMs = autoQuickSamples > 0 ? autoQuickSampleMs : autoSpeedSampleMs;
	if (now - speedSampleMs >= windowMs) {
		const ULONGLONG elapsedMs = now - speedSampleMs;
		speedSampleMs = now;
		const int moved = movedPx.exchange(0);
		if (moved > 8) {
			const double secs = std::max(0.005, (double)elapsedMs / 1000.0);
			const double target = std::max(60.0, (double)imgH * autoTargetViewportPerSec);
			const double actual = (double)moved / secs;
			// 比例直接用“目标/实测”，一轮就到位，不做逐轮逼近
			const double factor = std::clamp(target / std::max(1.0, actual), 0.35, 6.0);
			if (autoQuickSamples > 0) --autoQuickSamples;
			// 吞吐 = 每次投递量 / 事件间隔。先把 factor 全部压到“每次投递量”上，
			// 只有它已经到 1 或 64（压不动）时，剩下的部分交给事件间隔。
			const double a = std::clamp(factor, 1.0 / autoUnitsPerPulse, 64.0 / autoUnitsPerPulse);
			autoUnitsPerPulse = std::clamp(autoUnitsPerPulse * a, 1.0, 64.0);
			const double b = a / factor; // 间隔需要乘的倍数（等价于把剩余差额补上）
			autoPulseIntervalMs = std::clamp(autoPulseIntervalMs * b, autoMinPulseIntervalMs, 80.0);
			// 记住这个应用的刻度，下次一开始就是匀速
			if (!rateKey_.empty()) {
				std::lock_guard<std::mutex> lock(g_scrollRateMutex);
				g_scrollRates[rateKey_] = ScrollRate{ autoUnitsPerPulse, autoPulseIntervalMs };
			}
		}
	}

	// 投递无效：目标窗口多半不理会投递的滚轮，改用真实输入
	if (!autoUseInjection && now - autoStartMs > autoStartGraceMs && sinceMotion > autoInjectionFallbackMs)
		autoUseInjection = true;

	autoPulseAccum += dt * 1000.0;
	if (autoPulseAccum < autoPulseIntervalMs) return;
	// 只扣掉一个间隔、保留余量：定时器抖动不会让平均事件频率变慢（变慢就会被当成“要加速”）
	autoPulseAccum -= autoPulseIntervalMs;
	if (autoPulseAccum > autoPulseIntervalMs * 3.0) autoPulseAccum = 0.0;

	const int units = std::max(1, (int)std::lround(autoUnitsPerPulse));
	const int delta = -units; // 负数 = 页面向下滚
	if (autoUseInjection) injectWheelStep(delta);
	else if (!postWheelStep(delta)) {
		autoUseInjection = true;
		injectWheelStep(delta);
	}
}

void CapLong::pumpTick()
{
	if (isFinish) return;
	if (hitMaxHeight && isCapturing) {
		stopCap();
		return;
	}
	// “滚得太快”提示的出没：拼接线程只写截止时间，界面这边负责重绘一次
	{
		const bool show = isCapturing && tooFastUntilMs.load() > GetTickCount64();
		if (show != hintTooFastShown) {
			hintTooFastShown = show;
			win->refresh();
		}
	}
	// 手动滚动时顺手量一下这个应用的滚轮刻度：用户滚过的滚轮单位 与 画布实际位移之比。
	// 量到之后切回自动就是“一开始就匀速”，不用再试探。
	if (manualMode && isCapturing && !isPaused) {
		const ULONGLONG now = GetTickCount64();
		if (now - manualScaleMs >= 300) {
			manualScaleMs = now;
			const int units = manualUnits.exchange(0);
			const int moved = movedPx.exchange(0);
			if (units >= 120 && moved > 16 && !rateKey_.empty()) {
				const double pxPerUnit = (double)moved / (double)units;
				const double targetPx = std::max(60.0, (double)imgH * autoTargetViewportPerSec);
				const double interval = autoMinPulseIntervalMs;
				const double per = std::clamp(
					targetPx / std::max(0.05, pxPerUnit) * interval / 1000.0, 1.0, 64.0);
				std::lock_guard<std::mutex> lock(g_scrollRateMutex);
				g_scrollRates[rateKey_] = ScrollRate{ per, interval };
			}
		}
	}
	// 预览是整张长图的 O(长图高度) 重建，按固定节拍刷新，避免拖慢拼接
	if (previewDirty.exchange(false)) {
		const ULONGLONG now = GetTickCount64();
		if (now - lastPreviewMs >= previewRefreshMs) {
			if (!previewHot && !previewPanning && !selViewportPanning) {
				inspectRatio = viewportRatio();
				previewFollowTop = inspectRatio <= 0.001f;
				previewFollowBottom = inspectRatio >= 0.999f;
				syncPreviewPanToRatio(inspectRatio);
			}
			// 拼接线程正占着画布时这一拍先跳过，脏标记留着下一拍再刷
			if (makeImgPreview()) {
				lastPreviewMs = now;
				win->refresh();
			}
			else {
				previewDirty = true;
			}
		}
		else {
			previewDirty = true;
		}
	}
}

void CapLong::startCaptureMode(bool manual)
{
	if (isCapturing || isFinish) return;
	manualMode = manual;
	isCapturing = true;
	isPaused = false;
	if (!tool) makeTool();
	// 滚动必须挖洞：真实滚轮能落到页面上，自动注入的滚轮也不会被自绘覆盖层吃掉
	win->hollowWin();
	ensureExcludeFromCapture(true);
	if (win->cutMask) {
		win->cutMask->hideLabel = true;
		win->cutMask->showHandles = false;
	}
	raiseTool();
	previewFollowTop = false;
	previewFollowBottom = true;
	inspectRatio = 1.f;
	autoUseInjection = false;
	lastShift = 0;
	// 目标应用（按进程名）决定“滚轮刻度”缓存键：手动阶段量到的刻度也记在它名下
	rateKey_.clear();
	if (HWND target = scrollTarget()) rateKey_ = processNameOf(target);
	manualUnits = 0;
	manualScaleMs = GetTickCount64();
	firstStep();
	if (isFinish) {
		syncToolBtn();
		win->refresh();
		return;
	}
	installMouseHook();
	startGrabThread();
	startStitchThread();
	syncToolBtn();
	win->refresh();
}

void CapLong::resetStitchState()
{
	std::lock_guard<std::mutex> lock(canvasMutex);
	stitcher.reset();
	canvasStamp = 0;
	fullPreviewSrcY = -1;
	fullPreviewRows = 0;
	previewDirty = false;
	resultH = 0;
	viewportTop = 0;
	stoppedAtEnd = false;
	hitMaxHeight = false;
	lastShift = 0;
	lastPreviewMs = 0;
	lastFullPreviewMs = 0;
	std::lock_guard<std::mutex> fl(frameMutex);
	latestFrame.clear();
	latestFresh = false;
}

void CapLong::firstStep()
{
	resetStitchState();
	auto& maskRect = win->cutMask->maskRect;
	imgW = (int)(maskRect.right - maskRect.left);
	imgH = (int)(maskRect.bottom - maskRect.top);
	if (imgW <= 0 || imgH <= 0) { stopCap(); return; }
	resultH = imgH;
	capStartPos.x = (int)maskRect.left;
	capStartPos.y = (int)maskRect.top;
	ClientToScreen(win->hwnd, &capStartPos);

	int maxHeight = maxLongHeight;
	{
		const size_t rowBytes = (size_t)std::max(1, imgW) * 4;
		const int rowsByMem = (int)((256ull << 20) / rowBytes);
		maxHeight = std::min(maxLongHeight, std::max(imgH * 2, rowsByMem));
	}
	capPipeline = -1;
	auto first = captureFrame();
	if (first.size() < (size_t)imgW * imgH * 4) {
		// 首帧都拿不到（受保护内容/采集接口异常）：直接收尾，避免进入空转的采集状态
		stoppedAtEnd = false;
		stopCap();
		return;
	}
	bool began = false;
	{
		std::lock_guard<std::mutex> lock(canvasMutex);
		began = stitcher.begin(first.data(), imgW, imgH, imgW * 4, maxHeight, 256ull << 20);
		if (began) {
			resultH = stitcher.height();
			++canvasStamp;
		}
	}
	if (!began) {
		stoppedAtEnd = false;
		stopCap();
		return;
	}
	previewFollowTop = false;
	previewFollowBottom = true;
	makeImgPreview();
	win->refresh();
	win->setTimer(pumpTickMs, pumpMsgId);
	if (!manualMode) {
		initUiaScroll();
		startAutoScroll();
	}
}

std::vector<BYTE> CapLong::captureFrame()
{
	if (imgW <= 0 || imgH <= 0) return {};
	const size_t need = (size_t)imgW * imgH * 4;
	const RECT sel{ capStartPos.x, capStartPos.y, capStartPos.x + imgW, capStartPos.y + imgH };
	auto tryMag = [&]() -> std::vector<BYTE> {
		MagnifierCapture mag;
		if (!mag.create(sel, { win->hwnd, tool ? tool->hwnd : nullptr }) || !mag.hasFilter()) return {};
		auto d = mag.capture(sel);
		mag.destroy();
		return d;
	};
	if (capPipeline < 0) {
		// 首帧用来确定本次会话用的采集管线：后续所有帧沿用同一条，
		// 否则同一张长图里会混入不同色彩管线的帧，出现色带/接缝。
		auto d = tryMag();
		if (d.size() >= need) { capPipeline = 0; return d; }
		auto dxgi = ScreenCapture::capture(capStartPos.x, capStartPos.y, imgW, imgH);
		if (dxgi.size() >= need) { capPipeline = 1; return dxgi; }
		capPipeline = 2;
	}
	if (capPipeline == 0) {
		auto d = tryMag();
		if (d.size() >= need) return d;
		return {};
	}
	if (capPipeline == 1) {
		auto d = ScreenCapture::capture(capStartPos.x, capStartPos.y, imgW, imgH);
		if (d.size() >= need) return d;
		return {};
	}
	return Util::captureScreen(capStartPos.x, capStartPos.y, imgW, imgH);
}

void CapLong::publishFrame(std::vector<BYTE>&& data)
{
	std::lock_guard<std::mutex> lock(frameMutex);
	latestFrame = std::move(data);
	latestFresh = true;
}

bool CapLong::takeFrame(std::vector<BYTE>& out)
{
	std::lock_guard<std::mutex> lock(frameMutex);
	if (!latestFresh || latestFrame.empty()) return false;
	out = std::move(latestFrame);
	latestFrame.clear();
	latestFresh = false;
	return true;
}

void CapLong::startGrabThread()
{
	if (!isCapturing || isPaused || isFinish) return;
	if (grabThread.joinable()) return;
	// 把定时器节拍提到 1ms：Sleep(6) 在默认节拍下实际会睡 ~15.6ms，
	// 快滚时相邻帧位移会大一倍，重叠不够就拼不上。
	if (!timerResRaised) {
		timeBeginPeriod(1);
		timerResRaised = true;
	}
	grabRunning = true;
	const RECT sel{ capStartPos.x, capStartPos.y, capStartPos.x + imgW, capStartPos.y + imgH };
	grabThread = std::thread([this, sel]() { grabLoop(sel); });
}

void CapLong::stopGrabThread()
{
	grabRunning = false;
	if (grabThread.joinable()) grabThread.join();
	manualMagActive = false;
	if (timerResRaised) {
		timeEndPeriod(1);
		timerResRaised = false;
	}
	std::lock_guard<std::mutex> lock(frameMutex);
	latestFrame.clear();
	latestFresh = false;
}

void CapLong::grabLoop(const RECT& sel)
{
	MagnifierCapture mag;
	const bool magOk = capPipeline == 0
		&& mag.create(sel, { win->hwnd, tool ? tool->hwnd : nullptr }) && mag.hasFilter();
	manualMagActive = magOk;
	const size_t need = (size_t)imgW * imgH * 4;
	uint64_t lastHash = 0;
	bool haveLast = false;
	int idle = 0;
	while (grabRunning.load(std::memory_order_relaxed)) {
		std::vector<BYTE> data = magOk ? mag.capture(sel) : captureFrame();
		if (data.size() >= need) {
			if (data.size() > need) data.resize(need);
			const uint64_t h = frameSignature(data);
			if (!haveLast || h != lastHash) {
				haveLast = true;
				lastHash = h;
				publishFrame(std::move(data));
				idle = 0;
			}
			else {
				++idle;
			}
		}
		Sleep(idle > 20 ? captureLoopIdleMs : captureLoopBusyMs);
	}
	manualMagActive = false;
	mag.destroy();
}

void CapLong::startStitchThread()
{
	if (!isCapturing || isPaused || isFinish) return;
	if (stitchThread.joinable()) return;
	stitchRunning = true;
	stitchThread = std::thread([this]() { stitchLoop(); });
}

void CapLong::stopStitchThread()
{
	stitchRunning = false;
	if (stitchThread.joinable()) stitchThread.join();
}

void CapLong::stitchLoop()
{
	std::vector<BYTE> frame;
	while (stitchRunning.load(std::memory_order_relaxed)) {
		if (!takeFrame(frame)) {
			Sleep(2);
			continue;
		}
		if (frame.size() < (size_t)imgW * imgH * 4) continue;
		LongStitch::Stitched r;
		{
			std::lock_guard<std::mutex> lock(canvasMutex);
			if (!stitcher.ready()) continue;
			r = stitcher.push(frame.data(), imgW * 4);
			viewportTop = stitcher.viewportTop();
			if (r.grew) {
				resultH = stitcher.height();
				++canvasStamp;
				previewDirty = true;
			}
			if (r.tooLong) hitMaxHeight = true;
			if (r.accepted) lastShift = r.shift;
		}
		if (r.sawMotion) lastMotionMs = GetTickCount64();
		// 测速用：画布实际滚过的像素（落点位移），供自动滚动的速度闭环使用
		if (r.accepted) movedPx += std::abs(r.shift);
		// 连续几帧“画面在动、却拼不上”＝滚得太快了（位移超过可拼接范围），提示一下
		if (r.accepted) {
			fastRejects = 0;
		}
		else if (r.sawMotion) {
			if (++fastRejects >= kTooFastRejects) {
				fastRejects = 0;
				tooFastUntilMs = GetTickCount64() + kTooFastHintMs;
			}
		}
	}
}

void CapLong::flushTail()
{
	if (!isCapturing || isFinish) return;
	if (imgW <= 0 || imgH <= 0) return;
	{
		std::lock_guard<std::mutex> lock(canvasMutex);
		if (!stitcher.ready()) return;
	}
	// 页面可能还在惯性里：先停掉采集，等它停稳，再补抓一帧，
	// 用这帧清晰画面覆盖画布底部条带（快滚过程中的帧常带运动模糊）。
	stopGrabThread();
	stopStitchThread();
	Sleep(90);
	auto frame = captureFrame();
	if (!frame.empty()) {
		std::lock_guard<std::mutex> lock(canvasMutex);
		if (stitcher.refreshTail(frame.data(), imgW * 4)) {
			resultH = stitcher.height();
			viewportTop = stitcher.viewportTop();
			++canvasStamp;
			previewDirty = true;
			lastPreviewMs = 0;
		}
	}
	if (isCapturing && !isPaused && !isFinish) {
		startGrabThread();
		startStitchThread();
	}
}

void CapLong::setPaused(bool on)
{
	if (!isCapturing || isFinish || isPaused == on) return;
	if (on) {
		isPaused = true;
		stopAutoScroll();
		win->killTimer(pumpMsgId);
		flushTail();
		stopGrabThread();
		stopStitchThread();
		win->restoreWin();
		win->ensureNoMousePierce();
		raiseTool();
		if (!previewHot && !previewPanning && !selViewportPanning) {
			// 暂停后停在“当前视口”对应的位置：往上滚就靠顶，往下滚就靠底，而不是固定贴某一端
			inspectRatio = viewportRatio();
			previewFollowTop = inspectRatio <= 0.001f;
			previewFollowBottom = inspectRatio >= 0.999f;
			syncPreviewPanToRatio(inspectRatio);
		}
		makeImgPreview();
	}
	else {
		isPaused = false;
		previewHot = false;
		previewPanning = false;
		selViewportPanning = false;
		stopPreviewDragPanTimer();
		win->hollowWin();
		ensureExcludeFromCapture(true);
		raiseTool();
		startGrabThread();
		startStitchThread();
		win->setTimer(pumpTickMs, pumpMsgId);
		if (!manualMode) {
			if (!uia) initUiaScroll();
			startAutoScroll();
		}
	}
	syncToolBtn();
	win->refresh();
}

void CapLong::syncToolBtn()
{
	if (!tool) return;
	tool->syncCaptureState(isCapturing, isPaused, isFinish, !manualMode);
}

bool CapLong::makeImgPreview()
{
	if (!tool || imgW <= 0) return false;
	std::unique_lock<std::mutex> lock(canvasMutex, std::try_to_lock);
	if (!lock.owns_lock() || !stitcher.ready()) return false;
	// 抢到画布之后再丢旧位图：抢不到时保持旧预览，界面上不会闪一下空白
	imgPreview.Reset();
	const int canvasH = stitcher.height();
	const BYTE* src = stitcher.data();
	if (!src || canvasH <= 0) return false;
	resultH = canvasH;

	// 预览底图=原图；标注与选区共用 paintShapes，每帧叠上（即时）
	const float previewTargetW = (42.f * 5.f + ToolbarTheme::shadowPad * 2.f) * tool->dpi;
	float previewScaleW = previewTargetW / (float)imgW;
	// 又窄又长的选区：宽度缩小后高度仍可能超过纹理上限，按高度再缩一次
	previewScaleW = std::min(previewScaleW, (float)maxPreviewTexDim / (float)canvasH);
	if (!(previewScaleW > 0.f)) return false;
	const int previewW = std::max(1, (int)std::lround((float)imgW * previewScaleW));
	const int previewH = std::max(1, (int)std::lround((float)canvasH * previewScaleW));

	// 预计算行列映射：内层只做整数索引 + 4 字节拷贝。
	std::vector<int> colMap((size_t)previewW);
	std::vector<int> rowMap((size_t)previewH);
	for (int x = 0; x < previewW; ++x)
		colMap[(size_t)x] = std::min(imgW - 1, (int)((float)x / previewScaleW));
	for (int y = 0; y < previewH; ++y)
		rowMap[(size_t)y] = std::min(canvasH - 1, (int)((float)y / previewScaleW));

	std::vector<BYTE> scaledData((size_t)previewW * 4 * previewH);
	const size_t srcStride = (size_t)imgW * 4;
	for (int y = 0; y < previewH; ++y) {
		const BYTE* srcRow = src + (size_t)rowMap[(size_t)y] * srcStride;
		BYTE* dst = scaledData.data() + (size_t)y * previewW * 4;
		for (int x = 0; x < previewW; ++x) {
			const BYTE* p = srcRow + (size_t)colMap[(size_t)x] * 4;
			dst[(size_t)x * 4] = p[0];
			dst[(size_t)x * 4 + 1] = p[1];
			dst[(size_t)x * 4 + 2] = p[2];
			dst[(size_t)x * 4 + 3] = p[3];
		}
	}
	D2D1_BITMAP_PROPERTIES1 props = {
		.pixelFormat{D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)},
		.dpiX{96.0f}, .dpiY{96.0f}, .bitmapOptions{D2D1_BITMAP_OPTIONS_NONE}
	};
	Ling::D2D::get()->deviceContext->CreateBitmap(
		D2D1::SizeU(previewW, previewH), scaledData.data(), previewW * 4, props, imgPreview.GetAddressOf());
	lock.unlock();
	updatePreviewLayout();

	// 悬停/拖右侧预览时选区要显示对应长图；扫动惯性还在继续拼接时最多 4fps 刷新大图，
	// 避免大位图重建拖慢抓帧；暂停/结束/编辑时立即重建。
	const bool inspecting = previewHot || previewPanning || selViewportPanning;
	if (isPaused || isFinish || isEditMode()) {
		ensureSelPreviewBmp();
	}
	else if (inspecting) {
		const ULONGLONG now = GetTickCount64();
		if (now - lastFullPreviewMs >= 250) {
			ensureSelPreviewBmp();
			lastFullPreviewMs = now;
		}
	}
	return imgPreview != nullptr;
}

void CapLong::makeTool()
{
	if (tool) return;
	win->clearMainToolUserPlaced();
	tool = std::make_unique<ToolLong>(win);
	layoutTool();
	tool->createNativeWindow(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, WS_POPUP);
	SetWindowLongPtr(tool->hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(win->hwnd));
	layoutTool();
	SetWindowPos(tool->hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
	App::excludeFromCapture(tool->hwnd);
	syncToolBtn();
}

void CapLong::raiseTool()
{
	if (!tool || !tool->hwnd) return;
	SetWindowPos(tool->hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void CapLong::setToolVisible(bool visible)
{
	// 和工具栏同进退：拖边 / 拖移选区期间工具条隐藏，右上角的文字提示也一起隐藏
	if (toolChromeHidden != !visible) {
		toolChromeHidden = !visible;
		win->refresh();
	}
	if (!tool || !tool->hwnd) return;
	if (visible) {
		layoutTool();
		tool->show();
		raiseTool();
	}
	else {
		tool->hide();
	}
}

void CapLong::layoutTool()
{
	if (!tool) return;
	if (win->isMainToolUserPlaced()) return;
	const int toolW = (int)(tool->w + 0.5f);
	const int toolH = (int)(tool->h + 0.5f);
	const int pad = (int)(ToolbarTheme::shadowPad * tool->dpi + 0.5f);
	const int visualH = (std::max)(1, toolH - pad * 2);

	const int maskLeftScr = win->x + (int)win->cutMask->maskRect.left;
	const int maskTopScr = win->y + (int)win->cutMask->maskRect.top;
	const int maskRightScr = win->x + (int)win->cutMask->maskRect.right;
	const int maskBottomScr = win->y + (int)win->cutMask->maskRect.bottom;

	RECT maskScrRect{ maskLeftScr, maskTopScr, maskRightScr, maskBottomScr };
	HMONITOR hMon = MonitorFromRect(&maskScrRect, MONITOR_DEFAULTTONEAREST);
	MONITORINFO mi{ sizeof(MONITORINFO) };
	GetMonitorInfo(hMon, &mi);

	const int gap = (int)(win->cutMask->strokeWidth + 2.f * tool->dpi + 0.5f);
	const bool fitBelow = (maskBottomScr + gap + visualH) <= mi.rcWork.bottom;
	const bool fitAbove = (maskTopScr - gap - visualH) >= mi.rcWork.top;

	// 与贴图一致：相对截图视窗右对齐
	int toolX = maskRightScr - toolW + pad;
	int toolY = 0;
	if (fitBelow) {
		toolY = maskBottomScr + gap - pad;
	}
	else if (fitAbove) {
		toolY = maskTopScr - gap - toolH + pad;
	}
	else {
		const int overlapPad = (int)(3.f * tool->dpi + 0.5f);
		toolX = maskRightScr - toolW + pad - overlapPad;
		toolY = maskBottomScr - toolH + pad - overlapPad;
	}

	if (toolX < mi.rcWork.left) toolX = mi.rcWork.left;
	if (toolX + toolW > mi.rcWork.right) toolX = mi.rcWork.right - toolW;
	if (toolY < mi.rcWork.top) toolY = mi.rcWork.top;
	if (toolY + toolH > mi.rcWork.bottom) toolY = mi.rcWork.bottom - toolH;
	if (tool->x == toolX && tool->y == toolY) return;
	tool->setPosition(toolX, toolY);
}

bool CapLong::isEditMode() const
{
	return tool && tool->isEditMode();
}

bool CapLong::isPreviewPanning() const
{
	return previewPanning;
}

void CapLong::setEditMode(bool on)
{
	if (!tool) return;
	if (on) {
		if (isCapturing && !isPaused && !isFinish) setPaused(true);
		win->restoreWin();
		win->ensureNoMousePierce();
		raiseTool();
		if (resultH > 0) {
			ensureSelPreviewBmp();
			makeImgPreview();
		}
	}
	else {
		tool->cancelAnnotSelect();
	}
	tool->setEditMode(on);
	raiseTool();
	win->refresh();
}

void CapLong::cancelAnnotSelect()
{
	if (tool) tool->cancelAnnotSelect();
}

void CapLong::notifyHistoryChanged()
{
	if (tool) tool->updateUndoEnabled();
}

HWND CapLong::toolHwnd() const
{
	return tool ? tool->hwnd : nullptr;
}

bool CapLong::queryBarAnchor(float& x, float& y, float& w, float& h, float& btnCenterX, float& dpi) const
{
	if (!tool) return false;
	x = (float)tool->x;
	y = (float)tool->y;
	w = tool->w;
	h = tool->h;
	btnCenterX = tool->getBtnCenterX();
	dpi = tool->dpi;
	return true;
}

void CapLong::stopCap()
{
	if (isFinish) return;
	win->killTimer(autoStepMsgId);
	win->killTimer(pumpMsgId);
	win->killTimer(previewDragPanMsgId);
	removeMouseHook();
	// 收尾前补抓一帧停稳画面刷一下底部条带：快滚到一半就结束时，最后一段常带运动模糊
	flushTail();
	stopGrabThread();
	stopStitchThread();
	releaseUiaScroll();
	isFinish = true;
	isCapturing = false;
	isPaused = false;
	makeStopText();
	ensureExcludeFromCapture(false);
	win->restoreWin();
	win->ensureNoMousePierce();
	raiseTool();
	previewFollowBottom = false;
	previewFollowTop = false;
	previewHot = false;
	previewPanning = false;
	previewPanY = 0.f;
	inspectRatio = 0.f; // 结束后从顶部浏览；遮罩仍钉在底部当前截图
	makeImgPreview();
	syncToolBtn();
	win->refresh();
}

void CapLong::makeStopText()
{
	if (hitMaxHeight) {
		layoutTextEnd = Ling::D2D::get()->makeTextLayout(Lang::get(L"long.tooLong"), 13 * win->dpi);
	}
	else if (stoppedAtEnd) {
		layoutTextEnd = Ling::D2D::get()->makeTextLayout(Lang::get(L"long.reachedBottom"), 13 * win->dpi);
	}
	else {
		layoutTextEnd = Ling::D2D::get()->makeTextLayout(Lang::get(L"long.stitchStopped"), 13 * win->dpi);
	}
	if (!layoutTextEnd) return;
	DWRITE_TEXT_METRICS tm = {};
	layoutTextEnd->GetMetrics(&tm);
	auto& maskRect = win->cutMask->maskRect;
	auto halfX = maskRect.left + (maskRect.right - maskRect.left) / 2;
	auto halfW = tm.width / 2;
	float padding{ 8 * win->dpi };
	stopTextRect.left = halfX - halfW - padding;
	// 选区很矮时不要把提示框顶到选区外（倒置的 D2D 矩形会画不出来）
	const float bottom = maskRect.bottom - padding;
	const float top = std::max(maskRect.top, bottom - 30 * win->dpi);
	stopTextRect.top = top;
	stopTextRect.right = halfX + halfW + padding;
	stopTextRect.bottom = std::max(top + 1.f, bottom);
	layoutTextEnd->SetMaxWidth(stopTextRect.right - stopTextRect.left);
	layoutTextEnd->SetMaxHeight(stopTextRect.bottom - stopTextRect.top);
	stopTextPos = { halfX - halfW, stopTextRect.top + (stopTextRect.bottom - stopTextRect.top - tm.height) / 2 };
}

void CapLong::copyToClipboard()
{
	std::vector<BYTE> pixels;
	if (!exportPixels(pixels)) return;
	// 与区域截图收工一致：按「截图完成执行」四档决定存不存文件、剪贴板放位图还是文件
	auto* st = Setting::get();
	if (!st) return;
	const int action = st->getCaptureFinishAction();
	const std::wstring saved = (action != 3)
		? Util::saveCompletedCapture(pixels, imgW, resultH, win ? win->focusAppName() : L"")
		: std::wstring{};
	if (!saved.empty() && action == 1) Util::addFileToClipboard(saved);
	else if (!saved.empty() && action == 2) Util::addFileToClipboardCut(saved);
	else Util::saveToClipboard(imgW, resultH, pixels.data());
	if (!saved.empty()) st->addHistoryFile(saved, imgW, resultH, L"long");
}

bool CapLong::saveToFile()
{
	std::vector<BYTE> pixels;
	if (!exportPixels(pixels)) return false;
	auto* st = Setting::get();
	if (!st) return false;
	// 与截图/贴图的手动保存一致：默认名取「手动保存文件名格式」的末段，起始目录 = 桌面
	std::wstring defName = Util::previewFileName(
		st->getManualSaveFormat(), Setting::defaultManualSaveFormat(), L"");
	if (auto slash = defName.find_last_of(L'/'); slash != std::wstring::npos) defName = defName.substr(slash + 1);
	auto path = Util::getSaveFilePath(win->hwnd,
		Util::extensionFromSaveFormat(st->getSaveFormat()), defName, Util::desktopDir().wstring());
	if (path.empty()) return false;
	if (!Util::saveToFile(path, imgW, resultH, pixels.data())) return false;
	st->addHistoryFile(path, imgW, resultH, L"long");
	return true;
}

void CapLong::pin()
{
	std::vector<BYTE> pixels;
	if (!exportPixels(pixels)) return;
	if (!win->cutMask) return;
	// 与普通截图「贴图」一致：落在选区所在的那块屏，而不是固定主屏
	const auto& r = win->cutMask->maskRect;
	const RECT selScr{
		win->x + (LONG)r.left, win->y + (LONG)r.top,
		win->x + (LONG)r.right, win->y + (LONG)r.bottom
	};
	MONITORINFO mi{ sizeof(MONITORINFO) };
	if (!GetMonitorInfo(MonitorFromRect(&selScr, MONITOR_DEFAULTTONEAREST), &mi))
		GetMonitorInfo(MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
	auto& workArea = mi.rcWork;
	const int screenW = workArea.right - workArea.left;
	const int screenH = workArea.bottom - workArea.top;
	const int posX = imgW >= screenW ? workArea.left : workArea.left + (screenW - imgW) / 2;
	const int posY = resultH >= screenH ? workArea.top : workArea.top + (screenH - resultH) / 2;
	WinPin::initFromData(posX, posY, imgW, resultH, pixels);
}
