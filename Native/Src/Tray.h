#pragma once
#include <include/Ling.h>

class Tray
{
public:
	~Tray();
	static void init();
	// 退出前主动拆掉托盘单例：菜单窗口、图标句柄缓存都依赖别的静态对象，
	// 留给静态析构会出现"缓存已被先一步销毁"的非法访问
	static void dispose();
	static Tray* get();
	void onTrayRightClick();   // 托盘右键菜单入口
private:
	Tray();
	winrt::event_token mouseTok{};
};
