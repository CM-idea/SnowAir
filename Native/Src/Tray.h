#pragma once
#include <include/Ling.h>

class Tray
{
public:
	~Tray();
	static void init();
	static Tray* get();
	void onTrayRightClick();   // 托盘右键菜单入口
private:
	Tray();
};
