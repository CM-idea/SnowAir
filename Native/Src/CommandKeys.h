#pragma once
#include <string>
#include <vector>

// 「系统按键」：本软件**命令**的可配置按键（默认值 + 冲突判定 + 查表派发）。
// 移植自 QT6 的 src/app/CommandKeys.h/.cpp；按用户口径**砍掉了「贴图/镜像」组**，
// 因此界面掩码也从 6 位收敛为 3 位（截图 / 录屏 / 全屏画布）。
//
// ⚠ 为什么不能并进「全局热键」（Setting 的 kHotkeys）——这是设计约束，别再走回头路：
//   全局热键走 RegisterHotKey / 低级键盘钩子，**命中后会消费按键**。
//   撤销/复制/完成/关闭 这类命令键一旦进全局钩子，用户在**任何进程**里按
//   Ctrl+Z / Ctrl+C / Enter / Esc 都会被本软件消费 —— 这不符合命令键的作用域。
//   所以命令键一律走**窗口内**：只在本软件的会话窗口有焦点时生效，
//   由会话窗口把按键交给 actionForKey() 查这张表。
//
//   ⚠ 只有三条是 Scope::Global（临时全局键），而且都只在**对应会话期间**生效：
//       · 「开始/暂停录制」—— 录屏时焦点在被录的程序上，窗口内根本收不到；
//       · 「鼠标穿透」「隐藏/显示工具栏」—— 它们本身就把焦点交出去（或把工具栏收起来），
//         没有一条全局的路就切不回来。
//     其余命令键一律只在窗口内生效（GIF / 系统音量 / 麦克风也是：它们只在工具栏有焦点时
//     用得到），以免在其他进程中无谓地消费用户的按键。
//
// ⚠ 键位可以**跨界面重复**：同一个键在不同工具栏上可以是两条不同命令 ——
//   冲突判定按**界面掩码有无交集**做，无交集就不算冲突（见 boundToOther）。
namespace CommandKeys
{
	// 键位的生效方式
	enum class Scope {
		Session, // 窗口内：只在本软件的会话窗口有焦点时生效（绝大多数命令）
		Global,  // 全局：焦点不在本软件时也要响应（录屏控制类，由全局热键代注册）
	};

	// 场景：这条命令会在**哪一种窗口**上被派发
	enum class Scene {
		None,       // 未归类（= 任意场景都能命中，通用绘图工具用这个）
		Capture,    // 截图会话（含录屏会话内的标注工具栏）
		Record,     // 录屏会话的全局控制
		Fullscreen, // 全屏画布（演示模式）
	};

	// 界面（工具栏宿主）：**冲突判定的真正粒度**
	enum Surface : unsigned {
		SurfaceNone = 0u,
		SurfaceCapture = 1u << 0,     // 截图工具栏
		SurfaceRecord = 1u << 1,      // 录屏工具栏
		SurfaceFullscreen = 1u << 2,  // 全屏画布工具栏
	};
	// 通用绘图工具（矩形…橡皮擦）在**每一个**工具栏上都有同一颗按钮 → 参与所有判定
	constexpr unsigned SurfaceAll = SurfaceCapture | SurfaceRecord | SurfaceFullscreen;

	// —— 查询（未登记的 id 一律给"安全默认"：不生效 / 空标签 / 全界面掩码）——
	bool isKnownId(const std::wstring& id);
	std::wstring labelKey(const std::wstring& id);      // 行标签的语言键（Lang::get 用）
	Scope scopeOf(const std::wstring& id);
	Scene sceneOf(const std::wstring& id);
	bool implemented(const std::wstring& id);           // 代码里是否已有实现（false = 设置行先占位）
	bool readOnly(const std::wstring& id);              // 只读提示行：只展示、不可改绑；键位固定为默认值，**照旧派发**
	unsigned surfaceMaskForId(const std::wstring& id);
	std::wstring defaultBinding(const std::wstring& id);

	// 生效键位：落盘值优先；**用户显式清空过就保持空**（不回落默认值），从没设过才用默认值
	std::wstring binding(const std::wstring& id);
	// 写入并落盘；chord 为空 = 清除。未登记的 id / 只读行一律不写
	void setBinding(const std::wstring& id, const std::wstring& chord);
	// 「恢复默认」：删掉全部用户覆盖项，binding() 于是逐条回落命令表里的默认键位。
	// ⚠ 不能写成 setBinding(id, "")：那是"用户显式清空"，会保持不绑而不是回落默认。
	void resetBindings();
	// 是否存在用户改绑（存过值就算改过，包括"覆盖时把旧主解绑成空"）→ 供「恢复默认」按钮置灰
	bool anyCustomBinding();

	// —— 分组（顺序即设置页的块顺序，唯一来源是本模块的表）——
	std::vector<std::wstring> groupOrder();                         // 分组语言键
	std::vector<std::wstring> idsInGroup(const std::wstring& gkey); // 某组内的 id 顺序
	bool groupIsGlobal(const std::wstring& gkey);                   // 该组是否整组都是全局键

	// 冲突判定：chord 是否已被**同一界面上**的别的命令占用；命中则回写占用者的行标签语言键与命令 id。
	// 判据是"界面掩码有没有交集"，不是"场景是否相同"（跨界面同键是允许的）。
	bool boundToOther(const std::wstring& chord, const std::wstring& forId,
		std::wstring* otherLabelKey, std::wstring* otherId = nullptr);

	// 固定键判定：chord 是否被**只读行**（设置页只展示的既有行为固定键）占用。
	// 与 boundToOther 不同，此处**跨界面一律算占用**：固定键不允许被其他命令改绑。命中则回写占用者标签。
	bool reservedChord(const std::wstring& chord, std::wstring* ownerLabelKey);

	// chord 是否"可解析"（修饰键 + 一个能被 strToKey 认出来的主键）。
	// 设置页落盘前的最后一道闸：存进去但解析不出来的键 = 一条永远命中不了的死绑定。
	bool validChord(const std::wstring& chord);

	// 键 → 命令 id。
	// `surface` = 会话窗口自己的工具栏掩码；返回**已实装、掩码有交集**的命令
	// （只读行的键位固定，但**照样参与派发**）。
	// 修饰键由本函数自己读当前键盘状态（与调用方读 GetKeyState 的口径一致）。
	std::wstring actionForKey(unsigned vk, unsigned surface);
	// 已拼好的 chord 版本（供全局热键那边判"这条键在当前界面上有没有人接"）
	std::wstring actionForChord(const std::wstring& chord, unsigned surface);

	// 命令 id → 工具栏**动作 id**（ToolCap 的槽位/动作口径）；空串 = 不是工具栏动作。
	// ⚠ 设置页的行 id 是"命令"的稳定标识，工具栏槽 id 是**布局持久化的键**，两者不能混用。
	std::wstring toolbarActionForId(const std::wstring& id);
	// 需要先选中子工具的（包浆/水印/高亮）；空串 = 不需要
	std::wstring subToolForId(const std::wstring& id);

	// 「快捷键提示」用：某个工具栏槽位在该界面上当前的键位；空串 = 该槽位无对应命令。
	// slotId = 槽位 id，effective = 槽位当前的有效工具（ToolCap::resolveToolId：气泡里选了子工具时
	// 是子工具 id，如 mosaic 槽选「包浆」时为 patina）。
	std::wstring hintChord(const std::wstring& slotId, const std::wstring& effective, unsigned surface);
}
