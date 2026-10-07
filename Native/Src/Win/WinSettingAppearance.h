#pragma once
#include <include/Ling.h>
#include <array>
#include <string>
#include <vector>
#include "SettingWidgets.h"
#include "../Tool/ToolbarStore.h"
/**
 * 外观设置页（3 Tab：工具栏外观 / 软件外观 / 其他外观）。
 * Tab 0 工具栏外观：工具栏类型切换 / 显隐 / 拖拽排序 /
 * 恢复默认 / 预览效果 / 主题色板（4 模式 + 6 色槽自定义）。
 * 数据层（布局+色板）走 ToolbarStore，纯 C++ 与 UI 解耦，运行时工具栏后续直接消费。
 */
class WinSettingAppearance:public Ling::Node
{
public:
	WinSettingAppearance(Ling::WinBase* parent);
	~WinSettingAppearance();
	void hideSelectBox(); // 保留：语言下拉浮层仍然挂在 body 下
private:
	void showTab(int index);
	void buildTabActions(Ling::Node* row);
	void buildSegTabs(Ling::Node* row);        // 顶部 3 个分段胶囊（Tab0 为「标题+▾下拉」组合）
	void segHighlight(int index);              // 程序化高亮选中段（不触发 onChange）
	void tbShowKindMenu();                     // Tab0 ▾ 下拉：工具栏类型菜单
	void tbUpdateKindTitle();                  // 刷新 Tab0 标题（××工具栏外观）

	void buildToolbarTab(Ling::Node* host);    // Tab 0
	void buildAppTab(Ling::Node* host);        // Tab 1：自启+语言+主题+贴图边框
	void buildOtherTab(Ling::Node* host);      // Tab 2：功能提示

	// Tab 1 内部子组件
	void initAutoStartCtrls(Ling::Node* p);
	void initLangCtrls(Ling::Node* p);
	void initThemeCtrls(Ling::Node* p);
	void initPinBorderCtrls(Ling::Node* p);
	void initTrayCtrls(Ling::Node* p);
	void refreshPinPreview();      // 贴图边框预览：按当前颜色/粗细/圆角重绘
	void pickPinColor();           // 贴图边框颜色：系统取色器
	void refreshTrayRadios();      // 托盘样式单选：按当前样式刷新选中态
	void refreshTrayPreviews();    // 托盘图标预览：自定义颜色/自定义图标两格的刷新
	void pickTrayColor();          // 自定义颜色：系统取色器
	void pickTrayIcon();           // 自定义图标：选文件
	void setAutoStartBtn(Ling::Button* btn);
	void showSelectBox(Ling::Button* btn);

	// —— 工具栏编辑器 ——
	void tbSetKind(int kind);
	void tbRebuildZones();
	void tbPersist();
	void tbRefreshRestore();
	void tbRestoreDefaults();
	void tbTogglePreview();
	void tbRefreshPreview();
	Ling::Button* tbMakeChip(const std::wstring& id, bool active);
	void tbStyleChip(Ling::Button* b, const std::wstring& id, bool active);
	std::wstring tbTipText(const std::wstring& id) const;   // chip 悬停文字
	void tbShowGhost(const std::wstring& id, POINT pos);    // 拖拽时跟手的浮动幽灵
	void tbHideGhost();
	// chip 右上角的红叉：**不能挂在 chip 下**（chip 有圆角 clip 会把它裁掉），
	// 改为页级共享的一个小按钮，悬停某 chip 时移到它的右上角外侧。
	void tbShowChipClose(Ling::Button* chip, const std::wstring& id);
	void tbHideChipClose();
	void tbBuildChips(Ling::Node* box, const std::vector<std::wstring>& ids, bool active);
	void tbOnDown(POINT pos, bool right);
	void tbOnMove(POINT pos);
	void tbOnUp(POINT pos, bool right);
	void tbDrop();
	void tbToggleItem();
	int tbComputeDropIndex(POINT c, int toZone) const;
	void tbUpdateDropHint();
	void tbClearDropHint();
	bool tbLocked(const std::wstring& id) const;
	static bool tbIsSplit(const std::wstring& id);
	std::wstring tbNextSplitId();
	POINT tbToContent(POINT pos) const;
	int tbHitZone(POINT p, int fallback) const;
	// —— 主题色板 ——
	void tbBuildTheme(Ling::Node* host);
	void tbRefreshTheme();
	void tbPickColor(int slot);

private:
	Ling::Node* tabHost{ nullptr };
	Ling::Node* tabs[3]{ nullptr };
	int tabIndex{ 0 };

