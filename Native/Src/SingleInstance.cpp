#include "pch.h"
#include <tlhelp32.h>
#include <winver.h>
#include <filesystem>
#include <vector>
#include "SingleInstance.h"
#include "Lang.h"
#include "Setting.h"
#include "Win/WinSetting.h"
#pragma comment(lib, "version.lib")

namespace {
	// 实例之间约定的消息（我们自己的隐藏窗口收）
	constexpr UINT kOpenSettingsMsg = WM_APP + 11; // 叫已有实例打开设置中心
	constexpr UINT kQuitMsg = WM_APP + 12;         // 请已有实例优雅退出
	constexpr UINT kYieldHotkeysMsg = WM_APP + 13; // 请把全局热键交出来
	constexpr UINT kResumeHotkeysMsg = WM_APP + 14; // 对方退出，可以取回全局热键

	constexpr wchar_t kInstanceClass[] = L"SnowAirInstance";
	HWND g_instanceWnd{ nullptr };

	// 代号 → 互斥体名 / 实例窗口标题 / AUMID；两个代号互相独立，可以并存
	std::wstring editionKey(bool lite) { return lite ? L"SnowAir.Lite" : L"SnowAir"; }

	// 这些消息由主循环派发，跑在 UI 线程上
	LRESULT CALLBACK instanceWndProc(HWND wnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		switch (msg) {
		case kOpenSettingsMsg:
			// 同类实例的第二次启动：弹出设置中心
			WinSetting::init();
			return 0;
		case kQuitMsg:
			// 同类别的更新实例要求让位：优雅退出
			if (auto* app = Ling::App::get()) app->quit(0);
			return 0;
		case kYieldHotkeysMsg:
			// 完整版起来了：交出全局热键，避免一次按键触发两遍
			if (auto* setting = Setting::get()) setting->setHotkeysYielded(true);
			return 0;
		case kResumeHotkeysMsg:
			// 完整版退出了：把全局热键取回来
			if (auto* setting = Setting::get()) setting->setHotkeysYielded(false);
			return 0;
		case WM_CLOSE:
			DestroyWindow(wnd);
			return 0;
		case WM_DESTROY:
			if (g_instanceWnd == wnd) g_instanceWnd = nullptr;
			return 0;
		default:
			break;
		}
		return DefWindowProcW(wnd, msg, wParam, lParam);
	}

	bool ensureInstanceWindow(const std::wstring& title)
	{
		if (g_instanceWnd && IsWindow(g_instanceWnd)) return true;
		WNDCLASSEXW wc{ sizeof(WNDCLASSEXW) };
		wc.lpfnWndProc = instanceWndProc;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.lpszClassName = kInstanceClass;
		// 已经注册过就会失败，忽略即可
		RegisterClassExW(&wc);
		// 顶层而非消息专用窗口：否则启动时系统会一直显示"忙"光标
		g_instanceWnd = CreateWindowExW(WS_EX_TOOLWINDOW, kInstanceClass, title.c_str(),
			WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
		return g_instanceWnd != nullptr;
	}

	struct Sibling
	{
		DWORD pid{ 0 };
		std::filesystem::path exe;
		ULONGLONG version{ 0 };   // major<<48 | minor<<32 | build<<16 | rev
		bool lite{ false };       // 文件名带 lite = Lite 版
		HWND msgWnd{ nullptr };   // 它的实例窗口
		bool busy{ false };       // 屏幕上有没有像样的界面（截图遮罩/设置窗等）
	};

	std::filesystem::path modulePath()
	{
		wchar_t buf[MAX_PATH]{};
		GetModuleFileNameW(nullptr, buf, MAX_PATH);
		return std::filesystem::path{ buf };
	}

	// 进程名小写；兄弟按前缀 snowair 认，老版本可能叫 SnowAir-Lite-v0.1.25.exe
	std::wstring exeNameLower(const std::filesystem::path& exe)
	{
		std::wstring name = exe.filename().wstring();
		for (auto& c : name) c = (wchar_t)towlower(c);
		return name;
	}

	bool isSnowAirName(const std::wstring& lowerName)
	{
		const std::wstring suffix = L".exe";
		return lowerName.size() > suffix.size()
			&& lowerName.compare(0, 7, L"snowair") == 0
			&& lowerName.compare(lowerName.size() - suffix.size(), suffix.size(), suffix) == 0;
	}

	// 文件名带 lite 的是 Lite 版
	bool isLiteName(const std::wstring& lowerName)
	{
		return lowerName.find(L"lite") != std::wstring::npos;
	}

	ULONGLONG versionOf(const std::filesystem::path& exe)
	{
		DWORD dummy = 0;
		const DWORD size = GetFileVersionInfoSizeW(exe.c_str(), &dummy);
		if (size == 0) return 0;
		std::vector<BYTE> buf(size);
		if (!GetFileVersionInfoW(exe.c_str(), 0, size, buf.data())) return 0;
		VS_FIXEDFILEINFO* info = nullptr;
		UINT len = 0;
		if (!VerQueryValueW(buf.data(), L"\\", (void**)&info, &len) || !info || len < sizeof(VS_FIXEDFILEINFO))
			return 0;
		const ULONGLONG ms = info->dwFileVersionMS;
		const ULONGLONG ls = info->dwFileVersionLS;
		return ((ms >> 16) << 48) | ((ms & 0xFFFF) << 32) | ((ls >> 16) << 16) | (ls & 0xFFFF);
	}

	BOOL CALLBACK collectWindowsProc(HWND wnd, LPARAM param)
	{
		reinterpret_cast<std::vector<HWND>*>(param)->push_back(wnd);
		return TRUE;
	}

	std::vector<HWND> topLevelWindows(DWORD pid)
	{
		std::vector<HWND> all;
		EnumWindows(collectWindowsProc, reinterpret_cast<LPARAM>(&all));
		std::vector<HWND> mine;
		for (HWND w : all) {
			DWORD owner = 0;
			GetWindowThreadProcessId(w, &owner);
			if (owner == pid) mine.push_back(w);
		}
		return mine;
	}

	std::vector<DWORD> siblingProcessIds()
	{
		std::vector<DWORD> out;
		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snap == INVALID_HANDLE_VALUE) return out;
		PROCESSENTRY32W pe{ sizeof(PROCESSENTRY32W) };
		const DWORD self = GetCurrentProcessId();
		if (Process32FirstW(snap, &pe)) {
			do {
				std::wstring name = pe.szExeFile;
				for (auto& c : name) c = (wchar_t)towlower(c);
				if (isSnowAirName(name) && pe.th32ProcessID != self) out.push_back(pe.th32ProcessID);
			} while (Process32NextW(snap, &pe));
		}
		CloseHandle(snap);
		return out;
	}

