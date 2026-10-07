#include "pch.h"
#include "HotkeyHook.h"

namespace
{
	constexpr const wchar_t* kHookClass{ L"SnowAirHotkeyHook" };
	constexpr UINT kMsgFire{ WM_APP + 0x422 };

	HWND g_wnd{ nullptr };            // UI 线程上的隐藏窗口：钩子线程 Post 到这里，回调在 UI 线程跑
	HANDLE g_thread{ nullptr };
	DWORD  g_tid{ 0 };
	HANDLE g_ready{ nullptr };
	// 下面这些只被钩子线程读写（install/uninstall 在起停前后设置，天然有先后）
	HHOOK g_hook{ nullptr };
	bool g_hookOk{ false };
	UINT g_mods{ 0 };
	UINT g_key{ 0 };
	UINT g_liveMods{ 0 };             // 当前物理按下的修饰键
	bool g_keyDown{ false };          // 主键已按下：长按只触发一次
	bool g_swallowUp{ false };        // 这一次抬起也要吃掉
	std::function<void()> g_onFire;

	// 与 Ling::App::regHotKey 同一套键名解析口径
	bool parseChord(const std::wstring& s, UINT& mods, UINT& key)
	{
		mods = 0;
		key = 0;
		std::wstring lower = s;
		std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
		for (auto& part : Ling::Util::splitStr(lower, L'+')) {
			if (part == L"ctrl") mods |= MOD_CONTROL;
			else if (part == L"alt") mods |= MOD_ALT;
			else if (part == L"shift") mods |= MOD_SHIFT;
			else if (part == L"win" || part == L"lwin" || part == L"rwin") mods |= MOD_WIN;
			else key = Ling::Util::strToKey(part);
		}
		return mods != 0 && key != 0;
	}

	UINT modBit(DWORD vk)
	{
		switch (vk) {
		case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return MOD_CONTROL;
		case VK_MENU: case VK_LMENU: case VK_RMENU: return MOD_ALT;
		case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return MOD_SHIFT;
		case VK_LWIN: case VK_RWIN: return MOD_WIN;
		default: return 0;
		}
	}

	LRESULT CALLBACK hookProc(int code, WPARAM wp, LPARAM lp)
	{
		if (code != HC_ACTION) return CallNextHookEx(nullptr, code, wp, lp);
		auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
		// 自己合成的按键不参与匹配（免得绕回来）
		if (kb->flags & LLKHF_INJECTED) return CallNextHookEx(nullptr, code, wp, lp);
		const bool up = (kb->flags & LLKHF_UP) != 0;
		if (const UINT bit = modBit(kb->vkCode)) {
			if (up) g_liveMods &= ~bit;
			else g_liveMods |= bit;
			return CallNextHookEx(nullptr, code, wp, lp);
		}
		if (kb->vkCode == g_key && (g_liveMods & g_mods) == g_mods) {
			if (!up) {
				if (!g_keyDown) {
					g_keyDown = true;
					g_swallowUp = true;
					if (g_wnd) PostMessageW(g_wnd, kMsgFire, 0, 0);
				}
				return 1;   // 吃掉：别的程序（含浏览器）看不到这个组合
			}
			g_keyDown = false;
			if (g_swallowUp) {
				g_swallowUp = false;
				return 1;
			}
		}
		return CallNextHookEx(nullptr, code, wp, lp);
	}

	DWORD WINAPI hookThread(LPVOID)
	{
		// 先把消息队列建出来，PostThreadMessage(WM_QUIT) 才不会失败
		MSG msg{};
		PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
		g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, hookProc, GetModuleHandleW(nullptr), 0);
		g_hookOk = g_hook != nullptr;
		if (g_ready) SetEvent(g_ready);
		if (!g_hook) return 0;
		while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		UnhookWindowsHookEx(g_hook);
		g_hook = nullptr;
		return 0;
	}

	LRESULT CALLBACK hookWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
	{
		if (msg == kMsgFire) {
			if (g_onFire) g_onFire();
			return 0;
		}
		return DefWindowProcW(hwnd, msg, wp, lp);
	}

	bool ensureWindow()
	{
		if (g_wnd) return true;
		static bool registered = false;
		if (!registered) {
			WNDCLASSEXW wc{ sizeof(wc) };
			wc.lpfnWndProc = hookWndProc;
			wc.hInstance = GetModuleHandleW(nullptr);
			wc.lpszClassName = kHookClass;
			registered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
			if (!registered) return false;
		}
		g_wnd = CreateWindowExW(0, kHookClass, L"", 0, 0, 0, 0, 0,
			HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
		return g_wnd != nullptr;
	}
}

namespace HotkeyHook
{
	void install(const std::wstring& chord, std::function<void()> onFire)
	{
		uninstall();
		if (!parseChord(chord, g_mods, g_key)) return;
		if (!ensureWindow()) return;
		g_liveMods = 0;
		g_keyDown = false;
		g_swallowUp = false;
		g_onFire = std::move(onFire);
		g_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		g_thread = CreateThread(nullptr, 0, hookThread, nullptr, 0, &g_tid);
		if (!g_thread) {
			if (g_ready) { CloseHandle(g_ready); g_ready = nullptr; }
			g_onFire = nullptr;
			return;
		}
		if (g_ready) {
			WaitForSingleObject(g_ready, 500);
			CloseHandle(g_ready);
			g_ready = nullptr;
		}
		// 钩子没装上：别留一个"看着装了其实没用"的状态（重试定时器那边还在转，会继续试 RegisterHotKey）
		if (!g_hookOk) uninstall();
	}

	void uninstall()
	{
		if (g_thread) {
			if (g_tid) PostThreadMessageW(g_tid, WM_QUIT, 0, 0);
			WaitForSingleObject(g_thread, 1500);
			CloseHandle(g_thread);
			g_thread = nullptr;
			g_tid = 0;
		}
		g_onFire = nullptr;
		if (g_wnd) {
			DestroyWindow(g_wnd);
			g_wnd = nullptr;
		}
	}

	bool installed()
	{
		return g_thread != nullptr;
	}
}
