#pragma once
#include <include/Ling.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// 贴图右键菜单：自绘圆角弹层，外观复用托盘菜单（MenuTheme）。
// 支持普通项 / 勾选项 / 分隔线 / 子菜单（右侧箭头，悬停展开）；点菜单外或选中后自动关闭。
// 全局只允许一个菜单实例（与托盘菜单同款：低级鼠标钩子点外关闭）。
class PinMenu : public Ling::WinBase
{
public:
	struct Item
	{
		std::wstring text;
		bool separator{ false };
		bool checkable{ false };
		bool checked{ false };
		bool enabled{ true };
		std::function<void()> action;
		// 滑条项（text 作为左侧标题，可空）：用于「透明度」这类连续值
		bool slider{ false };
		float sliderMin{ 10.f };
		float sliderMax{ 100.f };
		float sliderValue{ 100.f };
		std::function<void(float)> onSlide;
		// 子菜单：非空表示本项带右侧箭头，鼠标悬停时在其右侧展开；子项不再嵌套
		std::vector<Item> submenu;
	};

	// 在屏幕坐标 at 处弹出菜单（左上角贴光标，放不下自动翻转/钳制）。
	static void popup(std::vector<Item> items, POINT at);
	static bool isOpen();
	static void closeMenu();

	~PinMenu();

private:
	explicit PinMenu(std::vector<Item> items);
	void onCreated() override;
	void layout() override;
	void openAt(POINT at);
	float logicalHeight() const;

	// 按当前 items_ 计算并设置窗口尺寸（含滑条项加宽）
	void applySize();
	// 展开 rowIndex 项的子菜单（定位到该行右侧）
	void openSubmenu(int rowIndex, Ling::Button* ownerBtn);
	void closeSubmenu();
	// 关闭自身及所有子菜单窗口（WinBase 析构不销毁 HWND，须逐层 close）
	void closeTree();

	// —— 全局菜单管理 ——
	static void closeAll();
	// 命中测试：点是否落在 m 或其任意子菜单窗口内
	static bool treeHit(PinMenu* m, POINT pt);
	static LRESULT CALLBACK lowLevelProc(int code, WPARAM wp, LPARAM lp);

	std::vector<Item> items_;
	std::unique_ptr<PinMenu> submenu_;   // 当前展开的子菜单（随父菜单销毁）
	int submenuOwner_{ -1 };             // 子菜单对应的 items_ 下标（-1 表示无）

	static std::unique_ptr<PinMenu> s_root;
	static HHOOK s_hook;
};
