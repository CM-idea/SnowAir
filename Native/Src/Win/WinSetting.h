#pragma once
#include <include/Ling.h>
#include <memory>

namespace SettingUi { class HoverTip; }   // SettingWidgets.h：设置项「?」的停悬气泡
class WinSettingHotkeys;
class WinSettingSystemKeys;
class WinSettingQuickTranslate;
class WinSettingHistory;
class WinSettingPlugins;
class WinSettingAppearance;
class WinSettingFeatures;
class WinSettingAbout;

class WinSetting :public Ling::WinBase
{
public:
	~WinSetting();
	static void init();
	// 打开设置中心并切到指定菜单页（0 全局热键 / 1 快捷翻译 / 2 系统按键 / 3 外观 / 4 功能 / 5 截图历史 / 6 插件 / 7 关于）
	static void init(int pageIndex);
	static void dispose();
	// 按「停用截图历史」开关同步侧栏「截图历史」菜单显隐（设置里切换后立即生效）
	static void applyHistoryMenuVisibility();
private:
	WinSetting();
	void initMenuItems(Ling::Node* menuBox);
	void onCreated() override;
	void onMenuItemClick(Ling::Button* menu);
	void loadPage(int index);   // 按 index 构建对应的子页面 Node 到 content
	void showPage(int index);   // 切到指定页（已打开时也生效）
	LRESULT onHitTest(const POINT pos) override;
private:
	std::vector<Ling::Button*> menus;
	int menuIndex{ 0 };
	Ling::Node* content{ nullptr };            // 子页挂接点（contentScroll->content 的唯一子节点）
	Ling::ScrollerBox* contentScroll{ nullptr }; // 卡片内层整页滚动容器（contentScroll->content 负责 padding）
	Ling::Node* cardNode{ nullptr };           // 白色内容卡片（视口容器）；快捷翻译页直接挂这
	std::unique_ptr<SettingUi::HoverTip> helpTip_;   // 设置项旁的「?」说明气泡（需宿主 hwnd，故在 onCreated 里建）

	// 子页实例（切换时自动销毁旧页）—— 各子页继承 Ling::Node，作为 content 的唯一子节点
	WinSettingHotkeys*        pageHotkeys{ nullptr };
	WinSettingSystemKeys*     pageSysKeys{ nullptr };
	WinSettingQuickTranslate* pageQuick{ nullptr };
	WinSettingHistory*        pageHistory{ nullptr };
	WinSettingPlugins*        pagePlugins{ nullptr };
	WinSettingAppearance*     pageAppearance{ nullptr };
	WinSettingFeatures*       pageFeatures{ nullptr };
	WinSettingAbout*          pageAbout{ nullptr };
};
