#include "pch.h"
#include "CaptureShield.h"
#include "../App.h"

namespace
{
	constexpr const wchar_t* kShieldClass{ L"SnowAirCaptureShield" };
	constexpr UINT_PTR kTimeoutTimerId{ 0x5A17 };
	constexpr UINT kMsgDrain{ WM_APP + 0x421 };
	// 兜底：万一覆盖窗没建起来（抓屏失败、窗口创建失败），别让一层看不见的窗口一直吃鼠标
	constexpr UINT kTimeoutMs{ 2000 };

	HWND g_shield{ nullptr };
	HWND g_pendingTarget{ nullptr };   // drain 时鼠标还按着：等抬手再补交 + 撤盾牌
	bool g_down{ false };   // 空窗期按下过
	bool g_left{ false };   // 且是左键
	bool g_up{ false };     // 已经松手
	POINT g_anchor{};       // 屏幕坐标：按下点
	POINT g_last{};         // 屏幕坐标：最后一次已知位置

	void destroyShield()
	{
		g_pendingTarget = nullptr;
		if (!g_shield) return;
		KillTimer(g_shield, kTimeoutTimerId);
		DestroyWindow(g_shield);
		g_shield = nullptr;
	}

	// 物理上还有鼠标键按着（托盘/菜单那一记点击：按下不是我们收的，但抬手会落到我们身上）
	bool anyButtonHeld()
	{
		return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0
			|| (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0
			|| (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
	}

	void replayInto(HWND target)
	{
		if (g_down && g_left && target) {
			POINT a = g_anchor;
			POINT e = g_last;
			ScreenToClient(target, &a);
			ScreenToClient(target, &e);
			// 走真实输入同一条路：Ling 的 WndProc 用 lParam 取客户区坐标，所以 Post 合成消息即可
			PostMessageW(target, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(a.x, a.y));
			PostMessageW(target, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(e.x, e.y));
			if (g_up) {
				PostMessageW(target, WM_LBUTTONUP, 0, MAKELPARAM(e.x, e.y));
			}
			else {
				// 还按着：把捕获也交给覆盖窗，后续真实移动/抬起才不会被工具条、别的窗口截走
				SetCapture(target);
			}
		}
		g_down = g_left = g_up = false;
	}

	void finishDrain(HWND target)
	{
		replayInto(target);
		destroyShield();
	}

	LRESULT CALLBACK shieldProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
	{
		switch (msg)
		{
		case WM_MOUSEACTIVATE:
			return MA_NOACTIVATE;   // 只收鼠标，不抢焦点
		case WM_ERASEBKGND: {
			// 必须把客户区填成不透明：LWA_ALPHA 下逐像素 alpha 为 0 的地方对鼠标是穿透的，
			// 填实（黑）+ 常量 alpha=1 → 既看不见、又一定能收到鼠标
			HDC dc = reinterpret_cast<HDC>(wp);
			RECT rc{};
			GetClientRect(hwnd, &rc);
			FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
			return 1;
		}
		case WM_LBUTTONDOWN:
		case WM_RBUTTONDOWN:
		case WM_MBUTTONDOWN: {
			GetCursorPos(&g_anchor);
			g_last = g_anchor;
			if (!g_down) {
				g_down = true;
				g_left = (msg == WM_LBUTTONDOWN);
				g_up = false;
			}
			return 0;
		}
		case WM_MOUSEMOVE:
			GetCursorPos(&g_last);
			return 0;
		case WM_LBUTTONUP:
		case WM_RBUTTONUP:
		case WM_MBUTTONUP:
			GetCursorPos(&g_last);
			g_up = true;
			// drain 时还按着（按下不是我们收的）：抬手这一下也吃掉，然后才补交、撤盾牌
			if (g_pendingTarget) {
				finishDrain(g_pendingTarget);
				return 0;
			}
			return 0;
		case WM_TIMER:
			if (wp == kTimeoutTimerId) { destroyShield(); return 0; }
			break;
		case kMsgDrain:
			// 盾牌排在自己收到的鼠标消息之后，所以这里补交时按下/移动都已经记好了
			replayInto(reinterpret_cast<HWND>(wp));
			destroyShield();
			return 0;
		case WM_DESTROY:
			g_shield = nullptr;
			return 0;
		default:
			break;
		}
		return DefWindowProcW(hwnd, msg, wp, lp);
	}
}

namespace CaptureShield
{
	void arm()
	{
		// 已经有一层盾牌：只重置这一次的手势，别叠第二层
		if (g_shield) {
			g_down = g_left = g_up = false;
			return;
		}
		if (!App::get()) return;
		auto [sx, sy, sw, sh] = App::get()->getScreenArea();
		if (sw <= 0 || sh <= 0) return;

		static bool registered = false;
		if (!registered) {
			WNDCLASSEXW wc{ sizeof(wc) };
			wc.lpfnWndProc = shieldProc;
			wc.hInstance = GetModuleHandleW(nullptr);
			wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
			wc.lpszClassName = kShieldClass;
			registered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
			if (!registered) return;
		}
		// WS_EX_NOACTIVATE：不夺前台（浏览器保持前台，我们只把鼠标接过来）
		g_shield = CreateWindowExW(
			WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
			kShieldClass, L"", WS_POPUP, sx, sy, sw, sh,
			nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
		if (!g_shield) return;
		// alpha=1/255：肉眼看不见，但 alpha!=0 → 能收鼠标（alpha==0 才会穿透到下面的窗口）
		SetLayeredWindowAttributes(g_shield, 0, 1, LWA_ALPHA);
		// 抓屏时它等于不存在（老系统上这个调用由实现自己拦掉，那就只差 1/255 的一点点压暗）
		App::excludeFromCapture(g_shield);
		g_down = g_left = g_up = false;
		ShowWindow(g_shield, SW_SHOWNOACTIVATE);
		// 立刻把窗口填实：UI 线程紧接着就要同步抓屏（几十~几百毫秒），不能让"填背景"排在
		// 用户那次按下后面 —— 表面还没填过的话逐像素 alpha 可能是 0，那一下就会被穿透出去
		UpdateWindow(g_shield);
		SetWindowPos(g_shield, HWND_TOPMOST, sx, sy, sw, sh, SWP_NOACTIVATE | SWP_SHOWWINDOW);
		SetTimer(g_shield, kTimeoutTimerId, kTimeoutMs, nullptr);
	}

	void drain(HWND target)
	{
		if (!g_shield) return;
		// 托盘菜单项是在按下时就执行命令的（Ling::Button::onDown），所以覆盖窗 show() 出来时
		// 用户那一下点击往往还按着。这时候撤盾牌，抬手就会落到覆盖窗上，变成一个"没有按下的
		// 抬起"（旧代码会当成框选完成 → 录屏会话直接冲进录制）。等它抬起来再撤，顺手吃掉这一下。
		if (!g_down && anyButtonHeld()) {
			g_pendingTarget = target;
			return;
		}
		// 按队列顺序：盾牌先处理完空窗期收到的鼠标消息，再处理这条补交，然后销毁自己
		PostMessageW(g_shield, kMsgDrain, reinterpret_cast<WPARAM>(target), 0);
	}
}
