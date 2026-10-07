#pragma once
#include <cstdint>
#include "ToolbarStore.h"
#include "IconCodes.h"

// 主栏 / 属性栏间距
namespace ToolbarTheme
{
	inline constexpr float shadowPad{ 0.f };
	inline constexpr float borderRadius{ 10.f };
	inline constexpr float hoverRadius{ 8.f };
	inline constexpr float borderWidth{ 1.f };

	// 主栏：【12】拖拽【12】工具…【24】；工具本体间距 20
	inline constexpr float paddingLeft{ 12.f };
	inline constexpr float paddingRight{ 24.f };
	inline constexpr float dragGap{ 12.f };
	inline constexpr float dragIconSize{ 24.f };
	inline constexpr float iconGap{ 20.f };
	// 分隔线本体到两侧按钮本体的间距（线侧 margin = splitterGap - 按钮 hoverInset）
	inline constexpr float splitterGap{ 12.f };

	// 属性栏 / 色板：左右各 16，项间距 16
	inline constexpr float propPad{ 16.f };
	inline constexpr float propGap{ 16.f };
	inline constexpr float propBtnSize{ 42.f };
	inline constexpr float propIconInner{ 34.f };
	inline constexpr float caretSize{ 6.f };
	inline constexpr float sliderWidth{ 100.f };
	// 一级 / 二级属性栏滑条右侧数值区（共用，勿在子面板再写一套）
	inline constexpr float sliderValueWidth{ 28.f };
	// 滑条轨道控件 ↔ 数值：比 propGap 更紧（对齐参考属性气泡）
	inline constexpr float sliderValueGap{ 8.f };
	// 统一：QT CaptureInfoBar 圆角滑条（轨 4 / 钮 14 / #333·绿·浅灰）
	// 颜色随工具栏主题刷新（见文件末尾 refresh()），故不再写 constexpr。
	inline constexpr float sliderTrackH{ 4.f };
	inline constexpr float sliderThumbR{ 7.f };
	inline uint32_t sliderTrack{ 0x333333FFu };
	inline uint32_t sliderFill{ 0x34C759FFu };
	inline uint32_t sliderThumb{ 0xD9D9D9FFu };

	// 色块统一（对齐 QT swatchSize=18）
	inline constexpr float swatchSize{ 18.f };
	inline constexpr float swatchRing{ 22.f };
	inline constexpr float swatchRadius{ 2.f };

	// —— 主题色（运行时随 ToolbarStore::current() 刷新，勿再写 constexpr／勿当编译期常量用）——
	inline uint32_t background{ 0xFFFFFFFFu };
	inline uint32_t hoverBg{ 0xF2F2F2FFu };
	inline uint32_t border{ 0xE0E0E0FFu };
	inline uint32_t splitter{ 0xDDDDDDFFu };
	inline uint32_t dragHandleColor{ 0x51515180u };
	// 图标色（工具栏按钮）：正常/停悬同色 / 禁用 / 选中 / 框选
	inline uint32_t iconNormal{ 0x515151FFu };
	inline uint32_t iconDisabled{ 0x51515166u };
	inline uint32_t iconActive{ 0x34C759FFu };
	inline uint32_t selection{ 0x34C759FFu };

	// 选区左上角信息条
	inline constexpr float infoHeight{ 36.f };
	inline constexpr float infoRadius{ 10.f };
	inline constexpr float infoPadX{ 12.f };
	inline constexpr float infoIconSlot{ 24.f };    // 锁 / 圆角 / 阴影
	inline constexpr float infoIconGap{ 8.f };
	inline constexpr float infoSplitterGap{ 10.f };
	inline constexpr float infoSwatchGap{ 8.f };
	// 圆角滑条宽 / 数值宽：与属性栏 PropSlider 同一套 ToolbarTheme::sliderWidth / sliderValueWidth
	// 与选区间距：与主工具栏相同，见 CutMask（strokeWidth + 2×dpi）
	inline constexpr float infoFontSize{ 14.f };
	inline constexpr uint32_t infoBg{ 0x1E1E1E46u };
	inline constexpr uint32_t infoText{ 0xFFFFFFE6u };
	inline constexpr uint32_t infoIconMuted{ 0xFFFFFFB3u }; // ~70% 白，深底上的默认图标

	// 把 ToolbarStore::current() 的色板刷进上面的运行时变量。
	// 调用时机：工具栏窗口创建时（各 Tool*.cpp 的 onCreated 首行）与设置页切换主题时。
	// 工具栏里大量直接引用 Icon::ColorNormal / Icon::ColorDisabled 的调用点也一并跟随主题，
	// 免去逐个改调用点（这两个常量仅被工具栏/属性栏使用）。
	inline void refresh()
	{
		const auto c = ToolbarStore::current();
		auto withAlpha = [](uint32_t rgba, uint32_t a) { return (rgba & 0xFFFFFF00u) | a; };
		// 背景 / 停悬底
		background = c.primary;
		hoverBg = c.hover;
		// 描边 / 分隔线 / 拖拽手柄：由图标色淡化派生（浅色主题→淡灰，深色主题→淡白）
		border = withAlpha(c.icon, 0x26u);
		splitter = withAlpha(c.icon, 0x26u);
		dragHandleColor = withAlpha(c.icon, 0x80u);
		// 滑条
		sliderTrack = c.hover;
		sliderFill = c.accent;
		sliderThumb = c.icon;
		// 图标（停悬与常态同一色，不再细分）
		iconNormal = c.icon;
		iconDisabled = withAlpha(c.icon, 0x66u);
		iconActive = c.accent;
		selection = c.selection;
		// 工具栏按钮大量直接用 Icon::ColorNormal / ColorDisabled → 同步过去
		Icon::ColorNormal = c.icon;
		Icon::ColorDisabled = withAlpha(c.icon, 0x66u);
	}
}
