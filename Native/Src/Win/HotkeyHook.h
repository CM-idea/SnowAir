#pragma once
#include <Windows.h>
#include <functional>
#include <string>
#include <vector>

// 全局热键兜底：RegisterHotKey 是"先注册先独占"的语义，没有可调优先级 —— 启动时若目标
// 组合已被其他进程注册，本次注册会直接失败且不产生任何提示，表现为该热键暂不生效。
// 这里提供兜底：注册失败时改用低级键盘钩子。钩子先于常规热键与加速键取得按键事件，
// 命中后即消费该事件，效果等同于该组合由本进程持有。
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

	// —— 按键录制模式（设置页等待用户按键时）——
	// 录制期间使用一个临时低级键盘钩子接管键盘输入：按键在进入系统热键分发之前即被消费，
	// 再异步转交给 UI 线程。这样即使目标组合已被其他进程注册，也能被正常录制。
	// 回调在 UI 线程执行；vk 已把左右修饰键归一成 VK_CONTROL / VK_SHIFT / VK_MENU。
	// mods 是**钩子线程在按键发生那一刻**记下的修饰键状态（MOD_CONTROL/MOD_ALT/MOD_SHIFT/MOD_WIN）：
	// 钩子会把这些物理按键吞掉，GetAsyncKeyState 事后已经查不到它们，只能随事件一起带过来。
	using CaptureCallback = std::function<void(UINT vk, bool up, UINT mods)>;
	void beginCapture(CaptureCallback cb);
	void endCapture();
	bool capturing();
}
