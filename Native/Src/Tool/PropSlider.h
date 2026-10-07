#pragma once
#include <include/Ling.h>
#include "ToolbarTheme.h"
#include "IconCodes.h"
#include <functional>

// 属性栏滑条 + 数值：一级 ToolSub / 二级 ToolNestedPanel 共用同一套尺寸与间距
namespace PropSlider
{
	struct Pair {
		Ling::Slider* slider{ nullptr };
		Ling::Label* value{ nullptr };
	};

	// 滑条左 margin：与前一图标槽拼出「图标本体 ↔ 滑条」= propGap
	inline float marginH()
	{
		const float iconSlot = Icon::Size + ToolbarTheme::propGap;
		const float iconSide = (ToolbarTheme::propIconInner - Icon::Size) * 0.5f;
		const float marginHIcon = (iconSlot - ToolbarTheme::propIconInner) * 0.5f;
		return ToolbarTheme::propGap - iconSide - marginHIcon;
	}

	// 滑条行逻辑宽（不含 propPad）：marginH + track + gap + value
	inline float rowLogicW()
	{
		return marginH() + ToolbarTheme::sliderWidth
			+ ToolbarTheme::sliderValueGap + ToolbarTheme::sliderValueWidth;
	}

	Pair mount(Ling::Node* parent, float minV, float maxV, float val,
		std::function<void(float)> onChange);
}
