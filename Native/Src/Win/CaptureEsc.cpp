#include "pch.h"
#include "CaptureEsc.h"

namespace
{
	HWND g_owner{ nullptr };
	bool g_tabOn{ false };
	constexpr int kEscHotkeyId{ 1 };
	constexpr int kTabHotkeyId{ 2 };
}

namespace CaptureEsc
{
	void claim(HWND hwnd)
	{
		if (!hwnd || g_owner == hwnd) return;
		if (g_owner) {
			UnregisterHotKey(g_owner, kEscHotkeyId);
			if (g_tabOn) UnregisterHotKey(g_owner, kTabHotkeyId);
		}
		g_tabOn = false;
		if (RegisterHotKey(hwnd, kEscHotkeyId, 0, VK_ESCAPE))
			g_owner = hwnd;
		else
			g_owner = nullptr;
	}

	void release(HWND hwnd)
	{
		if (!hwnd || g_owner != hwnd) return;
		UnregisterHotKey(g_owner, kEscHotkeyId);
		if (g_tabOn) UnregisterHotKey(g_owner, kTabHotkeyId);
		g_tabOn = false;
		g_owner = nullptr;
	}

	void claimTab(HWND hwnd)
	{
		if (!hwnd || g_owner != hwnd || g_tabOn) return;
		if (RegisterHotKey(hwnd, kTabHotkeyId, 0, VK_TAB))
			g_tabOn = true;
	}

	void releaseTab(HWND hwnd)
	{
		if (!hwnd || g_owner != hwnd || !g_tabOn) return;
		UnregisterHotKey(g_owner, kTabHotkeyId);
		g_tabOn = false;
	}
}
