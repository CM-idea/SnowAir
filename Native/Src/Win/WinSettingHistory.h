#pragma once
#include <include/Ling.h>
#include <memory>
#include <string>
#include <vector>
#include "../Setting.h"

class Tip;
/**
 * 截图历史 (Section 2) —— 内容布局：
 *   - 顶部一行：左「全部时间 / 年月」筛选下拉，右「历史保留时间 + 天数」下拉
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
	// 按当前历史条目重建年月筛选项（条目增删/清空后月份集合会变）
	void rebuildFilter();
	// 已渲染内容盖不住「当前滚动位置 + 一屏 + 一行余量」时续补一批
	void fillCheck();
	void scheduleFillCheck();
	// 滚轮/拖滑块/拉窗口后：滚动偏移变了就重判是否要续补
	void maybeScrollGrew();
	// 光标停在哪张缩略图上，就浮出哪张的 复制/贴图/删除（其余隐藏）
	void updateHover(POINT pos);
	// 「复制成功」提示：铺在当前被复制的缩略图上（#0FDC78 底 + 居中文字），短暂停留
	void showCopyToast(Ling::Node* thumbHost);
	void hideCopyToast();
private:
	// 一张缩略图 + 叠在它上面的三个图标按钮（停悬显示用）
	struct CellSlot {
		Ling::Node* thumb{ nullptr };
		std::vector<Ling::Button*> btns;
	};
	Ling::Label* countLabel{ nullptr };
	Ling::ScrollerBox* gridScroll{ nullptr };   // 中部网格的滚动区（底行因此恒贴视口底部）
	Ling::Node* gridHost{ nullptr };
	Ling::Label* emptyLabel{ nullptr };
	Ling::Node* filterHost{ nullptr };      // 承载筛选下拉（选项随月份集合变化重建）
	std::wstring filterKey;                 // 选中的年月 key（空 = 全部时间）
	std::vector<std::wstring> filterKeys;   // 与下拉项一一对应

	// —— 分屏渲染状态 ——
	int shownCount{ 9 };                    // 目标渲染条数（只会随滚动增大）
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
	winrt::event_token wheelTok{}, moveTok{}, sizeTok{}, timerTok{};
};
