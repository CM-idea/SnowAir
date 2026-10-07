#include "pch.h"
#include <filesystem>
#include <algorithm>
#include "../Lang.h"
#include "../Setting.h"
#include "../History.h"
#include "../Util.h"
#include "../Tip.h"
#include "../Tool/IconCodes.h"
#include "WinSettingHistory.h"
#include "SettingTheme.h"
#include "SettingWidgets.h"

namespace {
	// 网格布局常量（3 列、每批 3 行 = 9 个）
	constexpr int kCols = 3;
	constexpr int kBatch = 9;
	constexpr float kGap = 10.f;
	constexpr float kPad = 8.f;
	constexpr float kThumbH = 104.f;
	constexpr float kCaptionH = 24.f;
	// 「复制成功」提示：用窗口定时器收起
	constexpr UINT kToastTimerId = 0x5A1A;
	constexpr UINT kToastMs = 1200;

	// 年月 key："yyyy-MM"（本地时间），空串 = 全部时间
	std::wstring monthKeyOf(long long ms)
	{
		time_t tt = (time_t)(ms / 1000);
		struct tm t {};
		localtime_s(&t, &tt);
		wchar_t buf[16]{};
		swprintf_s(buf, L"%04d-%02d", t.tm_year + 1900, t.tm_mon + 1);
		return buf;
	}
	std::wstring replaceOnce(std::wstring s, const std::wstring& from, const std::wstring& to)
	{
		auto pos = s.find(from);
		if (pos != std::wstring::npos) s.replace(pos, from.size(), to);
		return s;
	}
	// 下拉显示文案：空 key →「全部时间」，否则按 setting.historyMonthFmt 填充年/月
	std::wstring monthLabel(const std::wstring& key)
	{
		if (key.empty()) return Lang::get(L"setting.historyFilterAll");
		int year = 0, month = 0;
		swscanf_s(key.c_str(), L"%d-%d", &year, &month);
		std::wstring s = Lang::get(L"setting.historyMonthFmt");
		s = replaceOnce(s, L"%1", std::to_wstring(year));
		s = replaceOnce(s, L"%2", std::to_wstring(month));
		return s;
	}
	// 历史条目 → 来源文案
	std::wstring sourceLabel(const std::wstring& src)
	{
		if (src == L"fullscreen") return Lang::get(L"setting.srcFullscreen");
		if (src == L"focused")    return Lang::get(L"setting.srcFocused");
		return Lang::get(L"setting.srcCapture");
	}
	// 下拉外观对齐「插件集成 > 离线翻译」标题行里那个内嵌下拉：去掉描边 + 值文字改灰
	void stylePlainDdl(Ling::Button* ddl)
	{
		if (!ddl) return;
		ddl->setBorder(0.f, 0);
		// 值 Label 是按钮的第一个 Label 子节点（selectDdl 里先挂它，后挂图标）
		for (auto& c : ddl->children) {
			if (auto* lab = dynamic_cast<Ling::Label*>(c.get())) {
				lab->setColor(SettingTheme::textSecondary);
				break;
			}
		}
	}
	// 生成一个 26×26 的圆形浮层图标按钮（叠在缩略图右下角）
	Ling::Button* mkIconBtn(Ling::Node* parent, const wchar_t* glyph,
		float rightOffset, bool danger, std::function<void()> onClick)
	{
		auto* b = parent->makeChild<Ling::Button>();
		b->setPositionType(Ling::Position::Absolute);
		b->setPosition(Ling::Edge::Right, rightOffset);
		b->setPosition(Ling::Edge::Bottom, 6.f);
		b->setSize(26.f, 26.f);
		b->setPadding(0.f, 0.f, 0.f, 0.f);
		b->setBorderRadius(SettingTheme::radiusFull);
		b->setBorder(0.f, 0);
		b->setText(glyph);
		b->setFontFamily(Icon::Family);
		b->setFontSize(14.f);
		b->setColor(0xFFFFFFE1);
		b->setBg(0x00000078);
		b->setHoverBg(danger ? 0xFF383CD2u : 0x000000AAu);
		b->setHoverColor(0xFFFFFFFF);
		b->setFlexDirection(Ling::FlexDirection::Row);
		b->setJustifyContent(Ling::Justify::Center);
		b->setAlignItems(Ling::Align::Center);
		b->onClick.add([cb = std::move(onClick)](Ling::Button*) { cb(); });
		return b;
	}
}

