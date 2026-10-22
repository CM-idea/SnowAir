#include "pch.h"
#include <filesystem>
#include <algorithm>
#include <chrono>
#include <cwctype>
#include "../Lang.h"
#include "../Setting.h"
#include "../History.h"
#include "../Util.h"
#include "../Tip.h"
#include "../Tool/IconCodes.h"
#include "../Tool/ToolbarChrome.h"
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

	std::wstring replaceOnce(std::wstring s, const std::wstring& from, const std::wstring& to)
	{
		auto pos = s.find(from);
		if (pos != std::wstring::npos) s.replace(pos, from.size(), to);
		return s;
	}
	std::wstring lowerCopy(std::wstring s)
	{
		for (auto& c : s) c = (wchar_t)towlower(c);
		return s;
	}
	long long nowMs()
	{
		return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
	}
	// "YYYY-MM-DD" → 当天 0 点 / 23:59:59.999 的毫秒时间戳（本地时区）
	bool parseDateRange(const std::wstring& startText, const std::wstring& endText,
		long long& startMs, long long& endMs)
	{
		int y1 = 0, m1 = 0, d1 = 0, y2 = 0, m2 = 0, d2 = 0;
		if (swscanf_s(startText.c_str(), L"%d-%d-%d", &y1, &m1, &d1) != 3) return false;
		if (swscanf_s(endText.c_str(), L"%d-%d-%d", &y2, &m2, &d2) != 3) return false;
		struct tm a{};
		a.tm_year = y1 - 1900; a.tm_mon = m1 - 1; a.tm_mday = d1;
		struct tm b{};
		b.tm_year = y2 - 1900; b.tm_mon = m2 - 1; b.tm_mday = d2;
		b.tm_hour = 23; b.tm_min = 59; b.tm_sec = 59;
		startMs = (long long)mktime(&a) * 1000;
		endMs = (long long)mktime(&b) * 1000 + 999;
		return startMs > 0 && endMs > 0 && startMs <= endMs;
	}
	std::wstring dateText(long long ms)
	{
		time_t tt = (time_t)(ms / 1000);
		struct tm t{};
		localtime_s(&t, &tt);
		wchar_t buf[16]{};
		wcsftime(buf, 16, L"%Y-%m-%d", &t);
		return buf;
	}
	// 气泡外壳：圆角 + 指向筛选按钮的小箭头（复用工具栏属性栏那套绘制）
	class FilterBubble : public Ling::Canvas
	{
	public:
		FilterBubble(Ling::WinBase* w) : Ling::Canvas(w)
		{
			// 每次刷新都会重绘：用双缓冲后端，避免「清屏 → 重画」的中间态被合成器采样到而闪烁
			enableSwapChain();
		}
		void configure(bool tipDown, float arrowX)
		{
			tipDown_ = tipDown;
			arrowX_ = arrowX;
			if (win) win->refresh();
		}
	protected:
		void layout() override
		{
			Ling::Canvas::layout();
			auto ctx = startPaint();
			if (!ctx) return;
			ctx->Clear(0);
			const float d = (win && win->dpi > 0.f) ? win->dpi : 1.f;
			Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> bg;
			// 气泡底与描边跟随软件主题（与设置页浮层同色），不沿用截图工具栏配色
			ctx->CreateSolidColorBrush(Ling::Color(SettingTheme::popover).getD2DColor(), bg.GetAddressOf());
			ToolbarChrome::paintPropBubble(ctx, w, h, d, bg.Get(), arrowX_, tipDown_,
				ToolbarTheme::borderWidth, SettingTheme::border);
			finishPaint();
		}
	private:
		bool tipDown_{ false };
		float arrowX_{ 0.f };
	};
	// 日历切月箭头：复用「下拉」字形旋转 90°，与其它图标字重一致
	// （Composition 的 CenterPoint 是视觉局部坐标，得等 layout 拿到真实 w/h 再设）
	struct ChevronNavBtn : Ling::Button
	{
		explicit ChevronNavBtn(Ling::WinBase* w) : Ling::Button(w) {}
		void setAngle(float a) { angle_ = a; }
	protected:
		void layout() override
		{
			Ling::Button::layout();
			visual.CenterPoint({ w * 0.5f, h * 0.5f, 0.f });
			visual.RotationAngleInDegrees(angle_);
		}
	private:
		float angle_{ 0.f };
	};
	// 历史条目 → 来源文案
	std::wstring sourceLabel(const std::wstring& src)
	{
		if (src == L"fullscreen") return Lang::get(L"setting.srcFullscreen");
		if (src == L"focused")    return Lang::get(L"setting.srcFocused");
		if (src == L"long")       return Lang::get(L"setting.srcLong");
		if (src == L"record")     return Lang::get(L"setting.srcRecord");
		if (src == L"autosave")   return Lang::get(L"setting.srcAutoSave");
		return Lang::get(L"setting.srcCapture");
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
	// 在资源管理器里定位文件（录屏条目用：视频不复制到历史目录，定位的是用户自己保存的那份）
	void revealInExplorer(const std::wstring& path)
	{
		if (path.empty()) return;
		const std::wstring args = L"/select,\"" + path + L"\"";
		ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
	}
}

