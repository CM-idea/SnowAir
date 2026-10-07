#pragma once
#include <Windows.h>

// 截图/标注期间全局 ESC：Tool 窗常抢不到焦点，靠 RegisterHotKey 兜底
// 录屏穿透开启后全局 Tab：点击桌面后焦点已不在本进程，同样靠热键兜底
namespace CaptureEsc
{
	void claim(HWND hwnd);
	void release(HWND hwnd);
	void claimTab(HWND hwnd);
	void releaseTab(HWND hwnd);
}