static int retentionIdxOf(int days) {
	switch (days) {
	case 1: return 0;
	case 3: return 1;
	case 7: return 2;
	case 30: return 3;
	default: return 4; // -1 或 其他 → 永久
	}
}
static int retentionDaysOf(int idx) {
	static const int d[] = { 1,3,7,30,-1 };
	return d[idx < 0 || idx > 4 ? 4 : idx];
}

WinSettingHistory::WinSettingHistory(Ling::WinBase* parent) : Ling::Node(parent)
{
	// 本页整页不滚动（由 WinSetting 直接挂到视口容器，高度恒等于视口高度），
	// 只让中部网格自建滚动：底行因此始终贴住视口底部。
	setFlexDirection(Ling::FlexDirection::Column);

	// ---- 顶部一行：左「年月筛选」，右「历史保留时间 + 下拉」 ----
	auto* topRow = makeChild<Ling::Node>();
	topRow->setFlexDirection(Ling::FlexDirection::Row);
	topRow->setWidthPercent(100.f);
	topRow->setAlignItems(Ling::Align::Center);
	topRow->setMarginBottom(SettingTheme::blockGap);

	filterHost = topRow->makeChild<Ling::Node>();
	filterHost->setFlexDirection(Ling::FlexDirection::Row);
	filterHost->setAlignItems(Ling::Align::Center);
	filterHost->setFlexShrink(1.f);

	auto* spacer = topRow->makeChild<Ling::Node>();
	spacer->setFlexGrow(1.f);
	spacer->setFlexShrink(1.f);

	auto* retLab = topRow->makeChild<Ling::Label>();
	retLab->setText(Lang::get(L"setting.retention"));
	retLab->setFontSize(SettingTheme::fontBase);   // 与正文同字号
	retLab->setColor(SettingTheme::textMuted);
	retLab->setMarginRight(SettingTheme::sp2);

	const std::vector<std::wstring> retOpts = {
		Lang::get(L"setting.retention1d"),
		Lang::get(L"setting.retention3d"),
		Lang::get(L"setting.retention7d"),
		Lang::get(L"setting.retention30d"),
		Lang::get(L"setting.retentionForever"),
	};
	auto* retDdl = SettingUi::selectDdl(topRow, retOpts, retentionIdxOf(Setting::get()->getHistoryRetentionDays()),
		[](int i, const std::wstring&) {
			Setting::get()->setHistoryRetentionDays(retentionDaysOf(i));
			Setting::get()->pruneExpiredHistory();
		});
	retDdl->setWidth(120.f);   // 对齐「插件集成 > 离线翻译」内嵌下拉的宽度
	retDdl->setFlexShrink(0.f);
	stylePlainDdl(retDdl);

	// ---- 中部：空态 / 缩略图网格（互斥显示，同占顶行与底行之间的剩余空间）----
	// 空态直接作为中部区域的 flex 兄弟（flexGrow 占满），文字在其中上下居中。
	emptyLabel = makeChild<Ling::Label>();
	emptyLabel->setText(Lang::get(L"setting.noHistory"));
	emptyLabel->setFontSize(SettingTheme::fontBase);
	emptyLabel->setColor(SettingTheme::textMuted);
	emptyLabel->setWidthPercent(100.f);
	emptyLabel->setFlexGrow(1.f);
	emptyLabel->setFlexShrink(1.f);
	emptyLabel->setJustifyContent(Ling::Justify::Center);
	emptyLabel->setAlignItems(Ling::Align::Center);

	// 网格滚动区（内容溢出时只滚它）
	gridScroll = makeChild<Ling::ScrollerBox>();
	gridScroll->setWidthPercent(100.f);
	gridScroll->setFlexGrow(1.f);
	gridScroll->setFlexShrink(1.f);
	gridScroll->setScrollBarVisible(false);   // 不显示滚动条（滚轮仍可滚）
	// ScrollerBox::setChild 会把子节点挂到它的 content 上（滚动内容），下面直接往 gridScroll 里加。
	gridScroll->content->setFlexDirection(Ling::FlexDirection::Column);
	gridScroll->content->setWidthPercent(100.f);

	gridHost = gridScroll->makeChild<Ling::Node>();
	gridHost->setFlexDirection(Ling::FlexDirection::Column);
	gridHost->setWidthPercent(100.f);

	// ---- 底部一行：左「打开目录 / 清空历史」，右「当前条目」 ----
	auto* footer = makeChild<Ling::Node>();
	footer->setFlexDirection(Ling::FlexDirection::Row);
	footer->setWidthPercent(100.f);
	footer->setAlignItems(Ling::Align::Center);
	footer->setMarginTop(SettingTheme::blockGap);

	auto* openDir = SettingUi::btnOutline(footer, Lang::get(L"setting.openHistoryDir"), []() {
		auto dir = Setting::get()->getHistoryDir();
		std::filesystem::create_directories(dir);
		ShellExecute(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	});
	openDir->setMarginRight(SettingTheme::sp2);

	SettingUi::btnDestructive(footer, Lang::get(L"setting.clearHistory"), [this]() {
		SettingUi::ConfirmDialog::ask(win, Lang::get(L"setting.clearHistoryConfirm"),
			[this](bool ok) {
				if (!ok) return;
				Setting::get()->clearAllHistory();
				this->refreshGrid();
			});
	});

	auto* footerSpacer = footer->makeChild<Ling::Node>();
	footerSpacer->setFlexGrow(1.f);
	footerSpacer->setFlexShrink(1.f);

	countLabel = footer->makeChild<Ling::Label>();
	countLabel->setFontSize(SettingTheme::fontSm);
	countLabel->setColor(SettingTheme::textMuted);

	// 三个图标按钮的停悬提示：与截图工具栏同一套系统 tooltip（comctl32 TOOLTIPS_CLASS）
	tip = std::make_unique<Tip>(win);

	// 滚轮 / 拖滑块 / 拉窗口 → 重判要不要续补下一批
	// ⚠ 必须在本页的 gridScroll 建好之后再订阅：ScrollerBox 自己的滚轮处理先注册、
	//   先执行，这里读到的 getScrollY() 才是滚动后的新值。
	auto weak = getWeakThis();
	wheelTok = win->onMouseWheel.add([this, weak](POINT, float) {
		if (!weak.lock()) return;
		this->maybeScrollGrew();
		});
	moveTok = win->onMouseMove.add([this, weak](POINT pos) {
		if (!weak.lock()) return;
		this->updateHover(pos);     // 停悬才浮出 复制/贴图/删除
		this->maybeScrollGrew();
		});
	sizeTok = win->onSizeChanged.add([this, weak]() {
		if (!weak.lock()) return;
		this->scheduleFillCheck();
		});
	timerTok = win->onTimer.add([this, weak](UINT id) {
		if (!weak.lock()) return;
		if (id == kToastTimerId) this->hideCopyToast();
		});

	rebuildFilter();
	refreshGrid();
	// 内容区只有外层 content 提供底部留白：清掉末尾元素的 marginBottom，使底部与左右一致。
	SettingUi::trimTrailingGap(this);
}

WinSettingHistory::~WinSettingHistory()
{
	win->onMouseWheel.remove(wheelTok);
	win->onMouseMove.remove(moveTok);
	win->onSizeChanged.remove(sizeTok);
	win->onTimer.remove(timerTok);
	win->killTimer(kToastTimerId);
}

// 按当前历史条目重建年月筛选项（条目增删/清空后月份集合会变）
void WinSettingHistory::rebuildFilter()
{
	auto all = Setting::get()->getHistoryItems();

	std::vector<std::wstring> keys;
	keys.push_back(L"");                 // 首项 = 全部时间
	for (const auto& it : all) {         // all 为新→旧，月份天然降序
		auto k = monthKeyOf(it.createdAt);
		bool found = false;
		for (auto& e : keys) if (e == k) { found = true; break; }
		if (!found) keys.push_back(k);
	}
	// 选中的月份已无条目（被清空/过期清理）→ 退回「全部时间」
	{
		bool ok = false;
		for (auto& k : keys) if (k == filterKey) { ok = true; break; }
		if (!ok) filterKey.clear();
	}
	filterKeys = keys;

	if (!filterHost) return;
	filterHost->removeAllChildren();

	std::vector<std::wstring> labels;
	labels.reserve(keys.size());
	int cur = 0;
	for (size_t i = 0; i < keys.size(); ++i) {
		labels.push_back(monthLabel(keys[i]));
		if (keys[i] == filterKey) cur = (int)i;
	}
	auto* ddl = SettingUi::selectDdl(filterHost, labels, cur,
		[this](int i, const std::wstring&) {
			if (i < 0 || (size_t)i >= filterKeys.size()) return;
			filterKey = filterKeys[(size_t)i];
			shownCount = kBatch;         // 换筛选：从第一屏重新铺
			this->refreshGrid();
		});
	ddl->setWidth(SettingTheme::dropdownWidth);
	ddl->setFlexShrink(0.f);
	stylePlainDdl(ddl);
}

void WinSettingHistory::refreshGrid()
{
	Setting::get()->pruneExpiredHistory();
	auto all = Setting::get()->getHistoryItems();

	// 月份集合变化（条目增删/过期清理）→ 重建筛选项
	{
		std::vector<std::wstring> keys;
		keys.push_back(L"");
		for (const auto& it : all) {
			auto k = monthKeyOf(it.createdAt);
			bool found = false;
			for (auto& e : keys) if (e == k) { found = true; break; }
			if (!found) keys.push_back(k);
		}
		if (keys != filterKeys) {
			// 立即同步状态，延后重建下拉：refreshGrid 可能正由筛选下拉自身的回调触发，
			// 同步 removeAllChildren 会在事件回调中途销毁该按钮。
			filterKeys = keys;
			bool ok = false;
			for (auto& k : filterKeys) if (k == filterKey) { ok = true; break; }
			if (!ok) filterKey.clear();
			auto weak = getWeakThis();
			auto rebuild = [this, weak]() { if (weak.lock()) this->rebuildFilter(); };
			if (auto* app = Ling::App::get()) app->dq.TryEnqueue(rebuild);
			else rebuild();
		}
	}

	// 按当前筛选过滤
	std::vector<Setting::HistoryItem> items;
	for (const auto& it : all) {
		if (filterKey.empty() || monthKeyOf(it.createdAt) == filterKey)
			items.push_back(it);
	}
	const int total = (int)items.size();

	// 计数（右下「当前条目：%1」）
	if (countLabel) {
		std::wstring c = Lang::get(L"setting.currentCount");
		c = replaceOnce(c, L"%1", std::to_wstring(total));
		countLabel->setText(c);
	}

	if (!gridHost) return;

	const bool empty = items.empty();
	// 空态与网格互斥显示：两者都带 flexGrow(1)，必须一并收起另一个，否则会平分中部区域
	// 导致「暂无截图历史」只在中部上半段居中（视觉上偏上）。
	if (emptyLabel) empty ? emptyLabel->show() : emptyLabel->hide();
	if (gridScroll)  empty ? gridScroll->hide()  : gridScroll->show();
	if (empty) {
		gridHost->removeAllChildren();
		gridHost->hide();
		builtIds.clear();
		cells.clear();
		copyToast = nullptr;
		if (tip) tip->hide();       // 提示挂在将被拆掉的按钮上，一并收掉
		lastRow = nullptr; lastRowCells = 0; lastRowFillers.clear();
		shownCount = kBatch;
		return;
	}
	gridHost->show();

	// 分屏渲染：首屏一批，之后只随滚动增大
	if (shownCount <= 0) shownCount = kBatch;
	if (shownCount > total) shownCount = total;
	const int want = shownCount;

	// 需要整体重建？已建格子数超过 want（删条目/换筛选把目标压小），
	// 或前缀条目对不上（删一条后后面整体前移，格子里装的就不是 items[i] 了）
	bool rebuild = (int)builtIds.size() > want;
	if (!rebuild) {
		const size_t n = (std::min)(builtIds.size(), (size_t)want);
		for (size_t i = 0; i < n; ++i) {
			if (builtIds[i] != items[i].id) { rebuild = true; break; }
		}
	}
	if (rebuild) {
		gridHost->removeAllChildren();
		builtIds.clear();
		cells.clear();
		// 提示挂在将被拆掉的格子上，随格子一起销毁，这里一并作废
		copyToast = nullptr;
		if (tip) tip->hide();       // 提示挂在将被拆掉的按钮上，一并收掉
		lastRow = nullptr; lastRowCells = 0; lastRowFillers.clear();
	}

	// 续补前先撤掉末行的占位块
	if (lastRow && !lastRowFillers.empty()) {
		for (auto* f : lastRowFillers) if (f) lastRow->removeChild(f);
		lastRowFillers.clear();
	}

	for (int i = (int)builtIds.size(); i < want; ++i) {
		if (!lastRow || lastRowCells == kCols) {
			if (lastRow) lastRow->setMarginBottom(kGap);   // 上一行不再是末行，补回行距
			lastRow = gridHost->makeChild<Ling::Node>();
			lastRow->setFlexDirection(Ling::FlexDirection::Row);
			lastRow->setWidthPercent(100.f);
			lastRow->setMarginBottom(0.f);                 // 末行先不给间距
			lastRowCells = 0;
		}
		const bool lastCol = (lastRowCells == kCols - 1);
		const auto& it = items[i];

		auto* cell = lastRow->makeChild<Ling::Node>();
		cell->setWidthPercent(0.f);
		cell->setFlexGrow(1.f);
		cell->setFlexShrink(1.f);
		if (!lastCol) cell->setMarginRight(kGap);
		cell->setHeight(kThumbH + kPad * 2.f + kCaptionH);
		cell->setFlexDirection(Ling::FlexDirection::Column);
		cell->setBg(SettingTheme::card);
		cell->setBorder(1.f, SettingTheme::border);
		cell->setBorderRadius(SettingTheme::radiusLg);
		cell->setPadding(kPad, kPad, kPad, kPad);

		// 缩略图容器：右下角叠 复制/贴图/删除 图标按钮
		auto* thumbHost = cell->makeChild<Ling::Node>();
		thumbHost->setWidthPercent(100.f);
		thumbHost->setHeight(kThumbH);
		thumbHost->setFlexShrink(0.f);
		thumbHost->setBg(0x00000028);
		thumbHost->setBorderRadius(SettingTheme::radiusMd);
		thumbHost->setPositionType(Ling::Position::Relative);

		auto* img = thumbHost->makeChild<Ling::ImageBox>();
		img->setWidthPercent(100.f);
		img->setHeightPercent(100.f);
		img->loadImg(it.filePath);

		const std::wstring id = it.id;
		auto* copyBtn = mkIconBtn(thumbHost, Icon::Copy, 70.f, false,
			[this, id, thumbHost]() {
				Util::copyHistoryFileToClipboard(id);
				this->showCopyToast(thumbHost);   // 在当前这张缩略图上提示「复制成功」
			});
		auto* pinBtn = mkIconBtn(thumbHost, Icon::Pin, 38.f, false,
			[this, id]() { Util::pinHistoryFile(id); });
		auto* delBtn = mkIconBtn(thumbHost, Icon::Remove, 6.f, true,
			[this, id]() {
				SettingUi::ConfirmDialog::ask(win, Lang::get(L"setting.deleteHistoryConfirm"),
					[this, id](bool ok) {
						if (!ok) return;
						Setting::get()->removeHistoryItem(id);
						// 延后一拍重建：这个按钮自己就在要被拆掉的格子里，同步重建会在它自己的
						// 点击回调里把它销毁；
						auto weak = getWeakThis();
						auto reload = [this, weak]() { if (weak.lock()) this->refreshGrid(); };
						if (auto* app = Ling::App::get()) app->dq.TryEnqueue(reload);
						else reload();
					});
			});
		// 三个按钮默认收起，停悬到这张缩略图上才浮出
		copyBtn->hide(); pinBtn->hide(); delBtn->hide();
		cells.push_back({ thumbHost, { copyBtn, pinBtn, delBtn } });
		if (tip) {
			tip->bind(copyBtn, Lang::get(L"setting.copy"));
			tip->bind(pinBtn, Lang::get(L"setting.pin"));
			tip->bind(delBtn, Lang::get(L"setting.delete"));
		}

		// 元信息：MM-dd HH:mm · W×H · 来源
		wchar_t whenBuf[32]{};
		struct tm t {};
		time_t tt = (time_t)(it.createdAt / 1000);
		localtime_s(&t, &tt);
		wcsftime(whenBuf, 32, L"%m-%d %H:%M", &t);
		auto* meta = cell->makeChild<Ling::Label>();
		meta->setText(std::wstring(whenBuf) + L" · " + std::to_wstring(it.width) + L"×"
			+ std::to_wstring(it.height) + L" · " + sourceLabel(it.source));
		meta->setFontSize(SettingTheme::fontXs);
		meta->setColor(SettingTheme::textTertiary);
		meta->setWidthPercent(100.f);
		meta->setMarginTop(6.f);
		meta->setJustifyContent(Ling::Justify::Start);
		meta->setAlignItems(Ling::Align::FlexStart);

		lastRowCells++;
		builtIds.push_back(it.id);
	}

	// 末行不足一列：补等宽占位块，保持列宽一致（续补时会先撤掉）
	if (lastRow && lastRowCells > 0 && lastRowCells < kCols) {
		for (int c = lastRowCells; c < kCols; ++c) {
			auto* filler = lastRow->makeChild<Ling::Node>();
			filler->setWidthPercent(0.f);
			filler->setFlexGrow(1.f);
			filler->setFlexShrink(1.f);
			if (c != kCols - 1) filler->setMarginRight(kGap);
			lastRowFillers.push_back(filler);
		}
	}

	scheduleFillCheck();
}

void WinSettingHistory::scheduleFillCheck()
{
	if (fillScheduled) return;
	fillScheduled = true;
	auto weak = getWeakThis();
	auto run = [this, weak]() {
		if (!weak.lock()) return;
		this->fillScheduled = false;
		this->fillCheck();
	};
	// 延后一拍：refreshGrid 可能正由下拉/删除按钮的回调触发，不能在回调里同步重建整片网格
	if (auto* app = Ling::App::get()) app->dq.TryEnqueue(run);
	else run();
}

void WinSettingHistory::fillCheck()
{
	if (!gridScroll || !gridHost) return;

	auto all = Setting::get()->getHistoryItems();
	int total = 0;
	for (const auto& it : all)
		if (filterKey.empty() || monthKeyOf(it.createdAt) == filterKey) ++total;
	if (total <= shownCount) return;

	// 判据用「自己算得出来的几何」，不依赖刚重建、还没重排的 content 高度：
	// 已渲染内容是否盖住「当前滚动位置 + 一屏 + 一行余量」，盖住就停。
	const int rows = (shownCount + kCols - 1) / kCols;
	const float dpi = win->dpi;
	const float cellH = (kThumbH + kPad * 2.f + kCaptionH) * dpi;
	const float gap = kGap * dpi;
	const float contentH = rows > 0 ? rows * cellH + (rows - 1) * gap : 0.f;
	const float need = gridScroll->getScrollY() + gridScroll->h + cellH;
	if (contentH >= need) return;

	shownCount = (std::min)(total, shownCount + kBatch);
	refreshGrid();
}

void WinSettingHistory::maybeScrollGrew()
{
	if (!gridScroll) return;
	const float y = gridScroll->getScrollY();
	if (y == lastScrollY) return;   // 鼠标动了但没滚动：忽略
	lastScrollY = y;
	scheduleFillCheck();
}

// 停悬到哪张缩略图上，就浮出哪张的 复制/贴图/删除；其余全部收起
void WinSettingHistory::updateHover(POINT pos)
{
	for (auto& c : cells) {
		if (!c.thumb) continue;
		const bool inside = c.thumb->isPosIn(pos);
		for (auto* b : c.btns) {
			if (!b) continue;
			if (inside) b->show();
			else b->hide();
		}
	}
}

// 「复制成功」提示：在当前被复制的那张缩略图**正中**放一个小胶囊徽章（#0FDC78 底），
// 不全盖住图
void WinSettingHistory::showCopyToast(Ling::Node* thumbHost)
{
	if (!thumbHost) return;
	hideCopyToast();                 // 连点复制不叠加，先撤上一个
	// 透明全铺层只负责把徽章摆到正中；它不拦点击 —— 三个图标按钮都是各自订阅
	// win->onMouseDown 再 isPosIn 判定，不会被这层盖住
	auto* layer = thumbHost->makeChild<Ling::Node>();
	layer->setPositionType(Ling::Position::Absolute);
	layer->setPosition(Ling::Edge::Left, 0.f);
	layer->setPosition(Ling::Edge::Top, 0.f);
	layer->setPosition(Ling::Edge::Right, 0.f);
	layer->setPosition(Ling::Edge::Bottom, 0.f);
	layer->setBg(0);
	layer->setJustifyContent(Ling::Justify::Center);
	layer->setAlignItems(Ling::Align::Center);

	auto* badge = layer->makeChild<Ling::Label>();
	badge->setText(Lang::get(L"about.copySuccess"));
	badge->setFontSize(SettingTheme::fontSm);
	badge->setColor(0xFFFFFFFF);
	badge->setBg(0x0FDC78FF);        // #0FDC78
	badge->setPadding(10.f, 5.f, 10.f, 5.f);
	badge->setBorderRadius(SettingTheme::radiusFull);
	badge->setJustifyContent(Ling::Justify::Center);
	badge->setAlignItems(Ling::Align::Center);

	copyToast = layer;
	win->setTimer(kToastMs, kToastTimerId);
}

void WinSettingHistory::hideCopyToast()
{
	win->killTimer(kToastTimerId);
	// 注意：格子只在「整体重建 / 空态」里被 removeAllChildren 掉，那两条路径都会把
	// copyToast 置空，所以这里指针不会悬空。
	if (copyToast && copyToast->parent)
		copyToast->parent->removeChild(copyToast);
	copyToast = nullptr;
}