WinSettingHistory::WinSettingHistory(Ling::WinBase* parent) : Ling::Node(parent)
{
	// 本页整页不滚动（由 WinSetting 直接挂到视口容器，高度恒等于视口高度），
	// 只让中部网格自建滚动：底行因此始终贴住视口底部。
	setFlexDirection(Ling::FlexDirection::Column);

	// ---- 顶部一行：导航（全部/截图/录屏） + 时间筛选（贴最右） ----
	auto* topRow = makeChild<Ling::Node>();
	topRow->setFlexDirection(Ling::FlexDirection::Row);
	topRow->setWidthPercent(100.f);
	topRow->setAlignItems(Ling::Align::Center);
	topRow->setMarginBottom(SettingTheme::blockGap);

	navPills = SettingUi::segmentedPills(topRow,
		{ Lang::get(L"setting.historyTabAll"), Lang::get(L"setting.historyTabShots"),
		  Lang::get(L"setting.historyTabRecord") },
		sourceTab,
		[this](int idx) {
			sourceTab = idx;
			shownCount = kBatch;
			this->closeFilterPanel();
			this->closeModal();
			this->refreshGrid();
		});
	if (navPills.row) navPills.row->setFlexShrink(0.f);
	// 三段等宽：以侧栏「功能设置」四个字的文本宽度为准
	{
		float textW = 0.f;
		if (auto layout = Ling::D2D::get()->makeTextLayout(
			Lang::get(L"setting.function"), SettingTheme::fontBase * win->dpi)) {
			DWRITE_TEXT_METRICS m{};
			layout->GetMetrics(&m);
			textW = m.width / (win->dpi > 0.f ? win->dpi : 1.f);
		}
		if (textW <= 0.f) textW = SettingTheme::fontBase * 4.f;
		const float pillW = std::ceil(textW) + SettingTheme::sp4 * 2.f;
		for (auto* b : navPills.btns) {
			b->setWidth(pillW);
			b->setFlexShrink(0.f);
		}
	}

	// 中间弹性空白：把时间筛选推到最右
	auto* spacer = topRow->makeChild<Ling::Node>();
	spacer->setFlexGrow(1.f);
	spacer->setFlexShrink(1.f);

	// 顶行压暗层：盖住导航等，但排在筛选按钮之前 → 按钮始终在它上面
	topRowDim = topRow->makeChild<Ling::Node>();
	topRowDim->setPositionType(Ling::Position::Absolute);
	topRowDim->setPosition(Ling::Edge::Left, 0.f);
	topRowDim->setPosition(Ling::Edge::Top, 0.f);
	topRowDim->setPosition(Ling::Edge::Right, 0.f);
	topRowDim->setPosition(Ling::Edge::Bottom, 0.f);
	topRowDim->setBg(0x00000059);
	topRowDim->hide();

	filterHost = topRow->makeChild<Ling::Node>();
	filterHost->setFlexDirection(Ling::FlexDirection::Row);
	filterHost->setAlignItems(Ling::Align::Center);
	filterHost->setFlexShrink(0.f);

	// 筛选按钮：点击展开页内气泡面板（不再使用下拉）
	filterBtn = SettingUi::btnOutline(filterHost, Icon::Filter, [this]() { this->toggleFilterPanel(); });
	filterBtn->setFontFamily(Icon::Family);
	filterBtn->setFontSize(16.f);
	filterBtn->setWidth(SettingTheme::ctrlH);
	filterBtn->setHeight(SettingTheme::ctrlH);
	filterBtn->setPadding(0.f, 0.f, 0.f, 0.f);
	filterBtn->setFlexShrink(0.f);
	filterBtn->setColor(SettingTheme::textMuted);
	filterBtn->setHoverColor(SettingTheme::textPrimary);

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
		auto dir = Util::outputDir();
		if (dir.empty()) return;
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		ShellExecute(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	});
	openDir->setMarginRight(SettingTheme::sp2);

	SettingUi::btnDestructive(footer, Lang::get(L"setting.clearHistory"), [this]() {
		// 提示文案跟随当前导航分类
		const wchar_t* msgKey = sourceTab == 2 ? L"setting.clearHistoryConfirmRecord"
			: (sourceTab == 1 ? L"setting.clearHistoryConfirmShots" : L"setting.clearHistoryConfirmAll");
		SettingUi::ConfirmDialog::ask(win, Lang::get(msgKey),
			[this](bool ok) {
				if (!ok) return;
				Setting::get()->clearAllHistory(sourceTab);
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
	// 气泡 / 弹窗盖住卡片时不冒停悬提示
	tip->suppressed = [this]() { return this->overlayOpen(); };
	if (filterBtn) tip->bind(filterBtn, Lang::get(L"setting.filter"));

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
	// 窗口重新获得焦点：重扫目录。用户可能刚在资源管理器里从回收站还原了截图，
	// 回到设置窗就应该能在历史里看到（历史以目录里的真实文件为准）。
	focusTok = win->onFocus.add([this, weak]() {
		if (!weak.lock()) return;
		auto rescan = [this, weak]() { if (weak.lock()) this->refreshGrid(); };
		if (auto* app = Ling::App::get()) app->dq.TryEnqueue(rescan);
		else rescan();
		});
	// 点击面板/弹窗以外的地方 → 收起；Esc 同样收起
	mouseDownTok = win->onMouseDown.add([this, weak](POINT pos, bool) {
		if (!weak.lock()) return;
		this->dismissingClick = false;
		this->closeTransientUi(pos);
		});
	keyTok = win->onKeyDown.add([this, weak](UINT key) {
		if (!weak.lock()) return;
		if (key == VK_ESCAPE) this->closeTransientUi({ INT_MAX, INT_MAX });
		});

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
	win->onFocus.remove(focusTok);
	win->onMouseDown.remove(mouseDownTok);
	win->onKeyDown.remove(keyTok);
	win->killTimer(kToastTimerId);
}

// 当前导航分类（0 全部 / 1 截图 / 2 录屏）是否包含该条目
bool WinSettingHistory::matchSourceTab(const Setting::HistoryItem& it) const
{
	if (sourceTab == 0) return true;
	const bool isRecord = (it.source == L"record");
	return sourceTab == 2 ? isRecord : !isRecord;
}

// 导航分类 + 时间范围 + 标签 + 关键词，全部叠加生效
bool WinSettingHistory::matchFilters(const Setting::HistoryItem& it) const
{
	if (!matchSourceTab(it)) return false;
	const long long now = nowMs();
	if (filter.timeMode == 1 && it.createdAt < now - 7LL * 24 * 3600 * 1000) return false;
	if (filter.timeMode == 2 && it.createdAt < now - 30LL * 24 * 3600 * 1000) return false;
	if (filter.timeMode == 4) {
		// 今天：本地时间当天 0 点之后
		time_t tt = (time_t)(now / 1000);
		struct tm t{};
		localtime_s(&t, &tt);
		t.tm_hour = 0;
		t.tm_min = 0;
		t.tm_sec = 0;
		const long long startOfDay = (long long)mktime(&t) * 1000;
		if (it.createdAt < startOfDay) return false;
	}
	if (filter.timeMode == 3) {
		if (filter.rangeStart > 0 && it.createdAt < filter.rangeStart) return false;
		if (filter.rangeEnd > 0 && it.createdAt > filter.rangeEnd) return false;
	}
	if (!filter.tag.empty()) {
		bool hit = false;
		for (auto& t : Setting::get()->getHistoryTags(it.filePath))
			if (_wcsicmp(t.c_str(), filter.tag.c_str()) == 0) { hit = true; break; }
		if (!hit) return false;
	}
	if (!filter.keyword.empty()) {
		const std::wstring kw = lowerCopy(filter.keyword);
		bool hit = lowerCopy(std::filesystem::path(it.filePath).filename().wstring()).find(kw) != std::wstring::npos;
		if (!hit) {
			for (auto& t : Setting::get()->getHistoryTags(it.filePath))
				if (lowerCopy(t).find(kw) != std::wstring::npos) { hit = true; break; }
		}
		if (!hit) return false;
	}
	return true;
}

void WinSettingHistory::toggleFilterPanel()
{
	if (filterPanel) { closeFilterPanel(); return; }
	rebuildFilterPanel();
}

// 筛选按钮的选中态：面板展开时用停悬底色 + 绿色图标
void WinSettingHistory::setFilterBtnActive(bool on)
{
	if (!filterBtn) return;
	filterBtn->setColor(on ? SettingTheme::success : SettingTheme::textMuted);
	filterBtn->setHoverColor(on ? SettingTheme::success : SettingTheme::textPrimary);
	filterBtn->setBg(on ? SettingTheme::accent : SettingTheme::card);
	filterBtn->setHoverBg(SettingTheme::accent);
}

void WinSettingHistory::closeFilterPanel(bool keepMode)
{
	setFilterBtnActive(false);
	if (topRowDim) topRowDim->hide();
	if (!keepMode) panelMode = 0;
	if (filterDim) {
		if (filterDim->parent) filterDim->parent->removeChild(filterDim);
		filterDim = nullptr;
	}
	if (!filterPanel) return;
	if (filterPanel->parent) filterPanel->parent->removeChild(filterPanel);
	filterPanel = nullptr;
}

// 在筛选按钮正下方（放不下就翻到上方）挂一个页内气泡面板
void WinSettingHistory::rebuildFilterPanel()
{
	closeFilterPanel(true);   // 保留 panelMode（区间模式重建时不回落）
	if (!filterBtn) return;
	setFilterBtnActive(true);
	if (tip) tip->hide();     // 已经挂着的停悬提示先收掉
	// 气泡内所有配色跟随软件主题（不使用截图工具栏主题）
	const uint32_t tFg = SettingTheme::textPrimary;
	const uint32_t tHover = SettingTheme::accent;
	const uint32_t tActive = SettingTheme::brand;   // 标签选中态
	const uint32_t fgFaint = (tFg & 0xFFFFFF00u) | 0x66u;
	// 日历格：今天 = 次级底 + 主文字；选中 = 主色底 + 主色前景（深/浅主题各自成立）
	const uint32_t todayBg = SettingTheme::secondary;
	const uint32_t todayFg = SettingTheme::textPrimary;
	const uint32_t selBg = SettingTheme::primary;
	const uint32_t selFg = SettingTheme::primaryForeground;

	const float d = win->dpi > 0.f ? win->dpi : 1.f;
	const float pageW = w / d;
	const float pageH = h / d;
	const float caret = ToolbarTheme::caretSize;
	const float panelW = 320.f;
	const float panelH = 370.f;          // 固定高度（列表 / 日历两种模式一致）
	const float gap = 4.f;

	// Node 的 x/y 已是窗口内绝对坐标（physical）；换算成相对本页的逻辑坐标
	const float bx = (filterBtn->x - x) / d;
	const float by = (filterBtn->y - y) / d;
	const float bw = filterBtn->w / d;
	const float bh = filterBtn->h / d;

	// 面板右缘与筛选按钮右缘对齐（向左展开），箭头仍指向按钮中心
	float px = bx + bw - panelW;
	float py = by + bh + gap;
	bool tipDown = false;                // false：箭头在顶部（面板在按钮下方）
	if (py + panelH > pageH - 2.f) {     // 下方放不下 → 翻到按钮上方
		tipDown = true;
		py = by - gap - panelH;
	}
	px = (std::max)(2.f, (std::min)(px, pageW - panelW - 2.f));
	py = (std::max)(2.f, (std::min)(py, pageH - panelH - 2.f));

	// 压暗层：整页压暗；顶行整条由 topRowDim 压暗（按钮在它上层，保持高亮）
	{
		float rowX = 0.f, rowY = 0.f, rowW = pageW, rowH = 0.f;
		if (filterHost && filterHost->parent) {
			Ling::Node* topRow = filterHost->parent;
			rowX = (topRow->x - x) / d;
			rowY = (topRow->y - y) / d;
			rowW = topRow->w / d;
			rowH = topRow->h / d;
		}
		auto* dim = makeChild<Ling::Node>();
		dim->setPositionType(Ling::Position::Absolute);
		dim->setPosition(Ling::Edge::Left, 0.f);
		dim->setPosition(Ling::Edge::Top, 0.f);
		dim->setSize(pageW, pageH);
		filterDim = dim;

		auto addBand = [&](float bx0, float by0, float bw0, float bh0) {
			if (bw0 <= 0.f || bh0 <= 0.f) return;
			auto* n = dim->makeChild<Ling::Node>();
			n->setPositionType(Ling::Position::Absolute);
			n->setPosition(Ling::Edge::Left, bx0);
			n->setPosition(Ling::Edge::Top, by0);
			n->setSize(bw0, bh0);
			n->setBg(0x00000059);
		};
		addBand(0.f, 0.f, pageW, rowY);                               // 顶行上方留白
		addBand(0.f, rowY + rowH, pageW, pageH - (rowY + rowH));      // 顶行下方
		addBand(0.f, rowY, rowX, rowH);                               // 顶行左侧留白
		addBand(rowX + rowW, rowY, pageW - (rowX + rowW), rowH);      // 顶行右侧留白
		if (topRowDim) topRowDim->show();
	}

	auto* panel = makeChild<Ling::Node>();
	panel->setPositionType(Ling::Position::Absolute);
	panel->setPosition(Ling::Edge::Left, px);
	panel->setPosition(Ling::Edge::Top, py);
	panel->setSize(panelW, panelH);
	filterPanel = panel;

	auto* bubble = panel->makeChild<FilterBubble>();
	bubble->setPositionType(Ling::Position::Absolute);
	bubble->setSize(panelW, panelH);
	const float arrowX = (bx + bw * 0.5f - px) * d;   // 物理像素
	bubble->configure(tipDown, arrowX);

	const float pad = 12.f;             // 气泡内边距（四周统一）
	auto* content = panel->makeChild<Ling::Node>();
	content->setPositionType(Ling::Position::Absolute);
	content->setPosition(Ling::Edge::Left, pad);
	content->setPosition(Ling::Edge::Right, pad);
	content->setPosition(Ling::Edge::Top, 0.f);
	content->setPosition(Ling::Edge::Bottom, 0.f);
	content->setFlexDirection(Ling::FlexDirection::Column);
	const float padTop = tipDown ? pad : caret + pad;
	const float padBottom = tipDown ? caret + pad : pad;
	content->setPadding(0.f, padTop, 0.f, padBottom);

	// 时间快捷：近7天 / 近30天 / 选择时间
	auto* timeRow = content->makeChild<Ling::Node>();
	timeRow->setFlexDirection(Ling::FlexDirection::Row);
	timeRow->setAlignItems(Ling::Align::Center);
	timeRow->setFlexWrap(Ling::Wrap::Wrap);
	timeRow->setWidthPercent(100.f);
	timeRow->setMarginBottom(10.f);

	// 两套内容（默认列表 / 区间输入）都常驻，只做显隐切换，避免整块重建导致闪烁
	struct PanelSections {
		Ling::Node* list{ nullptr };
		Ling::Node* range{ nullptr };
		Ling::Node* rangeBtns{ nullptr };
		Ling::Button* pick{ nullptr };
	};
	auto sec = std::make_shared<PanelSections>();
	auto calOpen = std::make_shared<bool>(false);   // 日历是否展开（展开时日期框与按钮都让位）
	auto applyMode = [this, sec, calOpen, tHover, tActive, tFg]() {
		const bool range = (this->panelMode == 1);
		auto toggle = [](Ling::Node* n, bool show) {
			if (!n) return;
			if (show) n->show(); else n->hide();
		};
		toggle(sec->list, !range);
		toggle(sec->range, range);
		// 日历展开时取消/确认也让位，别又冒出来
		toggle(sec->rangeBtns, range && !*calOpen);
		if (sec->pick) {
			// 选中态：停悬底色 + 正常文字色（不再变绿）
			sec->pick->setBg(range ? tHover : 0);
			sec->pick->setColor(tFg);
			sec->pick->setHoverColor(tFg);
		}
		// show/hide 只先改可见性，坐标要等下一次 WM_PAINT 的布局才更新；
		// 这里同步跑一遍布局，避免被隐藏节点的旧坐标（0,0）先被合成器采样到而闪一下
		if (this->win) this->win->layout();
	};

	auto mkTimeBtn = [&](const std::wstring& text, int mode) {
		auto* b = timeRow->makeChild<Ling::Button>();
		b->setText(text);
		b->setFontSize(SettingTheme::fontSm);
		b->setHeight(SettingTheme::ctrlH);   // 32，与「恢复默认」按钮同高
		b->setPadding(10.f, 0.f, 10.f, 0.f);
		b->setBorderRadius(ToolbarTheme::hoverRadius);
		b->setBorder(0.f, 0);
		b->setBg(0);
		b->setHoverBg(tHover);
		b->setColor(tFg);
		b->setHoverColor(tFg);
		b->setJustifyContent(Ling::Justify::Center);
		b->setAlignItems(Ling::Align::Center);
		b->setMarginRight(6.f);
		b->setFlexShrink(0.f);
		b->onClick.add([this, mode, applyMode](Ling::Button*) {
			if (mode == 3) {
				// 复用气泡：分隔线以下切成日期区间输入（不弹新窗）；再次点击退回
				this->panelMode = (this->panelMode == 1) ? 0 : 1;
				applyMode();
				return;
			}
			// 面板会连本按钮一起销毁 → 动作延后一拍执行
			auto weak = getWeakThis();
			filter.timeMode = mode;
			shownCount = kBatch;
			auto apply = [this, weak]() {
				if (!weak.lock()) return;
				this->closeFilterPanel();
				this->refreshGrid();
			};
			if (auto* app = Ling::App::get()) app->dq.TryEnqueue(apply);
			else apply();
		});
		if (mode == 3) sec->pick = b;
	};
	mkTimeBtn(Lang::get(L"setting.filterToday"), 4);
	mkTimeBtn(Lang::get(L"setting.filter7d"), 1);
	mkTimeBtn(Lang::get(L"setting.filter30d"), 2);
	mkTimeBtn(Lang::get(L"setting.filterPickTime"), 3);

	// 分隔线
	auto* line1 = content->makeChild<Ling::Node>();
	line1->setHeight(1.f);
	line1->setWidthPercent(100.f);
	line1->setBg(SettingTheme::border);   // 与筛选按钮描边同色
	line1->setMarginBottom(10.f);

	// 区间输入（「选择时间」展开）：并列起止日期 + 日历 + 取消/确认
	auto* rangeSec = content->makeChild<Ling::Node>();
	rangeSec->setFlexDirection(Ling::FlexDirection::Column);
	rangeSec->setWidthPercent(100.f);
	rangeSec->setFlexGrow(1.f);
	rangeSec->setFlexShrink(1.f);
	sec->range = rangeSec;
	{
		// 没选过日期时留空，只显示「开始日期 / 结束日期」占位提示
		const std::wstring startText = (filter.timeMode == 3 && filter.rangeStart > 0)
			? dateText(filter.rangeStart) : L"";
		const std::wstring endText = (filter.timeMode == 3 && filter.rangeEnd > 0)
			? dateText(filter.rangeEnd) : L"";

		// 起止日期：并列两个只读输入框，点按其中一个在下方展开日历
		auto* dateRow = rangeSec->makeChild<Ling::Node>();
		dateRow->setFlexDirection(Ling::FlexDirection::Row);
		dateRow->setWidthPercent(100.f);
		dateRow->setMarginBottom(8.f);

		// 日期选择框：用按钮做外观（文字天然水平居中），点一下展开日历
		auto mkDateBox = [&](const std::wstring& value, const std::wstring& hint) {
			auto* b = dateRow->makeChild<Ling::Button>();
			b->setHeight(SettingTheme::ctrlH);
			b->setFlexGrow(1.f);
			b->setFlexShrink(1.f);
			b->setBorderRadius(ToolbarTheme::hoverRadius);
			b->setBorder(0.f, 0);
			b->setBg(tHover);
			b->setHoverBg(tHover);
			b->setFontSize(SettingTheme::fontSm);
			b->setPadding(10.f, 0.f, 10.f, 0.f);
			b->setJustifyContent(Ling::Justify::Center);
			b->setAlignItems(Ling::Align::Center);
			b->setText(value.empty() ? hint : value);
			b->setColor(value.empty() ? fgFaint : tFg);
			b->setHoverColor(value.empty() ? fgFaint : tFg);
			return b;
		};
		auto* startBox = mkDateBox(startText, Lang::get(L"setting.filterStartDate"));
		startBox->setMarginRight(8.f);
		auto* endBox = mkDateBox(endText, Lang::get(L"setting.filterEndDate"));

		// 日历容器：点输入框时填充并显示
		auto* calHost = rangeSec->makeChild<Ling::Node>();
		calHost->setFlexDirection(Ling::FlexDirection::Column);
		calHost->setWidthPercent(100.f);
		calHost->setFlexGrow(1.f);
		calHost->setFlexShrink(1.f);
		calHost->setMarginTop(2.f);   // 与分隔线留 12（分隔线自带 10 的下边距）
		calHost->hide();

		struct CalState { int field{ 0 }; int year{ 0 }; int month{ 0 }; };
		auto cal = std::make_shared<CalState>();
		// 日历控件只建一次：切月只刷新文字 / 配色，不重建按钮，鼠标停悬状态才不会丢
		struct CalUi {
			Ling::Button* yearBtn{ nullptr };
			Ling::Button* monthBtn{ nullptr };
			std::vector<Ling::Button*> cells;       // 42 个日期格
			std::vector<Ling::Node*> dayRows;       // 6 行日期容器
			Ling::Node* monthHost{ nullptr };       // 月份网格
			Ling::Node* yearHost{ nullptr };        // 年份网格
			std::vector<Ling::Button*> monthCells;
			std::vector<Ling::Button*> yearCells;
			Ling::Button* nextNav{ nullptr };       // 右箭头（到未来时禁用）
			int view{ 0 };                          // 0 天 / 1 月 / 2 年
			int yearPage{ 0 };                      // 年份网格起始年
			int pickedYear{ -1 };                   // 本会话里用户点过的年份（-1 = 还没点）
			int pickedMonth{ -1 };                  // 本会话里用户点过的月份（-1 = 还没点）
		};
		auto ui = std::make_shared<CalUi>();
		auto updateCal = std::make_shared<std::function<void()>>();
		auto buildCal = std::make_shared<std::function<void()>>();
		auto btnRowRef = std::make_shared<Ling::Node*>(nullptr);   // 日历展开时先让位

		// 展开某个字段的日历：初始月份取该字段已有日期，否则今天
		auto openCal = [this, cal, calHost, ui, buildCal, updateCal, btnRowRef, dateRow, startBox, endBox, calOpen](int field) {
			cal->field = field;
			int y = 0, m = 0, d = 0;
			const std::wstring t = (field == 0) ? startBox->getText() : endBox->getText();
			if (swscanf_s(t.c_str(), L"%d-%d-%d", &y, &m, &d) == 3) {
				cal->year = y;
				cal->month = m - 1;
			}
			else {
				time_t tt = (time_t)(nowMs() / 1000);
				struct tm tn{};
				localtime_s(&tn, &tt);
				cal->year = tn.tm_year + 1900;
				cal->month = tn.tm_mon;
			}
			ui->view = 0;   // 每次展开都从日期视图开始
			ui->pickedYear = -1;   // 重新展开：年份回到「默认态」，只有当前年带底色
			ui->pickedMonth = -1;  // 月份同理
			if (!ui->yearBtn) (*buildCal)();
			else (*updateCal)();
			calHost->show();
			// 日历展开期间：日期框整行与取消/确认都收起，整块空间留给日历
			*calOpen = true;
			dateRow->hide();
			if (*btnRowRef) (*btnRowRef)->hide();
			if (this->win) this->win->layout();
		};
		startBox->onClick.add([openCal](Ling::Button*) { openCal(0); });
		endBox->onClick.add([openCal](Ling::Button*) { openCal(1); });

		// 按当前年月刷新标题 / 日期格 / 月份格 / 年份格（只改文字与配色，不动控件）
		*updateCal = [this, cal, ui, startBox, endBox, tFg, tHover, fgFaint,
			todayBg, todayFg, selBg, selFg]() {
			const int y = cal->year, m = cal->month;

			wchar_t yb[32]{};
			swprintf_s(yb, Lang::get(L"setting.calYearFmt").c_str(), y);
			ui->yearBtn->setText(yb);
			wchar_t mb[32]{};
			swprintf_s(mb, Lang::get(L"setting.calMonthFmt").c_str(), m + 1);
			ui->monthBtn->setText(mb);

			// 标题的选中态：打开哪个网格，对应标题就保持底色
			ui->yearBtn->setBg(ui->view == 2 ? tHover : 0);
			ui->monthBtn->setBg(ui->view == 1 ? tHover : 0);

			// 视图显隐：天 / 月 / 年
			auto show = [](Ling::Node* n, bool on) {
				if (!n) return;
				if (on) n->show(); else n->hide();
			};
			for (auto* row : ui->dayRows) show(row, ui->view == 0);
			show(ui->monthHost, ui->view == 1);
			show(ui->yearHost, ui->view == 2);

			time_t nowT = (time_t)(nowMs() / 1000);
			struct tm tn{};
			localtime_s(&tn, &nowT);

			// 月份网格：默认态只有当前月带底色；用户点过的月份才显示选中态
			for (int i = 0; i < (int)ui->monthCells.size(); ++i) {
				auto* b = ui->monthCells[i];
				wchar_t tb[16]{};
				swprintf_s(tb, Lang::get(L"setting.calMonthFmt").c_str(), i + 1);
				b->setText(tb);
				const bool isThisMonth = (y == tn.tm_year + 1900 && i == tn.tm_mon);
				const bool future = (y > tn.tm_year + 1900)
					|| (y == tn.tm_year + 1900 && i > tn.tm_mon);
				const bool sel = (ui->pickedMonth == i);
				b->setBg(sel ? selBg : (isThisMonth ? todayBg : 0));
				b->setHoverBg(sel ? selBg
					: (isThisMonth ? todayBg : (future ? 0 : tHover)));
				if (sel) b->setColor(selFg);
				else if (isThisMonth) b->setColor(todayFg);
				else b->setColor(future ? fgFaint : tFg);
				b->setHoverColor(sel ? selFg
					: (isThisMonth ? todayFg : (future ? fgFaint : tFg)));
			}

			// 年份网格：默认态只有当前年带底色；用户点过的年份才显示选中态
			for (int i = 0; i < (int)ui->yearCells.size(); ++i) {
				const int yy = ui->yearPage + i;
				auto* b = ui->yearCells[i];
				wchar_t tb[16]{};
				swprintf_s(tb, L"%d", yy);
				b->setText(tb);
				const bool future = (yy > tn.tm_year + 1900);
				const bool isThisYear = (yy == tn.tm_year + 1900);
				const bool sel = (ui->pickedYear == yy);
				b->setBg(sel ? selBg : (isThisYear ? todayBg : 0));
				b->setHoverBg(sel ? selBg : (isThisYear ? todayBg : (future ? 0 : tHover)));
				if (sel) b->setColor(selFg);
				else if (isThisYear) b->setColor(todayFg);
				else b->setColor(future ? fgFaint : tFg);
				b->setHoverColor(sel ? selFg
					: (isThisYear ? todayFg : (future ? fgFaint : tFg)));
			}

			// 不能往后翻到未来：右箭头置灰且点击无效
			bool canNext = false;
			const int curY = tn.tm_year + 1900, curM = tn.tm_mon;
			if (ui->view == 2) canNext = (ui->yearPage + 12) <= curY;
			else if (ui->view == 1) canNext = (y < curY);
			else canNext = (y < curY) || (y == curY && m < curM);
			if (ui->nextNav) {
				ui->nextNav->setColor(canNext ? tFg : fgFaint);
				ui->nextNav->setHoverColor(canNext ? tFg : fgFaint);
				ui->nextNav->setHoverBg(canNext ? tHover : 0);
				if (!canNext) ui->nextNav->setBg(0);   // 正停悬时立刻撤掉底色
			}

			if (ui->view != 0) {
				if (this->win) this->win->layout();
				return;
			}

			struct tm base{};
			base.tm_year = y - 1900;
			base.tm_mon = m;
			base.tm_mday = 1;
			base.tm_hour = 12;
			mktime(&base);                          // 归一化，同时拿到 1 号是周几
			const int lead = (base.tm_wday + 6) % 7;

			int selY = 0, selM = 0, selD = 0;
			const std::wstring cur = (cal->field == 0) ? startBox->getText() : endBox->getText();
			swscanf_s(cur.c_str(), L"%d-%d-%d", &selY, &selM, &selD);

			for (int i = 0; i < (int)ui->cells.size(); ++i) {
				struct tm td = base;
				td.tm_mday = i - lead + 1;
				mktime(&td);
				const int day = td.tm_mday;
				const bool inMonth = (td.tm_mon == m);
				const bool isToday = (td.tm_year == tn.tm_year && td.tm_mon == tn.tm_mon && day == tn.tm_mday);
				const bool selected = (selD > 0 && td.tm_year == selY - 1900
					&& td.tm_mon == selM - 1 && day == selD);

				auto* cell = ui->cells[i];
				cell->setText(std::to_wstring(day));
				// 今天之后的日子还没到：弱化且无停悬
				const bool future = (td.tm_year > tn.tm_year)
					|| (td.tm_year == tn.tm_year && td.tm_mon > tn.tm_mon)
					|| (td.tm_year == tn.tm_year && td.tm_mon == tn.tm_mon && day > tn.tm_mday);
				// 选中：主色底 + 主色前景；今天：次级底 + 主文字；其余透明 + 停悬底色
				const uint32_t cellBg = selected ? selBg : (isToday ? todayBg : 0);
				const uint32_t cellFg = selected ? selFg
					: (isToday ? todayFg : ((inMonth && !future) ? tFg : fgFaint));
				cell->setBg(cellBg);
				cell->setHoverBg(selected || isToday || future ? cellBg : tHover);
				cell->setColor(cellFg);
				cell->setHoverColor(cellFg);
			}
			if (this->win) this->win->layout();
		};

		*buildCal = [this, cal, ui, updateCal, calHost, calOpen, btnRowRef, dateRow,
			startBox, endBox, tFg, tHover]() {
			// 头部：‹ 年月 ›
			auto* head = calHost->makeChild<Ling::Node>();
			head->setFlexDirection(Ling::FlexDirection::Row);
			head->setAlignItems(Ling::Align::Center);
			head->setWidthPercent(100.f);
			head->setHeight(32.f);
			head->setMarginBottom(2.f);

			// 切月 / 切年 / 翻年份页（看当前视图）；到未来就不给再往后翻
			auto shift = [this, cal, ui, updateCal](int dir) {
				if (dir > 0) {
					time_t nowT = (time_t)(nowMs() / 1000);
					struct tm tn{};
					localtime_s(&tn, &nowT);
					const int curY = tn.tm_year + 1900, curM = tn.tm_mon;
					bool canNext = false;
					if (ui->view == 2) canNext = (ui->yearPage + 12) <= curY;
					else if (ui->view == 1) canNext = (cal->year < curY);
					else canNext = (cal->year < curY) || (cal->year == curY && cal->month < curM);
					if (!canNext) return;
				}
				if (ui->view == 2) {
					ui->yearPage += 12 * dir;
				}
				else if (ui->view == 1) {
					cal->year += dir;
				}
				else {
					int mm = cal->month + dir;
					while (mm < 0) { mm += 12; cal->year -= 1; }
					while (mm > 11) { mm -= 12; cal->year += 1; }
					cal->month = mm;
				}
				(*updateCal)();
			};
			auto mkNav = [&](float angle, std::function<void()> fn) {
				auto* b = head->makeChild<ChevronNavBtn>();
				b->setAngle(angle);
				b->setText(Icon::Dropdown);
				b->setFontFamily(Icon::Family);
				b->setFontSize(18.f);
				b->setSize(36.f, 36.f);
				b->setPadding(0.f, 0.f, 0.f, 0.f);
				b->setBorderRadius(ToolbarTheme::hoverRadius);
				b->setBorder(0.f, 0);
				b->setBg(0);
				b->setHoverBg(tHover);
				b->setColor(tFg);
				b->setHoverColor(tFg);
				b->setJustifyContent(Ling::Justify::Center);
				b->setAlignItems(Ling::Align::Center);
				b->onClick.add([fn](Ling::Button*) { fn(); });
				return b;
			};
			// 箭头中心与日期网格首列 / 末列的中心对齐（网格 7 列 38 宽、SpaceEvenly 均分，间距约 4）
			auto* prevNav = mkNav(90.f, [shift]() { shift(-1); });
			prevNav->setMarginLeft(5.f);
			prevNav->setMarginRight(4.f);

			auto* titleWrap = head->makeChild<Ling::Node>();
			titleWrap->setFlexGrow(1.f);
			titleWrap->setFlexShrink(1.f);
			titleWrap->setFlexDirection(Ling::FlexDirection::Row);
			titleWrap->setJustifyContent(Ling::Justify::Center);
			titleWrap->setAlignItems(Ling::Align::Center);

			// 年份 / 月份都可点：点开对应的选择网格
			auto mkTitle = [&]() {
				auto* b = titleWrap->makeChild<Ling::Button>();
				b->setFontSize(SettingTheme::fontBase);
				b->setFontWeight(600);
				b->setColor(tFg);
				b->setHoverColor(tFg);
				b->setBg(0);
				b->setHoverBg(tHover);
				b->setHeight(28.f);
				b->setPadding(6.f, 0.f, 6.f, 0.f);
				b->setBorder(0.f, 0);
				b->setBorderRadius(ToolbarTheme::hoverRadius);
				b->setJustifyContent(Ling::Justify::Center);
				b->setAlignItems(Ling::Align::Center);
				return b;
			};
			auto* yearBtn = mkTitle();
			yearBtn->setMarginRight(2.f);
			yearBtn->onClick.add([this, cal, ui, updateCal](Ling::Button*) {
				ui->view = 2;
				ui->yearPage = cal->year - 9;   // 最近 12 年：9 年过去 + 当年 + 2 年未来
				(*updateCal)();
			});
			ui->yearBtn = yearBtn;

			auto* monthBtn = mkTitle();
			monthBtn->onClick.add([this, ui, updateCal](Ling::Button*) {
				ui->view = 1;
				(*updateCal)();
			});
			ui->monthBtn = monthBtn;

			auto* nextNav = mkNav(-90.f, [shift]() { shift(1); });
			nextNav->setMarginRight(5.f);
			ui->nextNav = nextNav;

			// 日期网格：周一起算；固定 6 行（多出来的补下个月），
			// 行数固定行距才稳定，横竖间距才能一致
			for (int r = 0; r < 6; ++r) {
				auto* row = calHost->makeChild<Ling::Node>();
				row->setFlexDirection(Ling::FlexDirection::Row);
				row->setWidthPercent(100.f);
				row->setFlexGrow(1.f);
				row->setFlexShrink(1.f);
				row->setJustifyContent(Ling::Justify::SpaceEvenly);
				row->setAlignItems(Ling::Align::Center);
				ui->dayRows.push_back(row);
				for (int c = 0; c < 7; ++c) {
					auto* cell = row->makeChild<Ling::Button>();
					cell->setSize(38.f, 38.f);                  // 方形底块（列间距约 4）
					cell->setFlexShrink(0.f);
					cell->setFontSize(SettingTheme::fontBase);
					cell->setPadding(0.f, 0.f, 0.f, 0.f);
					cell->setBorderRadius(8.f);                 // 与时间按钮 / 停悬块同一档圆角
					cell->setBorder(0.f, 0);
					cell->setJustifyContent(Ling::Justify::Center);
					cell->setAlignItems(Ling::Align::Center);
					const int idx = r * 7 + c;
					cell->onClick.add([this, cal, calHost, btnRowRef, dateRow, startBox, endBox, calOpen, tFg, idx](Ling::Button*) {
						struct tm b{};
						b.tm_year = cal->year - 1900;
						b.tm_mon = cal->month;
						b.tm_mday = 1;
						b.tm_hour = 12;
						mktime(&b);
						const int lead = (b.tm_wday + 6) % 7;
						struct tm td = b;
						td.tm_mday = idx - lead + 1;
						std::wstring picked = dateText((long long)mktime(&td) * 1000);
						// 未来日期不能选，回落到今天
						const std::wstring todayStr = dateText(nowMs());
						if (picked > todayStr) picked = todayStr;
						if (cal->field == 0) startBox->setText(picked);
						else endBox->setText(picked);
						// 日期用正常色，不再是占位灰
						startBox->setColor(tFg);
						startBox->setHoverColor(tFg);
						endBox->setColor(tFg);
						endBox->setHoverColor(tFg);
						// 收起日历，恢复日期框与取消/确认
						*calOpen = false;
						calHost->hide();
						dateRow->show();
						if (*btnRowRef) (*btnRowRef)->show();
						if (this->win) this->win->layout();
					});
					ui->cells.push_back(cell);
				}
			}

			// 月份 / 年份网格：3 列 × 4 行，和日期区占据同一块空间
			auto buildMini = [&](std::vector<Ling::Button*>& out, Ling::Node*& host,
				std::function<void(int)> onPick) {
				host = calHost->makeChild<Ling::Node>();
				host->setFlexDirection(Ling::FlexDirection::Column);
				host->setWidthPercent(100.f);
				host->setFlexGrow(1.f);
				host->setFlexShrink(1.f);
				host->hide();
				for (int r = 0; r < 4; ++r) {
					auto* row = host->makeChild<Ling::Node>();
					row->setFlexDirection(Ling::FlexDirection::Row);
					row->setWidthPercent(100.f);
					row->setFlexGrow(1.f);
					row->setFlexShrink(1.f);
					row->setAlignItems(Ling::Align::Center);
					for (int c = 0; c < 3; ++c) {
						auto* b = row->makeChild<Ling::Button>();
						b->setFlexGrow(1.f);
						b->setMargin(4.f);
						b->setHeight(38.f);          // 与日期格同高的底块
						b->setPadding(0.f, 0.f, 0.f, 0.f);
						b->setBorder(0.f, 0);
						b->setBorderRadius(8.f);
						b->setFontSize(SettingTheme::fontBase);
						b->setJustifyContent(Ling::Justify::Center);
						b->setAlignItems(Ling::Align::Center);
						const int idx = r * 3 + c;
						b->onClick.add([onPick, idx](Ling::Button*) { onPick(idx); });
						out.push_back(b);
					}
				}
			};
			buildMini(ui->monthCells, ui->monthHost, [this, cal, ui, updateCal](int idx) {
				// 未来的月份不可选
				time_t nowT = (time_t)(nowMs() / 1000);
				struct tm tn{};
				localtime_s(&tn, &nowT);
				if (cal->year > tn.tm_year + 1900
					|| (cal->year == tn.tm_year + 1900 && idx > tn.tm_mon)) {
					ui->view = 0;   // 点未来月份只收起月份选择，不改月份
					(*updateCal)();
					return;
				}
				cal->month = idx;
				ui->pickedMonth = idx;   // 记住用户点过的月份 → 显示选中态
				ui->view = 0;
				(*updateCal)();
			});
			buildMini(ui->yearCells, ui->yearHost, [this, cal, ui, updateCal](int idx) {
				const int yy = ui->yearPage + idx;
				// 未来的年份不可选
				time_t nowT = (time_t)(nowMs() / 1000);
				struct tm tn{};
				localtime_s(&tn, &nowT);
				if (yy > tn.tm_year + 1900) {
					ui->view = 0;   // 点未来年份只收起年份选择，不改年份
					(*updateCal)();
					return;
				}
				cal->year = yy;
				ui->pickedYear = yy;   // 记住用户点过的年份 → 显示选中态
				ui->pickedMonth = -1;  // 换了年份，之前的月份选中态作废
				ui->view = 0;   // 只选年份，直接回日期视图
				(*updateCal)();
			});
			(*updateCal)();
		};

		// 取消 / 确认固定在气泡右下角，不随内容流排版（距底 12）
		auto* btnRow = content->makeChild<Ling::Node>();
		btnRow->setPositionType(Ling::Position::Absolute);
		btnRow->setPosition(Ling::Edge::Right, 0.f);
		btnRow->setPosition(Ling::Edge::Bottom, padBottom);
		btnRow->setFlexDirection(Ling::FlexDirection::Row);
		btnRow->setAlignItems(Ling::Align::Center);
		sec->rangeBtns = btnRow;
		*btnRowRef = btnRow;

		auto weakR = getWeakThis();
		// 取消：在「选择时间」里只退出日期区间选择，回到列表；气泡保持展开。
		// 不在区间模式时（理论上按钮不可见）才收起整个气泡。
		auto deferCancel = [this, weakR, applyMode, calOpen]() {
			auto run = [this, weakR, applyMode, calOpen]() {
				if (!weakR.lock()) return;
				if (this->panelMode == 1) {
					this->panelMode = 0;
					*calOpen = false;
					applyMode();
					return;
				}
				this->closeFilterPanel();
			};
			if (auto* app = Ling::App::get()) app->dq.TryEnqueue(run);
			else run();
		};
		auto deferApplyRange = [this, weakR]() {
			auto run = [this, weakR]() {
				if (!weakR.lock()) return;
				this->closeFilterPanel();
				this->refreshGrid();
			};
			if (auto* app = Ling::App::get()) app->dq.TryEnqueue(run);
			else run();
		};
		SettingUi::btnOutline(btnRow, Lang::get(L"setting.cancel"), deferCancel, 72.f)
			->setMarginRight(8.f);
		SettingUi::btnPrimary(btnRow, Lang::get(L"setting.confirm"),
			[this, startBox, endBox, deferApplyRange]() {
				long long s = 0, e = 0;
				if (!parseDateRange(startBox->getText(), endBox->getText(), s, e)) return;
				filter.timeMode = 3;
				filter.rangeStart = s;
				filter.rangeEnd = e;
				shownCount = kBatch;
				deferApplyRange();
			}, 72.f);
	}

	// 默认：搜索框 + 自定义标签
	auto* listSec = content->makeChild<Ling::Node>();
	listSec->setFlexDirection(Ling::FlexDirection::Column);
	listSec->setWidthPercent(100.f);
	listSec->setFlexGrow(1.f);
	listSec->setFlexShrink(1.f);
	sec->list = listSec;

	// 搜索框（文件名 / 标签）
	auto* search = listSec->makeChild<Ling::TextBox>();
	search->setHeight(30.f);
	search->setWidthPercent(100.f);
	search->setBorderRadius(ToolbarTheme::hoverRadius);
	search->setBorder(0.f, 0);
	search->setBg(tHover);
	search->setColor(tFg);
	search->setFontSize(SettingTheme::fontSm);
	search->setPadding(8.f, 0.f, 8.f, 0.f);
	search->setVerticalCenter(true);
	search->setPlaceholder(Lang::get(L"setting.filterSearchHint"));
	search->setPlaceholderColor(fgFaint);
	search->setMarginBottom(10.f);
	search->setText(filter.keyword);
	search->onTextChanged.add([this](Ling::TextBox*, const std::wstring& t) {
		if (filter.keyword == t) return;
		filter.keyword = t;
		shownCount = kBatch;
		this->refreshGrid();
	});

	// 自定义标签：只列当前导航分类（全部 / 截图 / 录屏）下的条目标签
	std::vector<std::wstring> tags;
	for (auto& it : Setting::get()->getHistoryItemsForUi()) {
		if (!matchSourceTab(it)) continue;
		for (auto& t : Setting::get()->getHistoryTags(it.filePath)) {
			bool dup = false;
			for (auto& x : tags) if (_wcsicmp(x.c_str(), t.c_str()) == 0) { dup = true; break; }
			if (!dup) tags.push_back(t);
		}
	}
	std::sort(tags.begin(), tags.end());
	// 选中的标签已不存在（文件被删/标签被清空）→ 退回不筛选
	if (!filter.tag.empty()) {
		bool exists = false;
		for (auto& t : tags) if (_wcsicmp(t.c_str(), filter.tag.c_str()) == 0) { exists = true; break; }
		if (!exists) filter.tag.clear();
	}
	if (tags.empty()) {
		auto* none = listSec->makeChild<Ling::Label>();
		none->setText(Lang::get(L"setting.filterNoTags"));
		none->setFontSize(SettingTheme::fontSm);
		none->setColor(fgFaint);
		none->setMarginLeft(8.f);   // 与搜索框内的文字左对齐
		applyMode();
		return;
	}

	// 有标签时直接铺标签列表（无分组标题）
	auto* tagScroll = listSec->makeChild<Ling::ScrollerBox>();
	tagScroll->setWidthPercent(100.f);
	tagScroll->setFlexGrow(1.f);
	tagScroll->setFlexShrink(1.f);
	tagScroll->setScrollBarVisible(false);
	tagScroll->content->setFlexDirection(Ling::FlexDirection::Row);
	tagScroll->content->setFlexWrap(Ling::Wrap::Wrap);
	tagScroll->content->setAlignItems(Ling::Align::FlexStart);
	tagScroll->content->setWidthPercent(100.f);

	auto tagBtns = std::make_shared<std::vector<std::pair<Ling::Button*, std::wstring>>>();
	auto syncTagBtns = [this, tagBtns, tFg, tActive, tHover]() {
			for (auto& [b, tag] : *tagBtns) {
				if (!b) continue;
				const bool active = (filter.tag == tag);
				b->setColor(active ? tActive : tFg);
				b->setHoverColor(active ? tActive : tFg);
				b->setBg(0);
				b->setHoverBg(tHover);
			}
		};

	for (auto& tag : tags) {
		auto* b = tagScroll->makeChild<Ling::Button>();
		b->setText(tag);
		b->setFontSize(SettingTheme::fontXs);
		b->setHeight(22.f);
		b->setPadding(6.f, 0.f, 6.f, 0.f);
		b->setBorderRadius(ToolbarTheme::hoverRadius);
		b->setBorder(0.f, 0);
		b->setBg(0);
		b->setHoverBg(tHover);
		b->setColor(tFg);
		b->setHoverColor(tFg);
		b->setJustifyContent(Ling::Justify::Center);
		b->setAlignItems(Ling::Align::Center);
		b->setMargin(0.f, 0.f, 6.f, 6.f);
		b->setFlexShrink(0.f);
		const std::wstring t = tag;
		b->onClick.add([this, t, syncTagBtns](Ling::Button*) {
			filter.tag = (filter.tag == t) ? L"" : t;
			shownCount = kBatch;
			syncTagBtns();
			this->refreshGrid();
		});
		tagBtns->push_back({ b, t });
	}
	syncTagBtns();
	applyMode();
}

void WinSettingHistory::closeTransientUi(POINT pos)
{
	const bool esc = (pos.x == INT_MAX);
	if (modal) {
		if (esc || (modalCard && !modalCard->isPosIn(pos))) {
			if (!esc) dismissingClick = true;
			closeModal();
		}
		return;
	}
	if (!filterPanel) return;
	if (esc) { closeFilterPanel(); return; }
	// 点面板内部不算点外；点筛选按钮本身交给按钮自己的开关逻辑
	const bool inPanel = filterPanel->isPosIn(pos);
	const bool onBtn = filterBtn && filterBtn->isPosIn(pos);
	if (!inPanel && !onBtn) {
		dismissingClick = true;
		closeFilterPanel();
	}
}

void WinSettingHistory::closeModal()
{
	if (modalKeyBound) {
		win->onKeyDown.remove(modalKeyTok);
		modalKeyBound = false;
	}
	if (!modal) return;
	if (modal->parent) modal->parent->removeChild(modal);
	modal = nullptr;
	modalCard = nullptr;
}

Ling::Node* WinSettingHistory::makeModalCard(float width, const std::wstring& title)
{
	closeModal();
	auto* layer = makeChild<Ling::Node>();
	layer->setPositionType(Ling::Position::Absolute);
	layer->setPosition(Ling::Edge::Left, 0.f);
	layer->setPosition(Ling::Edge::Top, 0.f);
	layer->setPosition(Ling::Edge::Right, 0.f);
	layer->setPosition(Ling::Edge::Bottom, 0.f);
	layer->setBg(0x00000059);
	layer->setJustifyContent(Ling::Justify::Center);
	layer->setAlignItems(Ling::Align::Center);
	modal = layer;

	auto* card = layer->makeChild<Ling::Node>();
	card->setWidth(width);
	card->setFlexDirection(Ling::FlexDirection::Column);
	card->setBg(SettingTheme::card);
	card->setBorder(1.f, SettingTheme::border);
	card->setBorderRadius(SettingTheme::radiusLg);
	card->setPadding(18.f, 16.f, 18.f, 16.f);
	modalCard = card;

	auto* t = card->makeChild<Ling::Label>();
	t->setText(title);
	t->setFontSize(SettingTheme::fontBase);
	t->setColor(SettingTheme::textPrimary);
	t->setMarginBottom(10.f);
	return card;
}

// 编辑某条历史的自定义标签：标签以 chip 形式显示在输入框内；
// 输入文字后按空格 → 变成 chip；输入框为空时按退格/Delete → 删掉最后一个 chip
void WinSettingHistory::showTagDialog(const Setting::HistoryItem& it)
{
	const std::wstring path = it.filePath;
	if (tip) tip->hide();     // 弹窗盖住卡片，先收掉卡片上的停悬提示
	auto tags = std::make_shared<std::vector<std::wstring>>(Setting::get()->getHistoryTags(path));

	auto* card = makeModalCard(320.f, Lang::get(L"setting.filterTags"));

	// 输入框外壳：单行流式——标签 chip 与输入框同一行，按宽度自动换行
	auto* field = card->makeChild<Ling::Node>();
	field->setFlexDirection(Ling::FlexDirection::Row);
	field->setFlexWrap(Ling::Wrap::Wrap);
	field->setAlignItems(Ling::Align::Center);
	field->setWidthPercent(100.f);
	field->setBg(SettingTheme::card);
	field->setBorder(1.f, SettingTheme::input);
	field->setBorderRadius(SettingTheme::radiusInner);
	field->setPadding(6.f, 5.f, 6.f, 5.f);
	field->setMarginBottom(8.f);

	// 光标位置：= 已确认标签的数量（0..size），左侧标签即 Delete 的目标
	auto sel = std::make_shared<int>((int)tags->size());
	auto boxPtr = std::make_shared<Ling::TextBox*>(nullptr);
	auto rebuild = std::make_shared<std::function<void()>>();
	*rebuild = [field, tags, sel, boxPtr]() {
		field->removeAllChildren();
		for (size_t i = 0; i < tags->size(); ++i) {
			auto* chip = field->makeChild<Ling::Label>();
			chip->setText((*tags)[i]);
			chip->setFontSize(SettingTheme::fontXs);
			chip->setColor(SettingTheme::textPrimary);
			// 光标左侧那个标签高亮：Delete 删的就是它
			chip->setBg((*sel == (int)i + 1) ? SettingTheme::accent : SettingTheme::secondary);
			chip->setPadding(5.f, 1.f, 5.f, 1.f);
			chip->setBorderRadius(4.f);
			chip->setMargin(0.f, 0.f, 4.f, 4.f);
			chip->setJustifyContent(Ling::Justify::Center);
			chip->setAlignItems(Ling::Align::Center);
		}
		auto* b = field->makeChild<Ling::TextBox>();
		b->setWidth(96.f);
		b->setHeight(22.f);
		b->setBg(0);
		b->setBorder(0.f, 0);
		b->setColor(SettingTheme::textPrimary);
		b->setFontSize(SettingTheme::fontSm);
		b->setPadding(2.f, 0.f, 2.f, 0.f);
		b->setVerticalCenter(true);
		*boxPtr = b;
		b->focus();
	};
	(*rebuild)();

	// 延后一拍再重建输入框：避免在按键事件派发中途销毁正在处理按键的 TextBox
	auto weakPage0 = getWeakThis();
	auto scheduleRebuild = [rebuild, weakPage0]() {
		auto run = [rebuild, weakPage0]() { if (weakPage0.lock()) (*rebuild)(); };
		if (auto* app = Ling::App::get()) app->dq.TryEnqueue(run);
		else run();
	};
	{	// 打开后聚焦输入框
		auto focusInit = [boxPtr]() { if (boxPtr && *boxPtr) (*boxPtr)->focus(); };
		if (auto* app = Ling::App::get()) app->dq.TryEnqueue(focusInit);
		else focusInit();
	}

	// 空格 → 生成标签；←/→ 在标签间移动光标；退格/Delete（输入框为空）删光标前一个标签
	auto weakPage = getWeakThis();
	Ling::Node* owner = modal;
	modalKeyTok = win->onKeyDown.add([this, weakPage, owner, tags, sel, boxPtr, scheduleRebuild](UINT key) {
		if (!weakPage.lock()) return;
		if (this->modal != owner) return;   // 弹窗已被 Esc 关掉，控件已销毁
		auto* box = *boxPtr;
		if (!box) return;
		const bool textEmpty = box->getText().empty();
		if (key == VK_SPACE) {
			std::wstring t = box->getText();
			while (!t.empty() && iswspace(t.back())) t.pop_back();
			if (!t.empty()) {
				bool dup = false;
				for (auto& e : *tags) if (_wcsicmp(e.c_str(), t.c_str()) == 0) { dup = true; break; }
				if (!dup) {
					const int at = (std::max)(0, (std::min)(*sel, (int)tags->size()));
					tags->insert(tags->begin() + at, t);
					*sel = at + 1;
				}
			}
			scheduleRebuild();
		}
		else if (key == VK_LEFT) {
			if (!textEmpty || *sel <= 0) return;
			--(*sel);
			scheduleRebuild();
		}
		else if (key == VK_RIGHT) {
			if (!textEmpty || *sel >= (int)tags->size()) return;
			++(*sel);
			scheduleRebuild();
		}
		else if (key == VK_BACK || key == VK_DELETE) {
			if (!textEmpty || *sel <= 0) return;   // 还有文字：正常删字符
			tags->erase(tags->begin() + (*sel - 1));
			--(*sel);
			scheduleRebuild();
		}
	});
	modalKeyBound = true;

	auto* row = card->makeChild<Ling::Node>();
	row->setFlexDirection(Ling::FlexDirection::Row);
	row->setWidthPercent(100.f);
	row->setJustifyContent(Ling::Justify::End);
	row->setAlignItems(Ling::Align::Center);
	row->setMarginTop(10.f);
	auto weak = getWeakThis();
	auto deferClose = [this, weak]() {
		auto run = [this, weak]() { if (weak.lock()) this->closeModal(); };
		if (auto* app = Ling::App::get()) app->dq.TryEnqueue(run);
		else run();
	};
	auto deferCloseAndRefresh = [this, weak]() {
		auto run = [this, weak]() {
			if (!weak.lock()) return;
			this->forceGridRebuild = true;   // 标签变了：卡片必须整体重建才看得到
			this->closeModal();
			this->refreshGrid();
		};
		if (auto* app = Ling::App::get()) app->dq.TryEnqueue(run);
		else run();
	};
	SettingUi::btnOutline(row, Lang::get(L"setting.cancel"), deferClose, 72.f)
		->setMarginRight(8.f);
	SettingUi::btnPrimary(row, Lang::get(L"setting.confirm"),
		[boxPtr, path, tags, deferCloseAndRefresh]() {
			// 还没按空格的那段文字也算一个标签
			std::wstring pending = boxPtr && *boxPtr ? (*boxPtr)->getText() : L"";
			while (!pending.empty() && iswspace(pending.back())) pending.pop_back();
			while (!pending.empty() && iswspace(pending.front())) pending.erase(pending.begin());
			std::vector<std::wstring> out = *tags;
			if (!pending.empty()) {
				bool dup = false;
				for (auto& e : out) if (_wcsicmp(e.c_str(), pending.c_str()) == 0) { dup = true; break; }
				if (!dup) out.push_back(pending);
			}
			Setting::get()->setHistoryTags(path, out);
			deferCloseAndRefresh();
		}, 72.f);
}

void WinSettingHistory::refreshGrid()
{
	Setting::get()->pruneExpiredHistory();
	auto all = Setting::get()->getHistoryItemsForUi(true);

	// 按当前筛选过滤（导航 + 时间 + 标签 + 关键词）
	std::vector<Setting::HistoryItem> items;
	for (const auto& it : all) {
		if (matchFilters(it)) items.push_back(it);
	}
	const int total = (int)items.size();

	// 计数（右下「历史条目：%1」，随导航/筛选变化）
	if (countLabel) {
		std::wstring c = Lang::get(L"setting.currentCount");
		c = replaceOnce(c, L"%1", std::to_wstring(total));
		countLabel->setText(c);
	}

	if (!gridHost) return;

	const bool empty = items.empty();
	// 空态与网格互斥显示：两者都带 flexGrow(1)，必须一并收起另一个，否则会平分中部区域
	// 导致「暂无截图历史」只在中部上半段居中（视觉上偏上）。
	if (emptyLabel) {
		const wchar_t* emptyKey = sourceTab == 0 ? L"setting.noAnyHistory"
			: (sourceTab == 2 ? L"setting.noRecordHistory" : L"setting.noHistory");
		emptyLabel->setText(Lang::get(emptyKey));
		empty ? emptyLabel->show() : emptyLabel->hide();
	}
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

	// 需要整体重建？强制标记（标签改动等）/ 已建格子数超过 want / 前缀条目对不上
	bool rebuild = forceGridRebuild || (int)builtIds.size() > want;
	forceGridRebuild = false;
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

		// 录屏条目（source=record）：视频文件不能直接当缩略图解码，用缓存里的封面小图；
		// 中间那颗按钮也从「贴图」换成「在资源管理器里定位」——贴图对视频没有意义。
		const bool isMedia = (it.source == L"record");

		// 缩略图按缩略图级分辨率解码：历史页会随滚动不断累积格子，若每格都持有整张 4K 原图
		// （约 33MB）解码位图，内存会很快堆起来。上限取显示尺寸的 2 倍（物理像素）足够清晰。
		std::wstring thumbPath;
		if (isMedia) {
			const auto cached = (Setting::get()->getHistoryThumbDir() / (it.id + L".png")).wstring();
			std::error_code ec;
			if (std::filesystem::exists(cached, ec)) thumbPath = cached;
		}
		else {
			thumbPath = it.filePath;
		}
		if (!thumbPath.empty()) {
			auto* img = thumbHost->makeChild<Ling::ImageBox>();
			img->setWidthPercent(100.f);
			img->setHeightPercent(100.f);
			const float thumbCap = (kThumbH + kPad * 2.f + kCaptionH) * 2.f * win->dpi;
			img->loadImg(thumbPath, thumbCap * 2.f, thumbCap);
		}
		else {
			// 目录里扫出来的录屏没有封面：用视频图标占位
			auto* ph = thumbHost->makeChild<Ling::Label>();
			ph->setWidthPercent(100.f);
			ph->setHeightPercent(100.f);
			ph->setText(Icon::Video);
			ph->setFontFamily(Icon::Family);
			ph->setFontSize(34.f);
			ph->setColor(0xFFFFFF66);
			ph->setJustifyContent(Ling::Justify::Center);
			ph->setAlignItems(Ling::Align::Center);
		}

		const std::wstring id = it.id;
		const std::wstring filePath = it.filePath;
		auto* copyBtn = mkIconBtn(thumbHost, Icon::Copy, 70.f, false,
			[this, id, thumbHost]() {
				if (this->overlayOpen()) return;
				Util::copyHistoryFileToClipboard(id);
				this->showCopyToast(thumbHost);   // 在当前这张缩略图上提示「复制成功」
			});
		auto* pinBtn = isMedia
			? mkIconBtn(thumbHost, Icon::Folder, 38.f, false, [this, filePath]() {
				if (this->overlayOpen()) return;
				revealInExplorer(filePath);
			})
			: mkIconBtn(thumbHost, Icon::Pin, 38.f, false, [this, id]() {
				if (this->overlayOpen()) return;
				Util::pinHistoryFile(id);
			});
		auto* delBtn = mkIconBtn(thumbHost, Icon::Remove, 6.f, true,
			[this, id]() {
				if (this->overlayOpen()) return;
				// 单条删除不弹确认；删的是这条历史对应的真实文件
				Setting::get()->removeHistoryItem(id);
				// 延后一拍重建：这个按钮自己就在要被拆掉的格子里，同步重建会在它自己的
				// 点击回调里把它销毁；
				auto weak = getWeakThis();
				auto reload = [this, weak]() { if (weak.lock()) this->refreshGrid(); };
				if (auto* app = Ling::App::get()) app->dq.TryEnqueue(reload);
				else reload();
			});
		// 三个按钮默认收起，停悬到这张缩略图上才浮出
		copyBtn->hide(); pinBtn->hide(); delBtn->hide();
		cells.push_back({ thumbHost, { copyBtn, pinBtn, delBtn } });
		if (tip) {
			tip->bind(copyBtn, Lang::get(L"setting.copy"));
			tip->bind(pinBtn, Lang::get(isMedia ? L"setting.revealFile" : L"setting.pin"));
			tip->bind(delBtn, Lang::get(L"setting.delete"));
		}

		// 元信息：时间/日期 · W×H ｜ 来源徽标（点它可编辑自定义标签）｜ 已设标签
		wchar_t whenBuf[32]{};
		struct tm t {};
		time_t tt = (time_t)(it.createdAt / 1000);
		localtime_s(&t, &tt);
		struct tm today {};
		time_t nowT = time(nullptr);
		localtime_s(&today, &nowT);
		if (t.tm_year == today.tm_year && t.tm_mon == today.tm_mon && t.tm_mday == today.tm_mday)
			wcsftime(whenBuf, 32, L"%H:%M", &t);
		else if (t.tm_year == today.tm_year)
			wcsftime(whenBuf, 32, L"%m-%d", &t);
		else
			wcsftime(whenBuf, 32, L"%Y-%m-%d", &t);
		std::wstring metaText = std::wstring(whenBuf);
		if (it.width > 0 && it.height > 0)
			metaText += L" · " + std::to_wstring(it.width) + L"×" + std::to_wstring(it.height);

		auto* metaRow = cell->makeChild<Ling::Node>();
		metaRow->setFlexDirection(Ling::FlexDirection::Row);
		metaRow->setAlignItems(Ling::Align::Center);
		metaRow->setWidthPercent(100.f);
		metaRow->setHeight(kCaptionH - 8.f);
		metaRow->setMarginTop(6.f);

		auto* meta = metaRow->makeChild<Ling::Label>();
		meta->setText(metaText);
		meta->setFontSize(SettingTheme::fontXs);
		meta->setColor(SettingTheme::textTertiary);
		meta->setFlexShrink(1.f);
		meta->setJustifyContent(Ling::Justify::Start);
		meta->setAlignItems(Ling::Align::FlexStart);

		// 标签位：有自定义标签就逐个显示（替换原来的来源标签）；没有则显示来源标签
		const auto tags = Setting::get()->getHistoryTags(it.filePath);
		std::vector<std::wstring> shown = tags;
		if (shown.empty()) shown.push_back(sourceLabel(it.source));
		for (auto& label : shown) {
			auto* chip = metaRow->makeChild<Ling::Button>();
			chip->setText(label);
			chip->setFontSize(SettingTheme::fontXs);
			chip->setColor(SettingTheme::textTertiary);
			chip->setHoverColor(SettingTheme::textPrimary);
			chip->setBg(SettingTheme::secondary);
			chip->setHoverBg(SettingTheme::accent);
			chip->setHeight(16.f);
			chip->setPadding(4.f, 0.f, 4.f, 0.f);
			chip->setBorderRadius(4.f);
			chip->setBorder(0.f, 0);
			chip->setMarginLeft(4.f);
			chip->setFlexShrink(0.f);
			chip->setJustifyContent(Ling::Justify::Center);
			chip->setAlignItems(Ling::Align::Center);
			chip->onClick.add([this, item = it](Ling::Button*) {
				if (this->overlayOpen()) return;   // 气泡/弹窗盖住卡片时不再响应
				this->showTagDialog(item);
			});
			if (tip) tip->bind(chip, Lang::get(L"setting.filterTagEdit"));
		}

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

	auto all = Setting::get()->getHistoryItemsForUi();
	int total = 0;
	for (const auto& it : all)
		if (matchFilters(it)) ++total;
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
	// 气泡 / 弹窗盖住卡片时，卡片不再响应停悬（隐藏浮出的图标按钮）
	const bool blocked = overlayOpen();
	for (auto& c : cells) {
		if (!c.thumb) continue;
		const bool inside = !blocked && c.thumb->isPosIn(pos);
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