	// 顶部 3 个分段胶囊（Tab0 为「标题 + ▾下拉」组合，保留现有胶囊样式）
	Ling::Node* seg0Box{ nullptr };       // Tab0 胶囊容器
	// Tab0：整条既是"切到工具栏外观页"又是"工具栏类型下拉"：
	// 已是当前页 → 点击弹出类型菜单；否则先切到该页。▾ 是按钮内的一个 Label。
	Ling::Button* seg0TitleBtn{ nullptr };
	Ling::Label* seg0Arrow{ nullptr };    // 按钮内的 ▾ 图标（左右各留 8px）
	Ling::Button* segBtn1{ nullptr };     // Tab1 胶囊
	Ling::Button* segBtn2{ nullptr };     // Tab2 胶囊
	// 工具栏类型下拉浮层（挂在 Tab0 ▾ 上）
	std::unique_ptr<SettingUi::Popup> tbKindPopup;

	// 语言下拉浮层（独立 WS_POPUP 弹层窗口，不受宿主客户区裁剪 / 底部上拉规避）
	Ling::Button* selectBtn{ nullptr };
	std::unique_ptr<SettingUi::Popup> selectPopup;

	// —— 工具栏编辑器状态 ——
	Ling::ScrollerBox* tbScroller{ nullptr };
	Ling::Node* tbActiveBox{ nullptr };
	Ling::Node* tbInactiveBox{ nullptr };
	Ling::Node* tbPreviewWrap{ nullptr };   // 预览容器（可整体隐藏）
	Ling::Button* tbRestoreBtn{ nullptr };
	Ling::Button* tbPreviewBtn{ nullptr };   // 「预览效果」：仅工具栏外观页显示
	int tbKind{ 0 };
	std::vector<std::wstring> tbActive, tbInactive;
	std::vector<Ling::Button*> tbChips;     // 与 tbActive 顺序同步
	std::vector<Ling::Button*> tbHidden;    // 与 tbInactive 顺序同步
	// 拖拽
	bool tbDragging{ false };
	bool tbDragHooked{ false };   // move/up 是否已挂载（懒挂载）
	POINT tbDragStart{};
	POINT tbDragLast{};
	std::wstring tbDragId;
	int tbDragFrom{ -1 };   // 0 active / 1 inactive
	int tbDragTo{ -1 };     // 落点区（tbDrop 用）
	int tbDropIndex{ -1 };  // 落点插入位（tbDrop 用）
	Ling::Button* tbDragChip{ nullptr };
	// 拖拽视觉：拖动中在"插入位置"显示一条蓝色光标（竖条）
	int tbLiveDropTo{ -1 };   // 实时落点区（0 active / 1 inactive）
	int tbLiveDropIndex{ -1 };// 实时落点插入位
	Ling::Node* tbCursorActive{ nullptr };   // 显示区插入光标（蓝色竖条，绝对定位）
	Ling::Node* tbCursorInactive{ nullptr }; // 隐藏区插入光标
	// 拖拽幽灵：挂在设置窗 body 上的绝对定位浮块（不随页面滚动裁剪），跟手显示被拖的图标
	Ling::Node* tbGhost{ nullptr };
	Ling::Label* tbGhostIcon{ nullptr };
	// chip 悬停文字（系统 tooltip，同截图工具栏）
	std::unique_ptr<class Tip> tbTip_;
	// chip 右上角红叉（页级共享，绝对定位；不挂在 chip 下，避免被 chip 圆角裁掉）
	Ling::Button* tbChipClose{ nullptr };
	std::wstring tbChipCloseId;
	winrt::event_token tbDownTok;
	winrt::event_token tbMoveTok, tbUpTok;
	// 预览
	bool tbPreviewShown{ false };
	// 主题色板
	std::array<Ling::Button*, 4> tbThemeRadios{};   // 单选外圈（实心圆 → 叠白色内圆成圆环）
	std::array<Ling::Node*, 4> tbThemeDots{};       // 单选中心点（选中态绿色实心圆）
	Ling::Node* tbCustomStrip{ nullptr };
	std::array<Ling::Node*, 5> tbCustomSwatches{};  // 自定义色块内圆（外圈为描边色实心圆），5 个色槽

	// —— 软件外观 · 贴图边框 ——
	Ling::Node* pinPreview{ nullptr };        // 右侧效果预览框
	Ling::Button* pinColorSwatch{ nullptr };
	Ling::Label* pinColorHex{ nullptr };
	Ling::Label* pinWidthVal{ nullptr };
	Ling::Label* pinRadiusVal{ nullptr };
	// —— 软件外观 · 托盘图标 ——
	std::array<Ling::Button*, 4> trayRadios{};   // 单选外圈（实心圆 → 叠白色内圆成圆环）
	std::array<Ling::Node*, 4> trayDots{};       // 单选中心点（选中态绿色实心圆）
	Ling::Button* trayColorGlyph{ nullptr };     // 「自定义颜色」格：可点字形（点开系统取色器）
	Ling::ImageBox* trayCustomImg{ nullptr };    // 「自定义图标」格的图片预览
	Ling::Button* trayUploadBtn{ nullptr };      // 「自定义图标」格的「上传」按钮
};