	bool describeProcess(DWORD pid, Sibling& out)
	{
		HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		if (!proc) return false;
		wchar_t path[MAX_PATH]{};
		DWORD size = MAX_PATH;
		const bool ok = QueryFullProcessImageNameW(proc, 0, path, &size) != FALSE;
		CloseHandle(proc);
		if (!ok) return false;
		std::filesystem::path exe{ std::wstring(path, size) };
		const std::wstring name = exeNameLower(exe);
		if (!isSnowAirName(name)) return false;
		out.pid = pid;
		out.exe = exe;
		out.lite = isLiteName(name);
		out.version = versionOf(exe);
		for (HWND w : topLevelWindows(pid)) {
			if (!out.msgWnd) {
				wchar_t cls[64]{};
				if (GetClassNameW(w, cls, 64) > 0 && _wcsicmp(cls, kInstanceClass) == 0) out.msgWnd = w;
			}
			// 只把屏幕上一大块窗口（截图遮罩/设置窗）算作正忙，晾衣架条与隐藏消息窗不算
			if (!IsWindowVisible(w)) continue;
			RECT rc{};
			if (!GetWindowRect(w, &rc)) continue;
			const double area = (double)(rc.right - rc.left) * (double)(rc.bottom - rc.top);
			const double screen = (double)GetSystemMetrics(SM_CXVIRTUALSCREEN)
				* (double)GetSystemMetrics(SM_CYVIRTUALSCREEN);
			if (screen > 0.0 && area / screen >= 0.15) out.busy = true;
		}
		return true;
	}

	bool waitExit(DWORD pid, DWORD timeoutMs)
	{
		HANDLE proc = OpenProcess(SYNCHRONIZE, FALSE, pid);
		if (!proc) return true; // 打不开了就按已退出处理
		const bool exited = WaitForSingleObject(proc, timeoutMs) == WAIT_OBJECT_0;
		CloseHandle(proc);
		return exited;
	}

	void terminate(DWORD pid)
	{
		HANDLE proc = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
		if (!proc) return;
		TerminateProcess(proc, 0);
		WaitForSingleObject(proc, 2000);
		CloseHandle(proc);
	}

	void postTo(HWND wnd, UINT msg)
	{
		if (wnd && IsWindow(wnd)) PostMessageW(wnd, msg, 0, 0);
	}

