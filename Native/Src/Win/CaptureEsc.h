#pragma once
#include <Windows.h>

// 截图/标注期间全局 ESC：Tool 窗常抢不到焦点，靠 RegisterHotKey 兜底。
// ⚠ 只有 Esc 这一条 —— 鼠标穿透（`` ` ``）由「系统按键」表在会话期间镜像到低级键盘钩子
//   （见 WinCap::syncGlobalCommandKeys），不再需要额外注册 Tab。
namespace CaptureEsc
{
	void claim(HWND hwnd);
	void release(HWND hwnd);
}
