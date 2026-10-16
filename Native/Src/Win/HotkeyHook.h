#pragma once
#include <functional>
#include <string>
#include <vector>

// 全局热键兜底：RegisterHotKey 是"先注册先独占"，没有优先级可抢 —— 打开软件那一刻这个组合
// 若已被别的程序（浏览器/浏览器插件、聊天工具、输入法…）占着，我们那次注册就失败，而且失败得
// 静悄悄，表现就是"软件热键被浏览器抢了，得去设置里重新设一次才好使"。
// 这里提供兜底：注册失败时改装低级键盘钩子。钩子比任何程序的热键/加速键都先取得按键，
// 匹配上即吞掉该按键，效果等同于"这个组合归本程序"。
// 一个钩子线程同时承载多条组合键：命中哪条就回调哪条。
namespace HotkeyHook
{
	// 一条绑定。chord 形如 "Ctrl+Shift+A"，键名口径与 Ling::App::regHotKey 一致。
	struct Binding
	{
		std::wstring chord;
		std::function<void()> onFire;
	};

	// 用一组绑定整体替换当前钩子；列表为空（或键名全部解析失败）= 等价于 uninstall。
	// 回调都在 UI 线程上跑（内部收在钩子线程的按键，PostMessage 回 UI 线程再调）。
	void setBindings(std::vector<Binding> bindings);
	// 会话期间**临时追加**的绑定（「系统按键」里 Scope::Global 的那三条：开始/暂停录制、
	// 鼠标穿透、隐藏/显示工具栏）。只在对应会话期间存在，会话结束传空表即撤。
	// 实现上是与主绑定**合并**后整体重装 —— 同一条钩子，不另起一个。
	// ⚠ 会话绑定**优先**匹配：录屏时 F1 是"开始/暂停录制"，不该被全局热键那次"区域截图"截走。
	void setSessionBindings(std::vector<Binding> bindings);
	void uninstall();
	bool installed();
}
