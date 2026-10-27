#pragma once
#include <Windows.h>
#include <uxtheme.h>
#include <cstdint>
#include "Setting.h"
#include "Win/SettingTheme.h"

// 托盘菜单 / 贴图右键菜单共用外观：配色（跟随「软件主题」）+ 常用尺寸。
// 从 Tray.cpp 抽出，避免两套菜单各自维护一份配色。
namespace MenuTheme
{
	struct Palette { uint32_t bg, border, fg, hover, hoverFg, sep; };

	// 菜单配色随「软件外观 → 软件主题」：跟随系统(0) 读系统 / 浅色(1) / 深色(2)。
	inline Palette palette()
	{
		Palette p;
		const int mode = Setting::get() ? Setting::get()->getAppTheme() : 1;
		const bool dark = (mode == 2) || (mode == 0 && !SettingTheme::sysIsLight());
		if (!dark) {
			p.bg = 0xFFFFFFFF;       // 浅色：白底
			p.border = 0x0000001A;   // 浅色：1px rgba(0,0,0,0.1)
			p.fg = 0x0B0C0EE0;       // 浅色：前景色 alpha 224
			p.hover = 0xF4F4F5FF;    // 浅色：悬停底色
			p.hoverFg = 0x0B0C0EFF;  // 浅色：悬停前景色
			p.sep = 0x0000000D;      // 浅色：分隔线 rgba(0,0,0,0.05)
		}
		else {
			p.bg = 0x1F1F1FFF;       // 深色：菜单底 #1F1F1F
			p.border = 0xFFFFFF1A;   // 深色：1px rgba(255,255,255,0.1)
			p.fg = 0xF5F5F5E6;       // 深色：前景色 alpha 230
			p.hover = 0x363635FF;    // 深色：悬停底 #363635
			p.hoverFg = 0xFFFFFFFF;  // 深色：悬停前景色
			p.sep = 0xFFFFFF14;      // 深色：分隔线 rgba(255,255,255,0.08)
		}
		return p;
	}

	// —— 尺寸（逻辑像素，和托盘菜单一致）——
	inline constexpr int kMenuW{ 168 };
	inline constexpr int kRowH{ 28 };
	inline constexpr int kMenuPadY{ 4 };
	inline constexpr int kSepMarginX{ 10 };
	inline constexpr int kItemMarginX{ 4 };
	inline constexpr int kItemPadX{ 12 };
	inline constexpr int kItemRadius{ 4 };
	inline constexpr int kMenuRadius{ 8 };
	inline constexpr int kSepH{ 11 };
}
