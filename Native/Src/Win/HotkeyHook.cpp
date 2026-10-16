#include "pch.h"
#include "HotkeyHook.h"
#include <atomic>

namespace
{
	constexpr const wchar_t* kHookClass{ L"SnowAirHotkeyHook" };
	constexpr UINT kMsgFire{ WM_APP + 0x422 };

	// 钩子线程独占的可变状态（mods/key/按键边沿）。回调不放这里 —— 回调由 UI 线程持有，
	// 避免钩子线程与 UI 线程同时碰同一个对象。
	struct HookBinding
	{
		UINT mods{ 0 };
		UINT key{ 0 };
		bool keyDown{ false };    // 主键已按下：长按只触发一次
		bool swallowUp{ false };  // 这一次抬起也要吃掉
	};

	HWND g_wnd{ nullptr };            // UI 线程上的隐藏窗口：钩子线程 Post 到这里，回调在 UI 线程跑
	HANDLE g_thread{ nullptr };
	DWORD  g_tid{ 0 };
	HANDLE g_ready{ nullptr };
	// 下面这些只被钩子线程读写（setBindings/uninstall 在起停前后设置，天然有先后）
	HHOOK g_hook{ nullptr };
	bool g_hookOk{ false };
	UINT g_liveMods{ 0 };             // 当前物理按下的修饰键
	std::vector<HookBinding> g_bindings;
	// 回调表：只在 UI 线程读（hookWndProc），setBindings 里一次性整体替换
	std::vector<std::function<void()>> g_callbacks;
	// 绑定换代号：换代前 Post 出去、换代后才到 UI 线程的旧 kMsgFire 一律丢弃
	std::atomic<unsigned> g_generation{ 0 };

	// 与 Ling::App::regHotKey 同一套键名解析口径，且**不要求带修饰键**：
	// 只挡 keyCode == 0。F1、` 等裸键（三条临时全局键即为此形态）必须能被装进钩子，
	// 否则会话绑定整条失效。
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
		return key != 0;
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
		const unsigned gen = g_generation.load(std::memory_order_relaxed);
		bool swallowed = false;
		for (size_t i = 0; i < g_bindings.size(); ++i) {
			auto& b = g_bindings[i];
			if (b.key != kb->vkCode) continue;
			if (!up) {
				// 按下：要求修饰键齐备
				if ((g_liveMods & b.mods) != b.mods) continue;
				if (!b.keyDown) {
					b.keyDown = true;
					b.swallowUp = true;
					if (g_wnd) PostMessageW(g_wnd, kMsgFire, (WPARAM)i, (LPARAM)gen);
				}
				swallowed = true;
			}
			else {
				// 抬起：只按主键匹配 —— 用户先松修饰键再松主键时，此刻 g_liveMods 已经变了，
				// 再拿组合去匹配会匹配不到，于是"该吞的抬起"漏出去
				if (!b.keyDown) continue;
				b.keyDown = false;
				if (b.swallowUp) {
					b.swallowUp = false;
					swallowed = true;
				}
			}
			break;
		}
		return swallowed ? 1 : CallNextHookEx(nullptr, code, wp, lp);
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
			// 换代前的旧消息直接丢；否则按索引回调（索引在 setBindings 时与回调表一起换）
			if ((unsigned)lp == g_generation.load(std::memory_order_relaxed)) {
				const size_t idx = (size_t)wp;
				// ⚠ 必须先把闭包**拷贝**出来再调用：回调内部可能触发钩子重装（例如「开始/暂停录制」
				// 经 startRecord → syncGlobalCommandKeys → setSessionBindings），那一步会整体替换
				// g_callbacks —— 若直接写 g_callbacks[idx]()，等于在执行期间析构该闭包自身
				//（其捕获的 wstring 随之释放），构成 use-after-free。
				std::function<void()> cb;
				if (idx < g_callbacks.size()) cb = g_callbacks[idx];
				if (cb) cb();
			}
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
	// 主绑定（全局热键那侧给的）与会话临时绑定：只在 UI 线程读写。
	// uninstall() 只清"已装进钩子的"那份，这两份列表要留着，好在下一次 installMerged() 里合并。
	std::vector<HotkeyHook::Binding> g_main;
	std::vector<HotkeyHook::Binding> g_session;

	void installMerged()
	{
		HotkeyHook::uninstall();
		std::vector<HookBinding> parsed;
		std::vector<std::function<void()>> callbacks;
		auto add = [&](const std::vector<HotkeyHook::Binding>& src) {
			for (const auto& b : src) {
				HookBinding hb;
				if (!parseChord(b.chord, hb.mods, hb.key)) continue;
				parsed.push_back(hb);
				callbacks.push_back(b.onFire);
			}
		};
		// 会话绑定在前：录屏时的 F1 必须先归"开始/暂停录制"，不能被全局热键的"区域截图"截走
		add(g_session);
		add(g_main);
		if (parsed.empty()) return;
		if (!ensureWindow()) return;

		g_liveMods = 0;
		g_bindings = std::move(parsed);
		g_callbacks = std::move(callbacks);
		g_generation.fetch_add(1, std::memory_order_relaxed);

		g_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		g_thread = CreateThread(nullptr, 0, hookThread, nullptr, 0, &g_tid);
		if (!g_thread) {
			if (g_ready) { CloseHandle(g_ready); g_ready = nullptr; }
			g_bindings.clear();
			g_callbacks.clear();
			return;
		}
		if (g_ready) {
			WaitForSingleObject(g_ready, 500);
			CloseHandle(g_ready);
			g_ready = nullptr;
		}
		// 钩子未安装成功：不留"看似已安装但无效"的状态（重试定时器仍在运行，会继续尝试 RegisterHotKey）
		if (!g_hookOk) HotkeyHook::uninstall();
	}
}

namespace HotkeyHook
{
	void setBindings(std::vector<Binding> bindings)
	{
		g_main = std::move(bindings);
		installMerged();
	}

	void setSessionBindings(std::vector<Binding> bindings)
	{
		g_session = std::move(bindings);
		installMerged();
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
		// 换代：把还压在消息队列里的旧 kMsgFire 全部作废
		g_generation.fetch_add(1, std::memory_order_relaxed);
		g_bindings.clear();
		g_callbacks.clear();
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
