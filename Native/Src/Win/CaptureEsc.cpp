#include "pch.h"
#include "CaptureEsc.h"

namespace
{
	HWND g_owner{ nullptr };
	constexpr int kEscHotkeyId{ 1 };
}

namespace CaptureEsc
{
	void claim(HWND hwnd)
	{
		if (!hwnd || g_owner == hwnd) return;
		if (g_owner) {
			UnregisterHotKey(g_owner, kEscHotkeyId);
		}
		if (RegisterHotKey(hwnd, kEscHotkeyId, 0, VK_ESCAPE))
			g_owner = hwnd;
		else
			g_owner = nullptr;
	}

	void release(HWND hwnd)
	{
		if (!hwnd || g_owner != hwnd) return;
		UnregisterHotKey(g_owner, kEscHotkeyId);
		g_owner = nullptr;
	}
}
