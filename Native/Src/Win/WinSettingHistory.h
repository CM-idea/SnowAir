#pragma once
#include <include/Ling.h>
#include <memory>
#include <string>
#include <vector>
#include "../Setting.h"
#include "SettingWidgets.h"

class Tip;
/**
 * 截图历史 (Section 2) —— 内容布局：
 *   - 顶部一行：左「全部/截图/录屏」导航，最右筛选按钮（气泡面板：近7天/近30天/选择时间/搜索/标签）
 *   - 中部：缩略图 3 列网格（每格 = 缩略图 + 叠在图右下角的 复制/贴图/删除 图标按钮 + 元信息）
 *   - 底部一行：左「打开历史目录 / 清空历史」，右「当前条目」
 *   - 分屏渲染：首屏 3×3，滚到底再续补下一批
 */
class WinSettingHistory : public Ling::Node
{
public:
	WinSettingHistory(Ling::WinBase* parent);
	~WinSettingHistory();
private:
	void refreshGrid();
	// 已渲染内容盖不住「当前滚动位置 + 一屏 + 一行余量」时续补一批
	void fillCheck();
	void scheduleFillCheck();
	// 滚轮/拖滑块/拉窗口后：滚动偏移变了就重判是否要续补
	void maybeScrollGrew();
	// 光标停在哪张缩略图上，就浮出哪张的 复制/贴图/删除（其余隐藏）
	void updateHover(POINT pos);
	// 当前导航分类（0 全部 / 1 截图 / 2 录屏）是否包含该条目
	bool matchSourceTab(const Setting::HistoryItem& it) const;
	// 导航分类 + 时间范围 + 标签 + 关键词（全部叠加）
	bool matchFilters(const Setting::HistoryItem& it) const;
	// 筛选气泡：页内绘制、箭头指向筛选按钮
	void toggleFilterPanel();
	void closeFilterPanel(bool keepMode = false);
	void setFilterBtnActive(bool on);
	void rebuildFilterPanel();
	// 页内模态：标签编辑
	void showTagDialog(const Setting::HistoryItem& it);
	void closeModal();
	Ling::Node* makeModalCard(float width, const std::wstring& title);
	// 点击外部 / Esc：收起筛选面板或页内弹窗（pos 传 {INT_MAX,INT_MAX} 表示 Esc）
	void closeTransientUi(POINT pos);
	// 有筛选气泡 / 页内弹窗时，下层卡片不应再响应点击（Ling 的 Button 只按几何命中，不判遮挡）
	bool overlayOpen() const { return filterPanel != nullptr || modal != nullptr || dismissingClick; }
	// 「复制成功」提示：铺在当前被复制的缩略图上（#0FDC78 底 + 居中文字），短暂停留
	void showCopyToast(Ling::Node* thumbHost);
	void hideCopyToast();
private:
	// 一张缩略图 + 叠在它上面的三个图标按钮（停悬显示用）
	struct CellSlot {
		Ling::Node* thumb{ nullptr };
		std::vector<Ling::Button*> btns;
	};
	// 当前筛选条件
	struct HistoryFilter {
		int timeMode{ 0 };              // 0 全部 / 1 近7天 / 2 近30天 / 3 自定义区间 / 4 今天
		long long rangeStart{ 0 };      // ms（含），timeMode=3 时有效
		long long rangeEnd{ 0 };        // ms（含）
		std::wstring tag;               // 空 = 不限
		std::wstring keyword;           // 文件名或标签包含
	};
	Ling::Label* countLabel{ nullptr };
	Ling::ScrollerBox* gridScroll{ nullptr };   // 中部网格的滚动区（底行因此恒贴视口底部）
	Ling::Node* gridHost{ nullptr };
	Ling::Label* emptyLabel{ nullptr };
	Ling::Node* filterHost{ nullptr };      // 承载筛选按钮
	Ling::Button* filterBtn{ nullptr };
	Ling::Node* filterPanel{ nullptr };     // 打开中的筛选气泡（页内绝对定位）
	Ling::Node* filterDim{ nullptr };       // 气泡打开时整页压暗（气泡是它的子节点）
	Ling::Node* topRowDim{ nullptr };       // 顶行压暗层（排在筛选按钮之前，按钮因此保持高亮）
	int panelMode{ 0 };                     // 0 首页（搜索+标签）/ 1 时间区间
	Ling::Node* modal{ nullptr };           // 页内模态（标签编辑）
	Ling::Node* modalCard{ nullptr };       // 模态里的卡片（点卡片外 = 关闭）
	HistoryFilter filter;
	SettingUi::SegmentedPillsRef navPills;  // 顶部导航：全部 / 截图 / 录屏
	int sourceTab{ 0 };                     // 0 全部 / 1 截图 / 2 录屏

	// —— 分屏渲染状态 ——
	int shownCount{ 9 };                    // 目标渲染条数（只会随滚动增大）
	bool forceGridRebuild{ false };         // 下一次 refreshGrid 强制重建格子（标签改动等）
	bool fillScheduled{ false };
	float lastScrollY{ -1.f };
	Ling::Node* lastRow{ nullptr };         // 当前最后一行（续补时往里加格子）
	int lastRowCells{ 0 };
	std::vector<Ling::Node*> lastRowFillers; // 末行占位块（续补前先移除）
	std::vector<std::wstring> builtIds;      // 已建格子对应的条目 id（判断要不要整体重建）
	// —— 停悬浮出按钮 / 复制成功提示 ——
	std::vector<CellSlot> cells;             // 已建好的缩略图槽（悬停切换三个按钮的显隐）
	Ling::Node* copyToast{ nullptr };        // 当前「复制成功」提示（被复制的缩略图上）
	std::unique_ptr<Tip> tip;                // 三个图标按钮的停悬提示（系统 tooltip，同截图工具栏）
	winrt::event_token wheelTok{}, moveTok{}, sizeTok{}, timerTok{}, focusTok{}, mouseDownTok{}, keyTok{}, modalKeyTok{};
	bool modalKeyBound{ false };
	// 本次按下正是「点外部收起气泡/弹窗」的那一下：同一次按下里不要再触发下层卡片
	bool dismissingClick{ false };
};
