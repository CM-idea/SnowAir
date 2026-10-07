#pragma once
#include <functional>
#include <string>

// 全局热键兜底：RegisterHotKey 是"先注册先独占"，没有优先级可抢 —— 打开软件那一刻这个组合
// 若已被别的程序（浏览器/浏览器插件、聊天工具、输入法…）占着，我们那次注册就失败，而且失败得
// 静悄悄，表现就是"软件热键被浏览器抢了，得去设置里重新设一次才好使"。
// 这里给个兜底：注册失败时改装低级键盘钩子。钩子比任何程序的热键/加速键都先拿到按键，
// 匹配上就顺手吃掉，效果上就是"这个组合归我们"。
namespace HotkeyHook
{
	// 装钩子。chord 形如 "Ctrl+Alt+A"，键名口径与 Ling::App::regHotKey 一致。
	// 回调在 UI 线程上跑（内部收在钩子线程的按键，PostMessage 回 UI 线程再调）。
	void install(const std::wstring& chord, std::function<void()> onFire);
	void uninstall();
	bool installed();
}
