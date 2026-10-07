#pragma once
#include <cstdint>
#include <string>
#include <vector>

// 工具栏布局存储 + 主题色板。
// 纯 C++ 数据层，不依赖 Ling UI：设置页编辑器写入，运行时工具栏后续消费。
// 持久化走 Setting 的 config.json（tool= 维度）。
namespace ToolbarStore {

	// —— 布局（toolbar/layout/{kind}/active|inactive） ——
	struct LayoutState {
		std::vector<std::wstring> active;
		std::vector<std::wstring> inactive;
	};
	constexpr int kindCount = 4;   // 0 截图 / 1 录屏 / 2 全屏画布 / 3 贴图

	LayoutState defaultLayout(int kind);
	LayoutState loadLayout(int kind);
	void saveLayout(int kind, const LayoutState& st);
	bool isDefault(int kind);
	void restoreDefaults(int kind);

	// —— 主题色板（toolbar/themeMode + toolbar/custom/*） ——
	// 色槽顺序：背景 / 停悬 / 图标 / 选中 / 框选
	// （停悬图标与常态图标不再细分，统一为同一个"图标"色）
	struct Colors {
		uint32_t primary{ 0x1E1E1FFF };    // 背景
		uint32_t hover{ 0x333333FF };      // 停悬
		uint32_t icon{ 0xE0E0E5FF };       // 图标（常态/停悬同一色）
		uint32_t accent{ 0x34C759FF };     // 选中
		uint32_t selection{ 0x34C759FF };  // 框选
	};
	enum class Mode { System = 0, Dark = 1, Light = 2, Custom = 3 };

	Colors darkPreset();
	Colors lightPreset();
	Colors systemPreset();
	int modeId();                 // 0..3
	void setModeId(int id);
	Colors current();             // 按 mode 返回生效色
	Colors custom();              // 自定义色（Custom 模式用，可编辑）
	void applyCustom(const Colors& c, bool persist = true);
	// 色槽下标 -> 中文名（shadcn 风格色板标签，直接内联免语言包）
	const wchar_t* slotName(int slot);

	// 工具 ID -> 图标字形（IconCodes.h）；split-* / record-time 返回空由 UI 特判
	std::wstring iconOf(const std::wstring& id);
}