	// 实例窗口：优先用枚举到的，找不到就按类名+代号标题找
	HWND windowOf(const Sibling& sib)
	{
		if (sib.msgWnd && IsWindow(sib.msgWnd)) return sib.msgWnd;
		return FindWindowW(kInstanceClass, editionKey(sib.lite).c_str());
	}

	// 完整版退出时通知 Lite 版取回热键
	void resumeOtherEditionHotkeys()
	{
		for (DWORD pid : siblingProcessIds()) {
			Sibling sib;
			if (!describeProcess(pid, sib) || !sib.lite) continue;
			postTo(windowOf(sib), kResumeHotkeysMsg);
		}
	}

}

InstanceDecision enforceSingleInstance(bool autoQuit)
{
	InstanceDecision decision;
	// 脚本并发截图（--auto-quit=true）不参与
	if (autoQuit) return decision;

	const auto self = modulePath();
	const ULONGLONG selfVer = versionOf(self);
	const bool selfLite = isLiteName(exeNameLower(self));
	const std::wstring selfKey = editionKey(selfLite);

	// AUMID 按代号统一，任务栏不再按版本分组
	SetCurrentProcessExplicitAppUserModelID(selfKey.c_str());

	// 1) 同代互斥锁：两个代号各一把，所以可以并存
	HANDLE mutex = CreateMutexW(nullptr, FALSE, selfKey.c_str());
	bool someoneHolds = false;
	if (!mutex) return decision; // 建不出来就别拦住启动
	if (GetLastError() == ERROR_ALREADY_EXISTS) {
		CloseHandle(mutex);
		mutex = nullptr;
		someoneHolds = true;
	}

	// 2) 枚举兄弟进程：同代参与互斥，异代只影响热键归属
	std::vector<Sibling> peers;
	for (DWORD pid : siblingProcessIds()) {
		Sibling sib;
		if (describeProcess(pid, sib)) peers.push_back(sib);
	}
	std::vector<Sibling> rivals;   // 同代
	std::vector<Sibling> litePeers; // 异代里的 Lite 版（本进程是完整版时才有意义）
	for (auto& sib : peers) {
		if (sib.lite == selfLite) rivals.push_back(sib);
		else if (sib.lite) litePeers.push_back(sib);
	}

	// 3) 热键归属：Lite 让出，完整版收走
	if (selfLite) {
		for (const auto& sib : peers) {
			if (!sib.lite) { decision.ownHotkeys = false; break; }
		}

	}
	else {
		for (const auto& sib : litePeers) postTo(windowOf(sib), kYieldHotkeysMsg);
		// 退出前把热键还给 Lite
		if (auto* lingApp = Ling::App::get()) {
			lingApp->onBeforeQuit.add([]() { resumeOtherEditionHotkeys(); });
		}
	}

	// 4) 同代互斥：新版本留；版本相同则先启动的留
	if (rivals.empty() && !someoneHolds) {
		ensureInstanceWindow(selfKey);
		static HANDLE heldMutex = nullptr;
		heldMutex = mutex;
		return decision;
	}

	const Sibling* top = nullptr;
	for (const auto& sib : rivals) {
		if (!top || sib.version > top->version) top = &sib;
	}
	if (top) {
		const bool rivalWins = top->version >= selfVer; // 同版本 → 先启动的（对方）赢
		if (rivalWins) {
			// 它更该留：把它叫出来，本进程退出
			postTo(windowOf(*top), kOpenSettingsMsg);
			decision.continueLaunch = false;
			return decision;
		}
		// 本进程更新：请它让位；它开着界面时先问一句
		if (top->busy && MessageBoxW(nullptr, Lang::get(L"instance.takeOver").c_str(),
				Lang::get(L"about.sysTip").c_str(),
				MB_YESNO | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND) != IDYES) {
			decision.continueLaunch = false;
			return decision;
		}
		for (auto& sib : rivals) postTo(windowOf(sib), kQuitMsg);
		for (auto& sib : rivals) {
			if (waitExit(sib.pid, 1500)) continue;
			// 不认这个消息的老版本：强关
			terminate(sib.pid);
		}
	}

	// 5) 重新抢锁；拿不到也放行，别让用户打不开
	if (!mutex) {
		for (int i = 0; i < 20; ++i) {
			mutex = CreateMutexW(nullptr, FALSE, selfKey.c_str());
			if (!mutex) return decision;
			if (GetLastError() != ERROR_ALREADY_EXISTS) break;
			CloseHandle(mutex);
			mutex = nullptr;
			Sleep(100);
		}
	}
	ensureInstanceWindow(selfKey);
	static HANDLE heldMutex = nullptr;
	heldMutex = mutex;
	return decision;
}
