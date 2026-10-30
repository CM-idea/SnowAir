#pragma once
#include <include/Ling.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "../Setting.h"
#include "SettingWidgets.h"

class Tip;
class ShotPreviewWindow;   // 停悬大图预览（定义在 WinSettingHistory.cpp）
/**
 * 历史记录 (Section 2) —— 内容布局：
 *   - 顶部一行：左「全部/截图/录屏/其他/剪切板」导航，最右筛选按钮（气泡面板：近7天/近30天/选择时间/搜索/标签）
 *   - 中部：截图/录屏用缩略图 3 列网格；剪切板用行列表
 *   - 底部一行：左「打开历史目录 / 清空…」，右「当前条目」
 *   - 分屏渲染：首屏 3×3，滚到底再续补下一批
 */
class WinSettingHistory : public Ling::Node
{
public:
	WinSettingHistory(Ling::WinBase* parent);
	~WinSettingHistory();
private:
	struct CellSlot;   // 下面才定义；showPreview 的形参先用到它
	void refreshGrid();
	// 当前分类的完整条目列表：文件历史 + （全部/剪切板页）剪切板条目，按时间倒序
	std::vector<Setting::HistoryItem> collectItems(bool forceRescan);
	// 已渲染内容盖不住「当前滚动位置 + 一屏 + 一行余量」时续补一批
	void fillCheck();
	void scheduleFillCheck();
	// 滚轮/拖滑块/拉窗口后：滚动偏移变了就重判是否要续补
	void maybeScrollGrew();
	// 光标停在哪张缩略图上，就浮出哪张的 复制/贴图/删除（其余隐藏）
	void updateHover(POINT pos);
	// 当前导航分类（0 全部 / 1 截图 / 2 录屏 / 3 其他 / 4 剪切板）是否包含该条目
	bool matchSourceTab(const Setting::HistoryItem& it) const;
	// 分类本身的归属（不看标签筛选）：「全部」里只有用自定义标签筛选时才把「其他/剪切板」带出来
	bool baseTabMatch(const Setting::HistoryItem& it) const;
	// 导航分类 + 时间范围 + 标签 + 关键词（全部叠加）
	bool matchFilters(const Setting::HistoryItem& it) const;
	// 筛选气泡：页内绘制、箭头指向筛选按钮
	void toggleFilterPanel();
	void closeFilterPanel(bool keepMode = false);
	void setFilterBtnActive(bool on);
	void rebuildFilterPanel();
	// 页内模态：标签编辑
	void showTagDialog(const Setting::HistoryItem& it);
	// 通用标签编辑：key = 标签存储键（文件历史用路径，剪切板用条目 id）
	void showTagDialogFor(const std::wstring& key, std::vector<std::wstring> tags,
		std::function<void(const std::vector<std::wstring>&)> save);
	void closeModal();
	Ling::Node* makeModalCard(float width, const std::wstring& title);
	// 点击外部 / Esc：收起筛选面板或页内弹窗（pos 传 {INT_MAX,INT_MAX} 表示 Esc）
	void closeTransientUi(POINT pos);
	// 有筛选气泡 / 页内弹窗时，下层卡片不应再响应点击（Ling 的 Button 只按几何命中，不判遮挡）
	bool overlayOpen() const { return filterPanel != nullptr || modal != nullptr || dismissingClick; }
	// 「复制成功」提示：铺在当前被复制的缩略图上（#0FDC78 底 + 居中文字），短暂停留
	void showCopyToast(Ling::Node* thumbHost);
	void hideCopyToast();
	// 停悬大图预览：停在有图的卡片上弹出，移开立即收起
	void showPreview(const CellSlot& slot);
	void hidePreview();
	// 复制后延迟置顶：先亮「复制成功」，过一会儿再把条目挪到最前并淡入网格
	void scheduleReorderToTop(const std::wstring& id);
	void applyReorderToTop();
	void fadeInGrid();
private:
	// 一张缩略图 + 叠在它上面的三个图标按钮（停悬显示用）
	struct CellSlot {
		Ling::Node* thumb{ nullptr };
		std::vector<Ling::Button*> btns;
		// 剪切板图片的滚动容器：指针停悬在它上面时，禁用外层网格的滚轮，避免两层同时滚
		Ling::ScrollerBox* imgScroll{ nullptr };
		// 剪切板文字预览：文字超出时同样独占滚轮
		Ling::TextBox* textScroll{ nullptr };
		// 停悬预览：图片卡给路径 + 尺寸；文字卡给全文；文件卡给文件列表
		std::wstring previewPath;
		int previewW{ 0 }, previewH{ 0 };
		std::wstring previewText;
		std::vector<std::wstring> previewFiles;
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
	SettingUi::SegmentedPillsRef navPills;  // 顶部导航：全部 / 截图 / 录屏 / 其他 / 剪切板
	int sourceTab{ 0 };                     // 0 全部 / 1 截图 / 2 录屏 / 3 其他 / 4 剪切板
	Ling::Button* clearBtn{ nullptr };      // 底部「清空…」：文案随导航切换
	Ling::Button* openDirBtn{ nullptr };    // 底部「打开历史目录」：剪切板分类隐藏（只存路径，没有本地目录）

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
	std::unique_ptr<ShotPreviewWindow> preview_;   // 停悬大图预览（懒创建）
	int previewIndex_{ -1 };                 // 当前弹出预览的卡片下标（-1 = 没弹）
	std::wstring pendingReorderId_;          // 等待置顶的剪切板条目 id
	// 「我们自己产出的文件」→ 封面图路径（小写路径为键）。剪贴板里复制本软件的录屏时，
	// 卡片直接复用媒体历史的封面，不再显示文件名。collectItems 时重建。
	std::unordered_map<std::wstring, std::wstring> ourMediaCover_;
	std::unique_ptr<Tip> tip;                // 三个图标按钮的停悬提示（系统 tooltip，同截图工具栏）
	winrt::event_token wheelTok{}, moveTok{}, sizeTok{}, timerTok{}, focusTok{}, mouseDownTok{}, keyTok{}, modalKeyTok{}, cursorTok{}, blurTok{};
	bool modalKeyBound{ false };
	// 本次按下正是「点外部收起气泡/弹窗」的那一下：同一次按下里不要再触发下层卡片
	bool dismissingClick{ false };
};
