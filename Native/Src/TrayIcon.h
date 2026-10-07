#pragma once
#include <windows.h>
#include <cstdint>
#include <string>

// 托盘图标样式与生成。
// 内置样式 = 把图标字库里的 Logo 字形按样式上色后转成 HICON；自定义图标 = 用用户选的图片文件。
namespace TrayIcon
{
	enum class Style { FollowSystem = 0, Theme = 1, CustomColor = 2, CustomIcon = 3 };

	// 某样式对应的着色（CustomIcon 返回 false，表示用图片本身的颜色）
	bool tintFor(int style, uint32_t& argb);

	// 按样式生成托盘图标句柄（内部缓存，调用方不要销毁）；size<=0 取系统小图标尺寸
	HICON iconFor(int style, int size = 0);

	// 读取 Setting 并应用到系统托盘（图标样式 + 显隐）。启动时与设置变更后调用。
	void apply();

	// 释放缓存图标（进程退出前调用，避免 GDI 句柄泄露）
	void dispose();
}
