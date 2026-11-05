#include "pch.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <thread>
#include "../Lang.h"
#include "../Setting.h"
#include "../App.h"
#include "WinSettingQuickTranslate.h"
#include "SettingTheme.h"
#include "SettingWidgets.h"
#include "../Tool/IconCodes.h"
#include "../Translate/TranslateService.h"
#include "../Translate/TranslateTypes.h"
#include "../Translate/TranslateModelManager.h"
#include "../Translate/DictLookup.h"
#include "../Translate/BergamotEngine.h"

namespace {
	constexpr float kBubbleMaxW{ 340.f };      // 气泡最大逻辑宽度
	constexpr float kHistW{ 200.f };           // 历史侧栏展开宽度
	constexpr float kGapSidebar{ 16.f };       // 历史侧栏与主面板的间距（中间那条缝）
	constexpr float kToggleBox{ 36.f };        // 展开/折叠按钮的尺寸（与顶栏等高）
	constexpr UINT kAnimTimerId{ 0x4151 };     // 侧栏动画定时器

	// 历史条目上的时间戳：今天只显示 HH:mm，非今天只显示 MM-DD
	std::wstring fmtStamp(long long ts)
	{
		time_t t = (time_t)(ts / 1000);
		tm local{};
		localtime_s(&local, &t);
		time_t now = time(nullptr);
		tm cur{};
		localtime_s(&cur, &now);
		const bool today = local.tm_year == cur.tm_year && local.tm_yday == cur.tm_yday;
		wchar_t buf[16]{};
		wcsftime(buf, 16, today ? L"%H:%M" : L"%m-%d", &local);
		return buf;
	}

	// 单行条目放得下的标题：超长截断加省略号
	std::wstring fitTitle(const std::wstring& s, size_t maxChars)
	{
		if (s.size() <= maxChars) return s;
		return s.substr(0, maxChars) + L"…";
	}

	// 下拉宽度：弹层与触发按钮取同一常量，两者始终等宽，避免"弹出项比下拉栏宽/窄"的错位。
	// 语言栏放得下「自动识别语言」，服务栏只需容下四字服务名，故比语言栏窄。
	constexpr float kLangDdlW{ 150.f };
	constexpr float kProviderDdlW{ 100.f };
	constexpr float kToneDdlW{ 88.f };        // 语气下拉：只放两三个字，比服务栏更窄
	// 下拉箭头：本页下拉栏高 28（小于其他页的 ctrlH 32），同一字号会显得偏大，故比
	// 全局 DropSize(16) 再小一档。
	constexpr float kQuickDropSize{ 14.f };

	// 下拉按钮：左=选中值 Label，中=弹性空白，右=下拉图标（与 SettingWidgets::selectDdl 同款）。
	// 值与图标字体族不同，故值也走子 Label，Button 自身 Text 仅置空占位。
	struct DdlParts { Ling::Button* btn{ nullptr }; Ling::Label* value{ nullptr }; };

	DdlParts makeDdl(Ling::Node* parent)
	{
		auto* btn = parent->makeChild<Ling::Button>();
		btn->setFlexDirection(Ling::FlexDirection::Row);
		btn->setJustifyContent(Ling::Justify::Start);
		btn->setAlignItems(Ling::Align::Center);
		btn->setText(L"");

		auto* value = btn->makeChild<Ling::Label>();
		value->setFontSize(SettingTheme::fontBase);
		value->setColor(SettingTheme::textPrimary);
		value->setFlexShrink(1.f);

		auto* spacer = btn->makeChild<Ling::Node>();   // 弹性空白：把下拉图标推到最右侧
		spacer->setFlexGrow(1.f);
		spacer->setFlexShrink(1.f);

		auto* icon = btn->makeChild<Ling::Label>();
		icon->setText(Icon::Dropdown);
		icon->setFontFamily(Icon::Family);
		icon->setFontSize(kQuickDropSize);
		icon->setColor(SettingTheme::textMuted);
		icon->setFlexShrink(0.f);
		icon->setMarginLeft(6.f);

		return { btn, value };
	}

	// 翻译服务清单：与「功能设置 → 翻译」共用同一份（只含非 AI 引擎）
	struct ProviderItem { const wchar_t* id; const wchar_t* labKey; };
	const ProviderItem kProviders[] = {
		{ L"microsoft", L"setting.fnTransMs" },
		{ L"google",    L"setting.fnTransGoogle" },
		{ L"youdao",    L"setting.fnTransYoudao" },
		{ L"baidu",     L"setting.fnTransBaidu" },
		{ L"offline",   L"setting.fnTransOffline" },
	};
	constexpr int kProviderCount = (int)(sizeof(kProviders) / sizeof(kProviders[0]));

	int providerIndex(const std::wstring& id)
	{
		for (int i = 0; i < kProviderCount; i++)
			if (_wcsicmp(kProviders[i].id, id.c_str()) == 0) return i;
		return 0;
	}

	// 微软翻译语气：id 原样作为 tone 参数（standard 表示不传），labelKey 取语言包
	struct ToneItem { const wchar_t* id; const wchar_t* labelKey; };
	const ToneItem kTones[] = {
		{ L"standard", L"setting.fnToneStandard" },
		{ L"informal", L"setting.fnToneInformal" },
		{ L"formal",   L"setting.fnToneFormal"   },
	};
	constexpr int kToneCount = (int)(sizeof(kTones) / sizeof(kTones[0]));

	int toneIndex(const std::wstring& id)
	{
		for (int i = 0; i < kToneCount; i++)
			if (_wcsicmp(kTones[i].id, id.c_str()) == 0) return i;
		return 0;
	}

	// 语言代码 → 下拉显示名（auto 单独处理；未知代码原样返回）
	std::wstring langLabel(const std::wstring& code)
	{
		if (_wcsicmp(code.c_str(), L"auto") == 0) return Lang::get(L"setting.fnLangAuto");
		for (int i = 0; i < kPopularLangCount; i++)
			if (code == kPopularLangs[i].code) return Lang::get(kPopularLangs[i].labelKey);
		return code;
	}
}

WinSettingQuickTranslate::WinSettingQuickTranslate(Ling::WinBase* parent) : Ling::Node(parent)
{
	auto host = makeChild<Ling::Node>();
	host->setFlexGrow(1.f);
	host->setFlexShrink(1.f);   // 允许随窗口变矮收缩（默认 flexShrink=0 会拒绝压缩）
	host->setWidthPercent(100.f);
	// 解除 min-height:auto 下限并切断 min-content 向上泄漏，否则 host 会被记录内容高度锁死，
	// 窗口变矮时无法收缩，把输入区挤出可视区（"面板被裁剪"）。
	YGNodeStyleSetMinHeight(host->node, 0.f);
	YGNodeSetMinContentHeight(host->node, 0.f);
	buildTranslateTab(host);
	ensureTargetWhenSrcZh();   // 源为中文/自动识别时，目标默认英语

	// 侧栏动画定时器只注册一次，toggleHistory 只负责启停
	timerTok = win->onTimer.add([this, weakThis = getWeakThis()](UINT id) {
		if (!weakThis.lock()) return;
		if (id != kAnimTimerId) return;
		animateHistory();
	});
	// 历史侧栏展开时，点击侧栏外的空白处收起它；点切换按钮本身交给 onClick，避免被这里
	// 先收起来又被 onClick 展开（连点两次看起来像没反应）
	mouseDownTok = win->onMouseDown.add([this, weakThis = getWeakThis()](POINT pos, bool isRight) {
		if (!weakThis.lock() || isRight) return;
		if (!visual.IsVisible() || !histOpen || histAnimRunning) return;
		if (historyShell && historyShell->isPosIn(pos)) return;
		if (histToggleBtn && histToggleBtn->isPosIn(pos)) return;
		// 顶栏控件与输入区不算空白区域，点击时保持侧栏展开
		if (srcLangBtn && srcLangBtn->isPosIn(pos)) return;
		if (tgtLangBtn && tgtLangBtn->isPosIn(pos)) return;
		if (swapBtn && swapBtn->isPosIn(pos)) return;
		if (providerBtn && providerBtn->isPosIn(pos)) return;
		if (toneBtn && toneBtn->isPosIn(pos)) return;
		if (sendBtn && sendBtn->isPosIn(pos)) return;
		if (input && input->isPosIn(pos)) return;
		toggleHistory();
	});
	// 结果复制按钮默认隐藏：鼠标进入某条译文所在行时才浮出。
	// 记录里可能残留已被重建掉的气泡/按钮，先按 weak 指针清掉，再遍历当前存活的。
	mouseMoveTok = win->onMouseMove.add([this, weakThis = getWeakThis()](POINT pos) {
		if (!weakThis.lock()) return;
		std::erase_if(copyHosts, [](const CopyHost& h) {
			return h.bubbleAlive.expired() || h.btnAlive.expired();
			});
		for (auto& h : copyHosts) {
			if (!h.bubble || !h.btn) continue;
			// 以整条气泡行作为停悬区：气泡与按钮之间有间距，落在间距上也不会让按钮闪没。
			// 按钮始终参与布局、只切 visual 可见性，命中区域在隐藏时也已算好。
			const bool on = h.bubble->parent && h.bubble->parent->isPosIn(pos);
			h.btn->visual.IsVisible(on);
		}
		// 历史条目：悬停该行时才显示行尾删除键
		for (auto& r : histRows) {
			if (!r.row || !r.del) continue;
			r.del->visual.IsVisible(r.row->isPosIn(pos));
		}
	});
	// 发送按钮的光标由这里统一决定：可发送时为手型，置灰时为普通箭头
	cursorTok = win->onCursor.add([this](bool* handled) {
		if (!sendBtn || !win || !win->hwnd) return;
		POINT p{};
		if (!GetCursorPos(&p)) return;
		ScreenToClient(win->hwnd, &p);
		if (!sendBtn->isPosIn(p)) return;
		SetCursor(LoadCursorW(nullptr, canSendNow() ? IDC_HAND : IDC_ARROW));
		if (handled) *handled = true;
	});
	// 发送按钮的点击同样由这里处理：置灰时直接忽略
	sendClickTok = win->onMouseDown.add([this](POINT pos, bool isRight) {
		if (isRight || !sendBtn || !visual.IsVisible()) return;
		if (!sendBtn->isPosIn(pos)) return;
		if (canSendNow()) doTranslate(L"");
	});
	loadSessions();
}

WinSettingQuickTranslate::~WinSettingQuickTranslate()
{
	hideOptionsBox();
	if (histAnimRunning) win->killTimer(kAnimTimerId);
	win->onTimer.remove(timerTok);
	win->onMouseDown.remove(mouseDownTok);
	win->onMouseMove.remove(mouseMoveTok);
	win->onCursor.remove(cursorTok);
	win->onMouseDown.remove(sendClickTok);
	// 离开这一页就把离线翻译模型（几百 MB）还回去：截图会话那条链路已在 WinCap::onClosed
	// 释放过，这里补上缺的那条 —— 否则在这一页翻过一次离线之后，模型会一直常驻到退出。
	// release() 内部加锁，后台还在翻的那一段会先跑完，不会被抽掉。
	BergamotEngine::release();
}

// ============ 语言选项弹层 ============

void WinSettingQuickTranslate::showOptionsBox(Ling::Button* anchor,
	const std::vector<std::wstring>& options, int current, float boxW, std::function<void(int)> onPick)
{
	hideOptionsBox();
	auto weak = getWeakThis();
	const float itemH = 30.f;
	const float itemGap = 4.f;   // 选项之间 4px 间距
	const float itemN = (float)options.size();
	const float boxH = itemH * itemN + itemGap * std::max(0.f, itemN - 1.f) + 8.f;   // 弹层逻辑高度（4 + 项 + 间距 + 4）
	optionsPopup = std::make_unique<SettingUi::Popup>(win, boxW, boxH);
	auto* box = optionsPopup->box();
	box->setBg(SettingTheme::popover);
	box->setBorder(1.f, SettingTheme::border);
	box->setBorderRadius(SettingTheme::radius);
	box->setPadding(4.f, 4.f, 4.f, 4.f);
	for (int i = 0; i < (int)options.size(); i++) {
		auto* it = box->makeChild<Ling::Button>();
		it->setText(options[i]);
		it->setHeight(itemH);
		it->setWidthPercent(100.f);
		it->setFlexDirection(Ling::FlexDirection::Row);
		it->setJustifyContent(Ling::Justify::Start);
		it->setAlignItems(Ling::Align::Center);
		it->setPadding(10.f, 0, 10.f, 0);
		it->setBorderRadius(SettingTheme::radiusInner);   // 内层小框：与外框(10)同心
		if (i + 1 < (int)options.size()) it->setMarginBottom(itemGap);
		bool sel = i == current;
		// 与其它下拉（SettingUi::selectDdl）保持一致：悬停/选中只换浅色底(popupSelBg)，文字恒用
		// popupFg 不变色。此前非选中项没设 hoverColor，悬停会退回 Button 默认的深灰文字色 ——
		// 表现为"停悬时文字变色"而不是浅色底，即本页下拉与其它下拉观感不一致的根因。
		it->setColor(SettingTheme::popupFg);
		it->setBg(sel ? SettingTheme::popupSelBg : 0);
		it->setHoverBg(SettingTheme::popupSelBg);
		it->setHoverColor(SettingTheme::popupFg);
		it->onClick.add([this, weak, i, onPick](Ling::Button*) {
			// 在回调内销毁弹层自身（独立窗口，不触碰宿主 hwnd），随后再触发回调。
			if (!weak.lock()) return;
			hideOptionsBox();
			if (onPick) onPick(i);
			});
	}
	optionsPopup->setAnchor(anchor);
	optionsPopup->onDismiss = [this]() { hideOptionsBox(); };
	optionsPopup->open();
}

void WinSettingQuickTranslate::hideOptionsBox()
{
	if (!optionsPopup) return;
	// 延迟销毁：optionsPopup 往往在宿主 onMouseDown 事件订阅回调中被关闭（点击弹层外部、
	// 或点击打开它的按钮切换当前下拉）。若在这里同步 optionsPopup.reset()，会在宿主事件迭代
	// 中途触发 Popup 析构 → removeHostHooks() → host->onMouseDown.remove(当前正在执行的 token)，
	// 使 winrt::event 的订阅列表迭代器失效；同时析构里还会 UnhookWindowsHookEx + DestroyWindow，
	// 是在钩子回调内部执行 Win32 重入危险操作，表现为 UI 线程未响应、多次重复后崩溃。
	// 把所有权移入 pending 推迟到调度队列再真正析构，既避开事件迭代期间的自我销毁，
	// 又不会影响紧随其后创建的新弹层。
	auto* app = Ling::App::get();
	if (app) {
		auto pending = std::make_shared<std::unique_ptr<SettingUi::Popup>>();
		*pending = std::move(optionsPopup);
		app->dq.TryEnqueue([pending]() { pending->reset(); });
	} else {
		optionsPopup.reset();   // 无调度队列时兜底：同步析构（正常路径不会走到）
	}
}

// ============ 翻译面板 ============

void WinSettingQuickTranslate::buildTranslateTab(Ling::Node* p)
{
	// 页面底铺画布色：左右两块直接占满内容区，中间留 16px 的缝
	if (p->parent) p->parent->setBg(SettingTheme::canvas);
	p->setFlexDirection(Ling::FlexDirection::Row);
	p->setAlignItems(Ling::Align::Stretch);
	p->setBg(0);

	// —— 历史侧栏（宽度动画，内容被圆角剪裁） ——
	historyShell = p->makeChild<Ling::Node>();
	historyShell->setWidth(0.f);
	historyShell->setFlexShrink(0.f);
	// 强制侧栏撑满整行高度（= 主面板高度）。否则仅靠 alignItems:Stretch 时，内部
	// histInner(宽200) 的内容会把侧栏高度收缩到内容高，导致灰色底/圆角底部与
	// 输入框底边不对齐（侧栏"不够高"）。heightPercent 100 依赖 host 的可确定高度。
	historyShell->setHeightPercent(100.f);
	historyShell->setBorderRadius(SettingTheme::contentCardRadius);
	historyShell->setBg(SettingTheme::card);
	// 折叠宽度为 0 时 margin 也必须为 0，否则会残留 16px 间距使主面板整体左移。
	// margin 由 animateHistory 随宽度同步。
	historyShell->setMarginRight(0.f);
	// 初始为折叠态：直接从布局树移除（display:none）。否则宽度为 0 的 historyShell 内部
	// histInner(宽200+x 内容) 以 alignItems:Stretch 撑高 flex 行，使折叠态主面板上下间距
	// 比其他内容页更大。
	historyShell->hide();

	histInner = historyShell->makeChild<Ling::Node>();
	histInner->setWidth(kHistW);   // 展开宽度（Yoga 的 width 含内边距）
	histInner->setHeightPercent(100.f);
	histInner->setFlexDirection(Ling::FlexDirection::Column);
	histInner->setPadding(20.f);

	auto titleRow = histInner->makeChild<Ling::Node>();
	titleRow->setFlexDirection(Ling::FlexDirection::Row);
	titleRow->setWidthPercent(100.f);
	titleRow->setAlignItems(Ling::Align::Center);
	titleRow->setHeight(36.f);   // 与顶栏等高，标题行和右侧语言栏在同一水平线
	titleRow->setMarginBottom(8.f);
	auto hTitle = titleRow->makeChild<Ling::Label>();
	hTitle->setText(Lang::get(L"quick.history"));
	hTitle->setFontSize(13.f);
	hTitle->setColor(SettingTheme::textSecondary);
	hTitle->setFlexGrow(1.f);
	auto clearBtn = titleRow->makeChild<Ling::Button>();
	clearBtn->setText(Lang::get(L"quick.clear"));
	clearBtn->setHeight(24.f);
	clearBtn->setPadding(8.f, 0, 8.f, 0);
	clearBtn->setBorderRadius(SettingTheme::radiusSm);
	clearBtn->setColor(SettingTheme::textMuted);
	clearBtn->setHoverColor(SettingTheme::textPrimary);   // 不设 hover 色会落到按钮默认的深灰
	clearBtn->setHoverBg(SettingTheme::accent);
	clearBtn->onClick.add([this](Ling::Button*) { clearSessions(); });

	auto newHistBtn = histInner->makeChild<Ling::Button>();
	newHistBtn->setText(L"");
	newHistBtn->setHeight(32.f);
	newHistBtn->setWidthPercent(100.f);
	newHistBtn->setFlexDirection(Ling::FlexDirection::Row);
	newHistBtn->setJustifyContent(Ling::Justify::Center);
	newHistBtn->setAlignItems(Ling::Align::Center);
	newHistBtn->setBorderRadius(SettingTheme::radiusSm);
	// 比卡片深一档的实心按钮
	newHistBtn->setBg(SettingTheme::secondary);
	newHistBtn->setHoverBg(SettingTheme::popupSelBg);
	newHistBtn->setMarginBottom(8.f);
	// 加号图标与文字分属两套字体，拆成两个子标签排列
	auto newPlus = newHistBtn->makeChild<Ling::Label>();
	newPlus->setText(Icon::Add);
	newPlus->setFontFamily(Icon::Family);
	newPlus->setFontSize(Icon::DropSize);   // 与下拉箭头同字号
	newPlus->setColor(SettingTheme::textSecondary);
	newPlus->setMarginRight(6.f);
	auto newText = newHistBtn->makeChild<Ling::Label>();
	newText->setText(Lang::get(L"quick.new"));
	newText->setFontSize(13.f);
	newText->setColor(SettingTheme::textSecondary);
	// 按钮自身不绘制文本；悬停配色在 onEnter/onLeave 中同步给两个子标签
	newHistBtn->onEnter.add([newPlus, newText](Ling::Button*) {
		newPlus->setColor(SettingTheme::foreground);
		newText->setColor(SettingTheme::foreground);
		});
	newHistBtn->onLeave.add([newPlus, newText](Ling::Button*) {
		newPlus->setColor(SettingTheme::textSecondary);
		newText->setColor(SettingTheme::textSecondary);
		});
	newHistBtn->onClick.add([this](Ling::Button*) { startNewSession(); });

	histScroll = histInner->makeChild<Ling::ScrollerBox>();
	histScroll->setFlexGrow(1.f);
	histScroll->setWidthPercent(100.f);
	histScroll->setScrollBarVisible(false);   // 历史侧栏也不显示滚动条
	histList = histScroll->makeChild<Ling::Node>();
	histList->setFlexDirection(Ling::FlexDirection::Column);
	histList->setWidthPercent(100.f);

	// —— 主面板 ——
	auto panel = p->makeChild<Ling::Node>();
	panel->setFlexGrow(1.f);
	panel->setFlexShrink(1.f);        // 允许收缩：侧栏展开且窗口变窄时面板压缩，不被剪裁
	panel->setFlexDirection(Ling::FlexDirection::Column);
	// 主面板也是一张卡：与左侧历史卡同色、同圆角，靠页面底色与 16px 的缝分开
	panel->setBg(SettingTheme::card);
	panel->setBorderRadius(SettingTheme::contentCardRadius);
	panel->setPadding(20.f);
	// 解除 panel 的 auto-min(min-height:auto) 与 min-content 下限：否则窗口变矮时
	// panel 被内容高度锁死无法收缩，底部输入区会被挤出可视区。
	YGNodeStyleSetMinHeight(panel->node, 0.f);
	YGNodeSetMinContentHeight(panel->node, 0.f);

	// 顶栏
	auto topBar = panel->makeChild<Ling::Node>();
	topBar->setFlexDirection(Ling::FlexDirection::Row);
	topBar->setWidthPercent(100.f);
	// 高度 36：与发送按钮(36×36)拉齐，整排按钮/下拉一致高
	topBar->setHeight(36.f);
	topBar->setAlignItems(Ling::Align::Center);

	// 展开/折叠按钮：跟着主面板一起移动，始终在下拉框左边
	histToggleBtn = topBar->makeChild<Ling::Button>();
	histToggleBtn->setText(Icon::Expand);      // 初始侧栏收起 → 显示「展开」
	histToggleBtn->setWidth(kToggleBox);
	histToggleBtn->setHeight(kToggleBox);
	histToggleBtn->setFontFamily(Icon::Family);
	histToggleBtn->setFontSize(Icon::SizeSm);
	histToggleBtn->setBorderRadius(SettingTheme::radiusSm);
	histToggleBtn->setColor(SettingTheme::textMuted);
	// 图标本身不变色：悬停只换背景
	histToggleBtn->setHoverColor(SettingTheme::textMuted);
	histToggleBtn->setHoverBg(SettingTheme::accent);
	histToggleBtn->onClick.add([this](Ling::Button*) { toggleHistory(); });

	DdlParts srcDdl = makeDdl(topBar);
	srcLangBtn = srcDdl.btn;
	srcLangVal = srcDdl.value;
	srcLangBtn->setHeight(36.f);
	srcLangBtn->setWidth(kLangDdlW);
	srcLangBtn->setPadding(10.f, 0, 10.f, 0);
	srcLangBtn->setFlexShrink(1.f);   // 允许收缩：窄窗时压缩到可用宽度，不被剪裁
	srcLangBtn->setMarginLeft(8.f);
	srcLangBtn->setMarginRight(4.f);
	srcLangBtn->setBorderRadius(SettingTheme::radiusSm);
	srcLangBtn->setHoverBg(SettingTheme::popupSelBg);   // 与其它下拉触发框一致
	srcLangBtn->onClick.add([this](Ling::Button* btn) {
		if (optionsPopup) return;
		showLangBox(btn, false);
		});

	swapBtn = topBar->makeChild<Ling::Button>();
	swapBtn->setText(Icon::Swap);
	swapBtn->setFontFamily(Icon::Family);
	swapBtn->setFontSize(18.f);
	swapBtn->setWidth(36.f);
	swapBtn->setHeight(36.f);
	swapBtn->setBorderRadius(SettingTheme::radiusSm);
	swapBtn->setColor(SettingTheme::textMuted);
	swapBtn->setHoverColor(SettingTheme::textPrimary);    // 停悬变亮，而不是落到按钮默认的深灰
	swapBtn->setHoverBg(SettingTheme::accent);
	swapBtn->onClick.add([this](Ling::Button*) {
		auto* s = Setting::get();
		auto src = s->getTranslateSourceLang();
		auto tgt = s->getTranslateTargetLang();
		tgtPinnedZh = false;   // 互换视为新组合，允许再次自动纠偏
		s->setTranslateSourceLang(tgt);
		s->setTranslateTargetLang(_wcsicmp(src.c_str(), L"auto") == 0 ? L"zh-CN" : src);
		ensureTargetWhenSrcZh();
		refreshTopBar();
		});

	DdlParts tgtDdl = makeDdl(topBar);
	tgtLangBtn = tgtDdl.btn;
	tgtLangVal = tgtDdl.value;
	tgtLangBtn->setHeight(36.f);
	tgtLangBtn->setWidth(kLangDdlW);
	tgtLangBtn->setPadding(10.f, 0, 10.f, 0);
	tgtLangBtn->setFlexShrink(1.f);   // 允许收缩：窄窗时压缩到可用宽度，不被剪裁
	tgtLangBtn->setMarginLeft(4.f);
	tgtLangBtn->setBorderRadius(SettingTheme::radiusSm);
	tgtLangBtn->setHoverBg(SettingTheme::popupSelBg);   // 与其它下拉触发框一致
	tgtLangBtn->onClick.add([this](Ling::Button* btn) {
		if (optionsPopup) return;
		showLangBox(btn, true);
		});

	auto spacer = topBar->makeChild<Ling::Node>();
	spacer->setFlexGrow(1.f);

	// 消息容器（相对定位，用于空态居中提示语覆盖）
	auto msgWrap = panel->makeChild<Ling::Node>();
	msgWrap->setFlexGrow(1.f);
	// 关键：默认 flexShrink=0，必须显式开启收缩，否则窗口变矮时 msgWrap 不会收缩，
	// 底部输入区会被挤出 panel 可视区域。
	msgWrap->setFlexShrink(1.f);
	msgWrap->setWidthPercent(100.f);
	msgWrap->setPositionType(Ling::Position::Relative);
	// 解除 msgWrap 的 auto-min（min-height:auto）下限：否则内容高度会锁定其最小高度。
	YGNodeStyleSetMinHeight(msgWrap->node, 0.f);
	// 切断 msgWrap 向祖先泄漏的 min-content：在高度轴上声明自身 min-content 为 0。
	YGNodeSetMinContentHeight(msgWrap->node, 0.f);

	msgScroll = msgWrap->makeChild<Ling::ScrollerBox>();
	msgScroll->setFlexGrow(1.f);
	msgScroll->setWidthPercent(100.f);
	msgScroll->setScrollBarVisible(false);   // 消息区不显示滚动条（滚轮仍可滚）
	msgList = msgScroll->makeChild<Ling::Node>();
	msgList->setFlexDirection(Ling::FlexDirection::Column);
	msgList->setWidthPercent(100.f);
	msgList->setPadding(0.f, 12.f, 0.f, 12.f);

	// 空态居中提示语
	welcomeWrap = msgWrap->makeChild<Ling::Node>();
	welcomeWrap->setPositionType(Ling::Position::Absolute);
	welcomeWrap->setWidthPercent(100.f);
	welcomeWrap->setHeightPercent(100.f);
	welcomeWrap->setFlexDirection(Ling::FlexDirection::Column);
	welcomeWrap->setJustifyContent(Ling::Justify::Center);
	welcomeWrap->setAlignItems(Ling::Align::Center);
	auto welcomeLab = welcomeWrap->makeChild<Ling::Label>();
	welcomeLab->setText(Lang::get(L"quick.welcome"));
	// 字号 24
	welcomeLab->setFontSize(24.f);
	welcomeLab->setColor(SettingTheme::textPrimary);

	// 输入区
	auto inputArea = panel->makeChild<Ling::Node>();
	inputArea->setFlexDirection(Ling::FlexDirection::Column);
	inputArea->setWidthPercent(100.f);
	// 底部 padding 必须为 0：底部留白统一由外层 content 的 contentPadYBottom 提供。
	inputArea->setPadding(0.f, 8.f, 0.f, 0.f);
	// 解除 inputArea 的 auto-min 下限：避免把其 min-content 撑大导致窗口变矮时被裁剪。
	YGNodeStyleSetMinHeight(inputArea->node, 0.f);
	YGNodeSetMinContentHeight(inputArea->node, 0.f);

	// 输入框（圆角大容器）：上输入 + 下工具行
	auto inputBox = inputArea->makeChild<Ling::Node>();
	inputBox->setFlexDirection(Ling::FlexDirection::Column);
	inputBox->setWidthPercent(100.f);
	inputBox->setBg(SettingTheme::secondary);   // 与卡片同色会难以分辨，输入框提亮一档
	inputBox->setBorder(1.f, SettingTheme::input);
	inputBox->setBorderRadius(26.f);   // = 发送按钮半径 18 + 右下内边距 8：圆角与按钮同心
	inputBox->setPadding(10.f, 8.f, 8.f, 8.f);

	input = inputBox->makeChild<Ling::TextBox>();
	input->setPlaceholder(Lang::get(L"quick.placeholder"));
	input->setWidthPercent(100.f);
	// 此处是"文字输入区"的高度(86)，不是整个输入框：
	// 整个圆角容器 ≈ 上内边距10 + 文字区86 + 工具行间距6 + 工具行36 + 下内边距8 ≈ 146。
	input->setHeight(86.f);
	input->setBorder(0.f, 0);
	input->setBorderRadius(SettingTheme::radiusSm);
	input->setBg(0);
	input->setColor(SettingTheme::textPrimary);
	input->setPadding(8.f, 4.f, 8.f, 4.f);

	// 工具行：翻译服务 + 字数 + 发送（圆形图标按钮）
	auto toolRow = inputBox->makeChild<Ling::Node>();
	toolRow->setFlexDirection(Ling::FlexDirection::Row);
	toolRow->setWidthPercent(100.f);
	toolRow->setAlignItems(Ling::Align::Center);
	toolRow->setMarginTop(6.f);

	DdlParts provDdl = makeDdl(toolRow);
	providerBtn = provDdl.btn;
	providerVal = provDdl.value;
	providerVal->setColor(SettingTheme::textMuted);
	providerBtn->setHeight(36.f);
	providerBtn->setWidth(kProviderDdlW);
	providerBtn->setPadding(10.f, 0, 10.f, 0);
	providerBtn->setFlexShrink(1.f);   // 允许收缩：窄窗时压缩到可用宽度，不被剪裁
	providerBtn->setBorderRadius(SettingTheme::radiusSm);
	providerBtn->setHoverBg(SettingTheme::popupSelBg);   // 与其它下拉触发框一致
	providerBtn->onClick.add([this](Ling::Button* btn) {
		if (optionsPopup) return;
		showProviderBox(btn);
		});

	// 语气下拉：只有微软翻译支持（标准 / 非正式 / 正式），选其它服务时隐藏
	DdlParts toneDdl = makeDdl(toolRow);
	toneBtn = toneDdl.btn;
	toneVal = toneDdl.value;
	toneVal->setColor(SettingTheme::textMuted);
	toneBtn->setHeight(36.f);
	toneBtn->setWidth(kToneDdlW);
	toneBtn->setPadding(10.f, 0, 10.f, 0);
	toneBtn->setFlexShrink(1.f);
	toneBtn->setBorderRadius(SettingTheme::radiusSm);
	toneBtn->setHoverBg(SettingTheme::popupSelBg);
	toneBtn->onClick.add([this](Ling::Button* btn) {
		if (optionsPopup) return;
		showToneBox(btn);
		});
	updateToneDdl();   // 初始按当前服务决定显示/隐藏

	auto toolGrow = toolRow->makeChild<Ling::Node>();
	toolGrow->setFlexGrow(1.f);

	counterLab = toolRow->makeChild<Ling::Label>();
	counterLab->setText(L"0 / 5000");
	counterLab->setFontSize(11.f);
	counterLab->setColor(SettingTheme::textTertiary);
	counterLab->setFlexShrink(0.f);
	counterLab->setMarginRight(8.f);

	// 发送按钮用普通 Node 而不是 Button：置灰时不希望它带按钮的悬停手型光标，
	// 光标与点击都交给窗口事件统一处理（见 ctor 中的 onCursor / onMouseDown）
	sendBtn = toolRow->makeChild<Ling::Node>();
	sendBtn->setWidth(36.f);
	sendBtn->setHeight(36.f);
	sendBtn->setBorderRadius(SettingTheme::radiusFull);
	sendBtn->setJustifyContent(Ling::Justify::Center);   // 图标居中：普通 Node 默认从左上角开始排
	sendBtn->setAlignItems(Ling::Align::Center);
	sendIcon = sendBtn->makeChild<Ling::Label>();
	sendIcon->setText(Icon::Send2);
	sendIcon->setFontFamily(Icon::Family);
	sendIcon->setFontSize(18.f);
	updateSendBtn();   // 初始（无输入）：灰底灰图标

	// 回车翻译（TextBox 的 VK_RETURN 会插入 '\n'，在 onTextChanged 里捕获）
	auto weak = getWeakThis();
	input->onTextChanged.add([this, weak](Ling::TextBox*, const std::wstring& val) {
		if (!weak.lock()) return;
		updateSendBtn();   // 有无输入 / 当前服务是否正在翻 → 切换发送按钮配色
		// 输入字数（Label::setText 内部会刷新窗口）
		if (counterLab)
			counterLab->setText(std::to_wstring(val.size()) + L" / 5000");
		if (val.empty()) return;
		wchar_t last = val.back();
		if (last != L'\r' && last != L'\n') return;
		std::wstring t = val;
		while (!t.empty() && (t.back() == L'\r' || t.back() == L'\n')) t.pop_back();
		// 当前服务正在翻：回车不发送、也不清空输入，只去掉这次回车敲进去的换行。
		// 换到别的服务后这条限制自然解除（请求按服务区分）。
		if (providerBusy(Setting::get()->getTranslateProvider())) {
			input->setText(t);
			return;
		}
		input->setText(L"");
		doTranslate(t);
		});

	refreshTopBar();
	refreshProviderBtn();
	refreshHistoryList();
	updateWelcome();
}

void WinSettingQuickTranslate::refreshTopBar()
{
	auto* s = Setting::get();
	srcLangVal->setText(langLabel(s->getTranslateSourceLang()));
	tgtLangVal->setText(langLabel(s->getTranslateTargetLang()));
	win->refresh();
}

// 现在能不能发送：有输入、且当前所选服务不在翻译
bool WinSettingQuickTranslate::canSendNow()
{
	return input && !input->getText().empty()
		&& !providerBusy(Setting::get()->getTranslateProvider());
}

// 发送按钮配色：无输入、或**当前所选服务**正在翻译 = 置灰（不可发送）；
// 有输入 = #0FDC78 底 + 白图标。翻译请求按服务区分：换到别的服务立刻就能发。
void WinSettingQuickTranslate::updateSendBtn()
{
	if (!sendBtn) return;
	const bool canSend = canSendNow();
	// 置灰底色要和输入框底色（secondary）区分开：浅色下 secondary 与 popupSelBg 同为 #F5F4F4，
	// 直接复用会让圆形按钮整块消失，所以按主题各取一档。
	const uint32_t disabledBg = SettingTheme::isDark() ? 0x424242FF : 0xDDDDDDFF;
	sendBtn->setBg(canSend ? 0x0FDC78FF : disabledBg);
	if (sendIcon) sendIcon->setColor(canSend ? 0xFFFFFFFF : SettingTheme::textMuted);
}

bool WinSettingQuickTranslate::providerBusy(const std::wstring& provider) const
{
	for (const auto& p : pending)
		if (_wcsicmp(p.provider.c_str(), provider.c_str()) == 0) return true;
	return false;
}

// 源为中文或自动识别时，目标默认英语（避免中译中）；用户手动改回中文后锁定不再纠偏
void WinSettingQuickTranslate::ensureTargetWhenSrcZh()
{
	auto* s = Setting::get();
	const std::wstring src = s->getTranslateSourceLang();
	if (!isChineseLang(src) && _wcsicmp(src.c_str(), L"auto") != 0) {
		tgtPinnedZh = false;
		return;
	}
	if (tgtPinnedZh) return;
	if (_wcsicmp(s->getTranslateTargetLang().c_str(), L"en") == 0) return;
	s->setTranslateTargetLang(L"en");
	refreshTopBar();
}

void WinSettingQuickTranslate::showLangBox(Ling::Button* btn, bool target)
{
	auto* s = Setting::get();
	// 离线服务只能翻「已经下载的语言包」：下拉里只列这些。在线服务照旧列全部主流语种
	// —— 用户只装了中/英/日，下拉就不该还能选到法语，选了也只是白白回退到在线。
	std::vector<std::wstring> codes;
	codes.reserve(kPopularLangCount);
	for (int i = 0; i < kPopularLangCount; i++) codes.push_back(kPopularLangs[i].code);
	if (_wcsicmp(s->getTranslateProvider().c_str(), TranslateProvider::Offline) == 0) {
		auto& mgr = TranslateModelManager::instance();
		std::vector<std::wstring> keep;
		for (const auto& code : codes) {
			// 源语言看「从它翻出去」，目标语言看「翻进它」；非英语之间可能还要经 en 枢轴
			bool ok = false;
			for (const auto& other : codes) {
				if (_wcsicmp(code.c_str(), other.c_str()) == 0) continue;
				const bool hit = target ? mgr.canServeOffline(other, code) : mgr.canServeOffline(code, other);
				if (hit) { ok = true; break; }
			}
			if (ok) keep.push_back(code);
		}
		// 一个语言包都没装：保持完整清单，别让下拉空着（实际请求会由服务链回退到在线）
		if (!keep.empty()) codes.swap(keep);
	}

	std::vector<std::wstring> ids, labs;
	if (!target) {   // 源语言最前多一项「自动检测」
		ids.push_back(L"auto");
		labs.push_back(Lang::get(L"setting.fnLangAuto"));
	}
	for (const auto& code : codes) {
		ids.push_back(code);
		labs.push_back(langLabel(code));
	}
	const std::wstring cur = target ? s->getTranslateTargetLang() : s->getTranslateSourceLang();
	// 当前值不在过滤后的清单里（刚切到离线服务，或语言包被卸掉）：仍然列出来，否则下拉
	// 高亮会落到第一项，和按钮上的文字对不上
	bool hasCur = false;
	for (const auto& id : ids)
		if (_wcsicmp(id.c_str(), cur.c_str()) == 0) { hasCur = true; break; }
	if (!hasCur && !cur.empty()) {
		ids.push_back(cur);
		labs.push_back(langLabel(cur));
	}
	int curIdx = 0;
	for (int i = 0; i < (int)ids.size(); i++)
		if (_wcsicmp(ids[i].c_str(), cur.c_str()) == 0) curIdx = i;
	showOptionsBox(btn, labs, curIdx, kLangDdlW, [this, target, ids](int i) {
		if (i < 0 || i >= (int)ids.size()) return;
		auto* st = Setting::get();
		if (target) {
			st->setTranslateTargetLang(ids[i]);
			// 源为中文/自动时，用户手动把目标改成中文 → 锁定，不再自动纠偏
			const std::wstring src = st->getTranslateSourceLang();
			const bool srcZh = isChineseLang(src) || _wcsicmp(src.c_str(), L"auto") == 0;
			if (srcZh && isChineseLang(ids[i])) tgtPinnedZh = true;
			else if (!isChineseLang(ids[i])) tgtPinnedZh = false;
		}
		else {
			st->setTranslateSourceLang(ids[i]);
			ensureTargetWhenSrcZh();
		}
		refreshTopBar();
		});
}

void WinSettingQuickTranslate::refreshProviderBtn()
{
	if (!providerBtn) return;
	int idx = providerIndex(Setting::get()->getTranslateProvider());
	providerVal->setText(Lang::get(kProviders[idx].labKey));
	// 语气只有微软支持：换服务后同步显示/隐藏
	updateToneDdl();
	// 换服务后"当前服务是否在翻"可能就变了 → 发送按钮立刻重算可用态
	updateSendBtn();
	win->refresh();
}

// 语气下拉的显示与取值：仅微软在线翻译显示
void WinSettingQuickTranslate::updateToneDdl()
{
	if (!toneBtn) return;
	const bool ms = _wcsicmp(Setting::get()->getTranslateProvider().c_str(),
		TranslateProvider::Microsoft) == 0;
	toneBtn->visual.IsVisible(ms);
	toneBtn->setWidth(ms ? kToneDdlW : 0.f);      // 隐藏时连占位一起去掉
	toneBtn->setMarginLeft(ms ? 8.f : 0.f);
	if (!ms) return;
	toneVal->setText(Lang::get(kTones[toneIndex(Setting::get()->getTranslateMsTone())].labelKey));
}

void WinSettingQuickTranslate::showToneBox(Ling::Button* btn)
{
	std::vector<std::wstring> ids, labs;
	for (int i = 0; i < kToneCount; i++) {
		ids.push_back(kTones[i].id);
		labs.push_back(Lang::get(kTones[i].labelKey));
	}
	const int curIdx = toneIndex(Setting::get()->getTranslateMsTone());
	showOptionsBox(btn, labs, curIdx, kToneDdlW, [this, ids](int i) {
		if (i < 0 || i >= (int)ids.size()) return;
		Setting::get()->setTranslateMsTone(ids[i]);
		updateToneDdl();
		win->refresh();
		});
}

void WinSettingQuickTranslate::showProviderBox(Ling::Button* btn)
{
	std::vector<std::wstring> ids, labs;
	for (int i = 0; i < kProviderCount; i++) {
		ids.push_back(kProviders[i].id);
		labs.push_back(Lang::get(kProviders[i].labKey));
	}
	int curIdx = providerIndex(Setting::get()->getTranslateProvider());
	showOptionsBox(btn, labs, curIdx, kProviderDdlW, [this, ids](int i) {
		if (i < 0 || i >= (int)ids.size()) return;
		Setting::get()->setTranslateProvider(ids[i]);
		// 换服务不取消正在跑的那条请求：它继续尝试、结果照常回填自己的气泡；
		// 只是发送按钮改成按**新服务**是否在翻来决定可用态（不同服务可以并行）。
		refreshProviderBtn();
		});
}

// ============ 翻译 ============

Ling::Label* WinSettingQuickTranslate::makeSelectableText(Ling::Node* parent,
	const std::wstring& text, float fontSize, uint32_t color)
{
	if (!parent) return nullptr;
	auto* label = parent->makeChild<Ling::Label>();
	label->setFontSize(fontSize);
	label->setColor(color);
	label->setSelectable(true);
	label->setText(text);
	return label;
}

Ling::Node* WinSettingQuickTranslate::appendBubble(const std::wstring& role, const std::wstring& text)
{
	// 一条消息 = block（整宽，Column）：上面右/左对齐的气泡行，下面挂释义卡（字典模式）
	auto block = msgList->makeChild<Ling::Node>();
	block->setWidthPercent(100.f);
	block->setFlexDirection(Ling::FlexDirection::Column);
	block->setMarginBottom(8.f);

	auto row = block->makeChild<Ling::Node>();
	row->setWidthPercent(100.f);
	row->setFlexDirection(Ling::FlexDirection::Row);
	row->setJustifyContent(role == L"user" ? Ling::Justify::End : Ling::Justify::Start);
	// 译文行的复制按钮贴在气泡右侧，底部与气泡对齐
	if (role != L"user") row->setAlignItems(Ling::Align::FlexEnd);

	auto bubble = row->makeChild<Ling::Node>();
	bubble->setFlexDirection(Ling::FlexDirection::Column);
	bubble->setAlignItems(Ling::Align::FlexStart);
	bubble->setPadding(8.f, 10.f, 8.f, 10.f);
	bubble->setBorderRadius(SettingTheme::radius);
	if (role == L"user") {
		bubble->setBg(SettingTheme::primary);
	}
	else {
		bubble->setBg(SettingTheme::secondary);
	}

	// 译文 + 开了字典模式 → 词元流（英文单词可点），否则单标签
	if (role != L"user" && dictModeOn()) {
		buildResultContent(bubble, block, text);
	}
	else {
		makeSelectableText(bubble, wrapText(text, kBubbleMaxW), 13.f,
			role == L"user" ? SettingTheme::primaryForeground : SettingTheme::textPrimary);
	}
	// 译文气泡带复制按钮；「翻译中…」的占位气泡等结果回来再挂（见 updateBubble）
	if (role != L"user" && text != Lang::get(L"quick.translating"))
		appendCopyButton(bubble, text);
	scrollToBottom();
	updateWelcome();
	return bubble;
}

void WinSettingQuickTranslate::updateBubble(Ling::Node* bubble, const std::wstring& text)
{
	if (!bubble) return;
	// 消息 block = row(气泡) 的父节点
	Ling::Node* block = (bubble->parent ? bubble->parent->parent : nullptr);
	// 内容重建 → 挂在该条上的释义卡一并失效
	if (dictCard && block && dictCard->parent == block) resetDictState();
	// 旧的复制按钮是气泡的兄弟节点（row 的第二个子节点），一并摘掉，避免反复回填时堆叠
	if (auto* row = bubble->parent) {
		while (row->children.size() > 1)
			row->removeChild(row->children.back().get());
	}
	while (!bubble->children.empty())
		bubble->removeChild(bubble->children.back().get());
	dictViews.erase(std::remove_if(dictViews.begin(), dictViews.end(),
		[&](const DictTextView& v) { return v.block == block; }), dictViews.end());
	if (dictModeOn() && block) {
		buildResultContent(bubble, block, text);
	}
	else {
		makeSelectableText(bubble, wrapText(text, kBubbleMaxW), 13.f,
			SettingTheme::textPrimary);
	}
	appendCopyButton(bubble, text);
	scrollToBottom();
}

// 译文气泡右侧的复制按钮：默认隐藏，鼠标停悬到这条结果所在行才显示。
// 按钮是气泡的兄弟节点（都在 row 里），所以不会影响气泡自身的宽度；回填时随气泡一起重建。
void WinSettingQuickTranslate::appendCopyButton(Ling::Node* bubble, const std::wstring& text)
{
	if (!bubble || text.empty()) return;
	auto* row = bubble->parent;
	if (!row) return;
	auto* copy = row->makeChild<Ling::Button>();
	copy->setText(Icon::Copy);
	copy->setFontFamily(Icon::Family);
	copy->setFontSize(15.f);
	copy->setWidth(28.f);
	copy->setHeight(28.f);
	copy->setMarginLeft(8.f);       // 与结果气泡留出间距
	copy->setMarginBottom(2.f);
	copy->setFlexShrink(0.f);
	copy->setBorderRadius(SettingTheme::radiusCtl);   // 圆角方形，不用 radiusFull 的圆形
	copy->setBg(SettingTheme::card);
	copy->setBorder(1.f, SettingTheme::border);
	copy->setColor(SettingTheme::textMuted);
	copy->setHoverBg(SettingTheme::accent);
	copy->setHoverColor(SettingTheme::textPrimary);
	// 不走 hide()（那会 display:none 退出布局）；只隐藏绘制，保留 28px 命中区域
	copy->visual.IsVisible(false);

	auto weak = getWeakThis();
	auto btnWeak = copy->getWeakThis();
	copy->onClick.add([weak, btnWeak, copy, text](Ling::Button*) {
		if (!weak.lock() || !btnWeak.lock()) return;
		Ling::Util::setTextToClipboard(text);
		if (auto* s = Setting::get()) s->markOwnClipboardWrite();
		copy->setText(Icon::CopyOk);
		copy->setColor(0x34C759FF);
		copy->setHoverColor(0x34C759FF);
		// 1.2s 后恢复图标；按钮/页面已销毁时 weak 为空，直接跳过
		std::thread([btnWeak, copy]() {
			std::this_thread::sleep_for(std::chrono::milliseconds(1200));
			// 页面先销毁时按钮 weak 已过期，先拦掉，避免退出阶段再去碰 App/调度队列
			if (!btnWeak.lock()) return;
			if (auto* app = Ling::App::get()) {
				app->dq.TryEnqueue([btnWeak, copy]() {
					if (!btnWeak.lock()) return;
					copy->setText(Icon::Copy);
					copy->setColor(SettingTheme::textMuted);
					copy->setHoverColor(SettingTheme::textPrimary);
				});
			}
		}).detach();
	});
	copyHosts.push_back({ bubble, copy, bubble->getWeakThis(), copy->getWeakThis() });
}

// ============ 字典模式（单击译文英文单词，于该条下方展开释义） ============

bool WinSettingQuickTranslate::dictModeOn() const
{
	return Setting::get()->getTranslateDictMode();
}

void WinSettingQuickTranslate::buildResultContent(Ling::Node* bubble, Ling::Node* block,
	const std::wstring& text)
{
	if (!bubble) return;
	const float dpi = win->dpi;
	const float fs = 13.f;
	const float maxLineW = (kBubbleMaxW - 16.f) * dpi;   // 气泡左右内边距各 8

	// 归一化换行（自行折行，故把 \r\n 视作空格）
	std::wstring plain;
	plain.reserve(text.size());
	for (wchar_t c : text) plain += (c == L'\r' || c == L'\n') ? L' ' : c;

	// 分词：连续的「字母/数字/'/-」为一个词元；空白单独成段；其余标点成段。
	// 空白必须单独拿出来做定宽占位：空白若放在 Label 末尾，DWrite 会把它当行尾空白
	// 裁掉，视觉上相邻的两个单词就粘在一起（离线英文结果最常见）。
	enum class TokKind { Word, Space, Other };
	struct Tok { std::wstring s; TokKind kind; };
	auto isWordChar = [](wchar_t c) {
		return iswalnum(c) || c == L'\'' || c == L'\u2019' || c == L'-';
	};
	auto kindOf = [&](wchar_t c) -> TokKind {
		if (isWordChar(c)) return TokKind::Word;
		if (iswspace(c)) return TokKind::Space;
		return TokKind::Other;
	};
	std::vector<Tok> toks;
	for (size_t i = 0, n = plain.size(); i < n;) {
		const TokKind kind = kindOf(plain[i]);
		size_t j = i;
		while (j < n && kindOf(plain[j]) == kind) j++;
		std::wstring s = plain.substr(i, j - i);
		TokKind tk = TokKind::Other;
		if (kind == TokKind::Space) {
			tk = TokKind::Space;
		}
		else if (kind == TokKind::Word) {
			bool alpha = false;
			for (wchar_t c : s) if (iswalpha(c)) { alpha = true; break; }
			if (alpha) tk = TokKind::Word;   // 纯数字不当单词
		}
		toks.push_back({ s, tk });
		i = j;
	}

	auto measure = [&](const std::wstring& s) -> float {
		if (s.empty()) return 0.f;
		auto lay = Ling::D2D::makeTextLayout(s, fs * dpi);
		if (!lay) return 0.f;
		DWRITE_TEXT_METRICS m{};
		if (FAILED(lay->GetMetrics(&m))) return 0.f;
		return m.width;
	};
	// 空格宽度：单独量一个空格可能被当成行尾空白返回 0，用「a a」与「aa」的差值反推。
	const float spaceW = std::max(1.f, measure(L"a a") - measure(L"aa"));

	// 生成可拖选的纯文本，同时记录每个单词在文本中的范围。
	std::wstring out;
	std::vector<DictWordRange> words;
	int wordIndex = -1;
	float lineW = 0.f;
	auto newLine = [&]() {
		if (!out.empty()) out += L'\n';
		lineW = 0.f;
	};

	for (auto& t : toks) {
		if (t.s.empty()) continue;
		if (t.kind == TokKind::Space) {
			if (lineW <= 0.f) continue;
			const float w = spaceW * (float)t.s.size();
			if (lineW + w > maxLineW) {
				newLine();
				continue;
			}
			out += t.s;
			lineW += w;
			continue;
		}
		const bool word = t.kind == TokKind::Word;
		if (word) wordIndex++;
		const float w = measure(t.s);
		if (lineW > 0.f && lineW + w > maxLineW) newLine();
		if (word)
			words.push_back({ (int)out.size(), (int)(out.size() + t.s.size()), wordIndex, t.s });
		out += t.s;
		lineW += w;
	}
	if (out.empty()) return;
	auto* box = makeSelectableText(bubble, out, fs, SettingTheme::textPrimary);
	if (!box) return;
	box->setHandCursor(true);   // 字典模式：悬停译文用点按光标
	dictViews.push_back({ block, box, std::move(words) });
	auto weak = getWeakThis();
	box->onTextClick.add([this, weak, block, box](Ling::Label*, int idx) {
		if (!weak.lock()) return;
		onDictTextClicked(block, box, idx);
	});
}

Ling::Node* WinSettingQuickTranslate::makeDictCard(Ling::Node* block)
{
	if (!block) return nullptr;
	// 挂在【消息 block】下（气泡之下）、整宽：block 宽度确定，卡内长文本的折行与高度都算得准，
	// 不会像挂在自适应宽度的气泡里那样"高度算一行、实际折三行"被裁掉一半。
	auto* card = block->makeChild<Ling::Node>();
	card->setFlexDirection(Ling::FlexDirection::Column);
	card->setAlignItems(Ling::Align::FlexStart);
	card->setWidthPercent(100.f);
	card->setMarginTop(4.f);   // 释义卡与上方译文气泡的间距
	card->setPadding(10.f, 10.f, 10.f, 10.f);
	card->setBg(SettingTheme::popover);
	card->setBorder(1.f, SettingTheme::border);
	card->setBorderRadius(SettingTheme::radiusSm);
	return card;
}

void WinSettingQuickTranslate::setWordActive(Ling::Label* box, int start, int end, bool active)
{
	if (!box) return;
	box->setTextRangeColor(start, end,
		active ? 0x34C759FF : SettingTheme::textPrimary);
}

void WinSettingQuickTranslate::onDictTextClicked(Ling::Node* block, Ling::Label* box, int textIndex)
{
	if (!block || !box) return;
	for (auto& view : dictViews) {
		if (view.box != box) continue;
		for (auto& w : view.words) {
			if (textIndex >= w.start && textIndex < w.end) {
				onDictWordClicked(block, box, w.start, w.end, w.word, w.index);
				return;
			}
		}
		return;
	}
}

void WinSettingQuickTranslate::onDictWordClicked(Ling::Node* block, Ling::Label* box,
	int start, int end, const std::wstring& word, int index)
{
	if (!block || !box) return;

	// 再点同一个词 → 收起
	const bool same = (dictCard && dictCard->parent == block && dictActiveIndex == index &&
		dictActiveBox == box && _wcsicmp(dictActiveWord.c_str(), word.c_str()) == 0);
	if (same) {
		block->removeChild(dictCard);
		setWordActive(box, start, end, false);
		resetDictState();
		win->refresh();
		return;
	}

	// 收起上一条（含“换到别的消息里点词”的情形）
	if (dictCard && dictCard->parent) dictCard->parent->removeChild(dictCard);
	if (dictActiveBox)
		setWordActive(dictActiveBox, dictActiveStart, dictActiveEnd, false);

	dictCard = makeDictCard(block);
	dictActiveBox = box;
	dictActiveStart = start;
	dictActiveEnd = end;
	dictActiveIndex = index;
	dictActiveWord = word;
	setWordActive(box, start, end, true);
	if (dictCard) {
		auto* loading = dictCard->makeChild<Ling::Label>();
		loading->setText(Lang::get(L"setting.dictLoading"));
		loading->setFontSize(12.f);
		loading->setColor(SettingTheme::textMuted);
	}
	win->refresh();
	scrollToBottom();

	// 回调已由 DictLookup 投递回 UI 线程；期间用户可能已改选/收起 → 用当前状态校验
	auto weak = getWeakThis();
	DictLookup::instance().lookup(word, [this, weak, block, box, word, index](const DictEntry& e) {
		if (!weak.lock()) return;
		if (!dictCard || dictCard->parent != block) return;
		if (dictActiveBox != box || dictActiveIndex != index ||
			_wcsicmp(dictActiveWord.c_str(), word.c_str()) != 0) return;
		fillDictCard(dictCard, e);
		win->refresh();
		scrollToBottom();
	});
}

void WinSettingQuickTranslate::fillDictCard(Ling::Node* card, const DictEntry& e)
{
	if (!card) return;
	while (!card->children.empty()) card->removeChild(card->children.back().get());
	auto addLab = [this, card](const std::wstring& s, float sz, uint32_t col, float mt) {
		auto* box = makeSelectableText(card, s, sz, col);
		// 释义卡段落间距收紧到原来的 1/2
		if (box && mt > 0.f) box->setMarginTop(mt * 0.5f);
		};
	if (!e.ok) {
		addLab(e.error.empty() ? Lang::get(L"setting.dictNotFound") : e.error, 12.f,
			SettingTheme::textMuted, 0.f);
		return;
	}
	std::wstring head = e.word;
	if (!e.usPhone.empty()) head += L"    美 /" + e.usPhone + L"/";
	if (!e.ukPhone.empty()) head += L"    英 /" + e.ukPhone + L"/";
	addLab(head, 13.f, SettingTheme::textPrimary, 0.f);
	if (!e.brief.empty()) addLab(e.brief, 12.f, SettingTheme::textPrimary, 4.f);
	for (auto& s : e.senses)
		addLab(s.pos.empty() ? s.meaning : (s.pos + L" " + s.meaning), 12.f,
			SettingTheme::textPrimary, 3.f);
	if (!e.forms.empty()) {
		std::wstring f;
		for (auto& x : e.forms) { if (!f.empty()) f += L"；"; f += x.label + L" " + x.value; }
		addLab(f, 11.f, SettingTheme::textMuted, 4.f);
	}
	for (auto& p : e.phrases)
		addLab(p.zh.empty() ? p.en : (p.en + L"    " + p.zh), 11.f, SettingTheme::textMuted, 3.f);
}

void WinSettingQuickTranslate::resetDictState()
{
	dictCard = nullptr;
	dictActiveBox = nullptr;
	dictActiveStart = 0;
	dictActiveEnd = 0;
	dictActiveWord.clear();
	dictActiveIndex = -1;
}

void WinSettingQuickTranslate::scrollToBottom()
{
	win->refresh();
	if (msgScroll) msgScroll->scrollTo(msgScroll->getMaxScrollY());
}

void WinSettingQuickTranslate::updateWelcome()
{
	if (!welcomeWrap) return;
	Session* s = currentSession();
	// 无会话或无记录 → 显示居中提示语；有记录 → 隐藏
	bool empty = (!s || s->msgs.empty());
	if (empty) welcomeWrap->show();
	else welcomeWrap->hide();
}

void WinSettingQuickTranslate::doTranslate(const std::wstring& preset)
{
	// 请求按服务区分：同一个服务同时只允许一条（按钮已对它置灰）；
	// 换到别的服务可以并行再发，旧请求继续跑、结果回填到自己的气泡。
	const std::wstring provider = Setting::get()->getTranslateProvider();
	if (providerBusy(provider)) return;
	std::wstring text = trim(preset.empty() && input ? input->getText() : preset);
	if (text.empty()) return;
	if (input) input->setText(L"");

	Session* s = currentSession();
	if (!s) { startNewSession(); s = currentSession(); if (!s) return; }

	Msg um;
	um.role = L"user";
	um.content = text;
	s->msgs.push_back(um);
	if (s->title.empty())
		s->title = text.size() > 24 ? text.substr(0, 24) : text;
	appendBubble(L"user", text);

	Msg am;
	am.role = L"assistant";
	am.reqId = nextReqId++;
	s->msgs.push_back(am);
	const int reqId = am.reqId;
	Ling::Node* bubble = appendBubble(L"assistant", Lang::get(L"quick.translating"));
	pending.push_back(PendingReq{ reqId, provider, bubble });
	updateSendBtn();   // 当前服务在翻：按钮置灰（换到别的服务会重新变绿）
	saveSessions();
	refreshHistoryList();

	TranslateRequest req;
	req.text = text;
	req.sourceLang = Setting::get()->getTranslateSourceLang();
	req.targetLang = Setting::get()->getTranslateTargetLang();
	req.provider = provider;
	// 语气只对微软在线翻译有意义
	if (_wcsicmp(provider.c_str(), TranslateProvider::Microsoft) == 0)
		req.tone = Setting::get()->getTranslateMsTone();
	// 回调可能晚于页面销毁（快速切页）：getWeakThis() 哨兵保护，页面已销毁则直接丢弃。
	// 并发请求各自带 reqId：回来只回填自己那条消息与气泡，不会盖掉别人的结果。
	auto weak = getWeakThis();
	TranslateService::instance().translate(req, [this, weak, reqId](const TranslateResult& r) {
		Ling::App::get()->dq.TryEnqueue([this, weak, reqId, r]() {
			if (!weak.lock()) return;
			auto it = std::find_if(pending.begin(), pending.end(),
				[reqId](const PendingReq& p) { return p.id == reqId; });
			if (it == pending.end()) return;   // 这条已不在等待（正常不会发生）
			Ling::Node* bubble = it->bubble;
			pending.erase(it);
			updateSendBtn();
			if (Session* s = currentSession()) {
				for (auto& m : s->msgs) {
					if (m.reqId != reqId) continue;
					m.content = r.ok ? r.text
						: (r.error.empty() ? Lang::get(L"quick.error") : r.error);
					updateBubble(bubble, m.content);
					break;
				}
			}
			scrollToBottom();
			saveSessions();
			refreshHistoryList();
			});
		});
}

// ============ 历史 ============

void WinSettingQuickTranslate::toggleHistory()
{
	histOpen = !histOpen;
	// 展开/折叠图标随侧栏状态切换：收起 →「展开」，展开 →「折叠」
	if (histToggleBtn)
		histToggleBtn->setText(histOpen ? Icon::Collapse : Icon::Expand);
	// 展开时先重新参与布局（折叠态已被 hide() 移出布局树），否则动画第一帧仍按 0 宽计算
	if (histOpen) historyShell->show();
	if (!histAnimRunning) {
		win->setTimer(16, kAnimTimerId);
		histAnimRunning = true;
	}
}

void WinSettingQuickTranslate::animateHistory()
{
	const float step = 28.f;
	histAnimW += histOpen ? step : -step;
	if (histAnimW >= kHistW) {
		histAnimW = kHistW;
		win->killTimer(kAnimTimerId);
		histAnimRunning = false;
	}
	else if (histAnimW <= 0.f) {
		histAnimW = 0.f;
		win->killTimer(kAnimTimerId);
		histAnimRunning = false;
		// 完全折叠：从布局树移除（display:none），否则宽度为 0 的 historyShell 仍会以
		// alignItems:Stretch 撑高 flex 行。
		historyShell->hide();
	}
	historyShell->setWidth(histAnimW);
	// 两卡之间的缝随展开动画同步变宽（折叠 0、展开 16）
	historyShell->setMarginRight(kGapSidebar * histAnimW / kHistW);
	win->refresh();
}

void WinSettingQuickTranslate::refreshHistoryList()
{
	if (!histList) return;
	while (!histList->children.empty()) {
		histList->removeChild(histList->children.back().get());
	}
	histRows.clear();
	std::vector<Session> list = sessions;
	std::sort(list.begin(), list.end(), [](const Session& a, const Session& b) { return a.ts > b.ts; });
	for (auto& s : list) {
		const bool empty = s.title.empty();
		// 空会话也保留在列表中（显示为「新翻译」），切换到其它条目时不会消失
		auto* row = histList->makeChild<Ling::Node>();
		row->setHeight(32.f);   // 与「新建翻译」按钮同高
		row->setWidthPercent(100.f);
		row->setFlexDirection(Ling::FlexDirection::Row);
		row->setAlignItems(Ling::Align::Center);
		row->setMarginBottom(4.f);
		row->setBorderRadius(SettingTheme::radiusSm);
		const bool sel = s.id == curId;
		row->setBg(sel ? SettingTheme::accent : 0);

		// 标题/时间：整行可点（切换会话），不做自己的悬停底色
		auto* item = row->makeChild<Ling::Button>();
		const std::wstring label = empty ? Lang::get(L"quick.untitled") : s.title;
		item->setText(fmtStamp(s.ts) + L"  " + fitTitle(label, 9));
		item->setFlexGrow(1.f);
		item->setFlexShrink(1.f);
		item->setHeightPercent(100.f);
		item->setFlexDirection(Ling::FlexDirection::Row);
		item->setJustifyContent(Ling::Justify::Start);
		item->setAlignItems(Ling::Align::Center);
		item->setPadding(8.f, 0, 8.f, 0);
		item->setBorderRadius(SettingTheme::radiusSm);
		item->setColor(sel ? SettingTheme::textPrimary : SettingTheme::textSecondary);
		item->setHoverBg(0);   // 条目本身不做悬停底色
		item->setHoverColor(sel ? SettingTheme::textPrimary : SettingTheme::textSecondary);   // 悬停不改文字颜色
		std::wstring sid = s.id;
		item->onClick.add([this, sid](Ling::Button*) {
			// 有请求在跑时不切会话：每条请求的结果要回填到它自己的气泡，消息列表不能重建
			if (anyBusy()) return;
			loadSession(sid);
			if (histOpen) toggleHistory();   // 选中后收起侧栏
			});

		// 删除键默认隐藏，悬停该行时显示（见 onMouseMove）
		auto* del = row->makeChild<Ling::Button>();
		del->setText(Icon::Remove);
		del->setFontFamily(Icon::Family);
		del->setFontSize(Icon::DropSize);   // 较行内图标小一档
		del->setWidth(20.f);
		del->setHeight(20.f);
		del->setMarginRight(6.f);   // 与上下留白一致：行高 32、按钮 20，上下各 6
		del->setColor(SettingTheme::textMuted);
		del->setHoverBg(0);   // 悬停不加底色，仅图标变红
		del->setHoverColor(SettingTheme::destructive);
		del->visual.IsVisible(false);
		del->onClick.add([this, sid](Ling::Button*) { deleteSession(sid); });
		histRows.push_back({ row, del });
	}
}

// 删除单个历史条目：删除后立即保存；若删的是当前会话，同时清空消息区
void WinSettingQuickTranslate::deleteSession(const std::wstring& id)
{
	if (anyBusy()) return;
	sessions.erase(std::remove_if(sessions.begin(), sessions.end(),
		[&](const Session& s) { return s.id == id; }), sessions.end());
	if (curId == id) {
		curId.clear();
		while (!msgList->children.empty()) {
			msgList->removeChild(msgList->children.back().get());
		}
	}
	saveSessions();
	refreshHistoryList();
	updateWelcome();
}

void WinSettingQuickTranslate::loadSession(const std::wstring& id)
{
	Session* target = nullptr;
	for (auto& s : sessions) if (s.id == id) { target = &s; break; }
	if (!target) return;
	curId = id;
	// 重建气泡
	while (!msgList->children.empty()) {
		msgList->removeChild(msgList->children.back().get());
	}
	resetDictState();
	dictViews.clear();
	for (auto& m : target->msgs) {
		appendBubble(m.role, m.content);
	}
	refreshHistoryList();
	scrollToBottom();
	updateWelcome();
}

void WinSettingQuickTranslate::startNewSession()
{
	if (anyBusy()) return;
	// 已经有一个空会话（还没输入/输出）时，直接切到它，不再重复新建
	for (auto& s : sessions) {
		if (s.title.empty() && s.msgs.empty()) {
			if (s.id != curId) loadSession(s.id);
			if (histOpen) toggleHistory();   // 新建后收起侧栏
			return;
		}
	}
	Session ns;
	ns.id = newId();
	ns.ts = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
	sessions.push_back(ns);
	curId = ns.id;
	while (!msgList->children.empty()) {
		msgList->removeChild(msgList->children.back().get());
	}
	resetDictState();
	dictViews.clear();
	if (input) input->setText(L"");
	saveSessions();   // 空会话一并保存，重启后仍可回到该条目
	if (histOpen) toggleHistory();   // 新建后收起侧栏
	refreshHistoryList();
	scrollToBottom();
	updateWelcome();
}

void WinSettingQuickTranslate::clearSessions()
{
	if (anyBusy()) return;
	sessions.clear();
	curId.clear();
	while (!msgList->children.empty()) {
		msgList->removeChild(msgList->children.back().get());
	}
	resetDictState();
	dictViews.clear();
	saveSessions();
	refreshHistoryList();
	updateWelcome();
}

void WinSettingQuickTranslate::saveSessions()
{
	JsonArray arr;
	for (auto& s : sessions) {
		JsonObject o;
		o.SetNamedValue(L"id", JsonValue::CreateStringValue(s.id));
		o.SetNamedValue(L"title", JsonValue::CreateStringValue(s.title));
		o.SetNamedValue(L"ts", JsonValue::CreateNumberValue((double)s.ts));
		JsonArray msgs;
		for (auto& m : s.msgs) {
			JsonObject mo;
			mo.SetNamedValue(L"role", JsonValue::CreateStringValue(m.role));
			mo.SetNamedValue(L"content", JsonValue::CreateStringValue(m.content));
			msgs.Append(mo);
		}
		o.SetNamedValue(L"messages", msgs);
		arr.Append(o);
	}
	JsonObject root;
	root.SetNamedValue(L"sessions", arr);
	// 独立存储键：不复用旧「AI 对话」历史（tool "chat"），避免把旧 AI 会话带进新版本
	Setting::get()->setToolStr(L"quickTranslate", L"history", root.Stringify().c_str());
}

void WinSettingQuickTranslate::loadSessions()
{
	sessions.clear();
	std::wstring raw = Setting::get()->getToolStr(L"quickTranslate", L"history", L"");
	JsonObject root{ nullptr };
	if (!raw.empty()) {
		try { root = JsonObject::Parse(raw); }
		catch (...) {}
	}
	if (root) {
		JsonArray arr = root.GetNamedArray(L"sessions", nullptr);
		if (arr) {
			for (auto&& v : arr) {
				JsonObject o = v.GetObjectW();
				Session s;
				s.id = o.GetNamedString(L"id", L"").c_str();
				s.title = o.GetNamedString(L"title", L"").c_str();
				s.ts = (long long)o.GetNamedNumber(L"ts", 0);
				JsonArray msgs = o.GetNamedArray(L"messages", nullptr);
				if (msgs) {
					for (auto&& mv : msgs) {
						JsonObject mo = mv.GetObjectW();
						Msg m;
						m.role = mo.GetNamedString(L"role", L"").c_str();
						m.content = mo.GetNamedString(L"content", L"").c_str();
						s.msgs.push_back(m);
					}
				}
				if (!s.id.empty()) sessions.push_back(std::move(s));
			}
		}
	}
	// 每次打开快捷翻译：按设置决定是否从空白会话开始；关闭时沿用上次会话
	if (Setting::get()->getQuickTranslateNewSession() || sessions.empty()) {
		startNewSession();
	}
	else {
		// 加载最新会话
		Session* latest = nullptr;
		for (auto& s : sessions)
			if (!latest || s.ts > latest->ts) latest = &s;
		if (latest) loadSession(latest->id);
	}
}

// ============ 工具 ============

std::wstring WinSettingQuickTranslate::trim(std::wstring s)
{
	auto a = s.find_first_not_of(L" \t\r\n");
	if (a == std::wstring::npos) return {};
	auto b = s.find_last_not_of(L" \t\r\n");
	return s.substr(a, b - a + 1);
}

std::wstring WinSettingQuickTranslate::wrapText(const std::wstring& text, float maxW)
{
	if (text.empty()) return text;
	auto layout = Ling::D2D::makeTextLayout(text, 13.f * win->dpi, maxW * win->dpi, FLT_MAX);
	if (!layout) return text;
	DWRITE_TEXT_METRICS m{};
	if (FAILED(layout->GetMetrics(&m)) || m.lineCount <= 1) return text;
	std::vector<DWRITE_LINE_METRICS> lines(m.lineCount);
	UINT32 actual = 0;
	if (FAILED(layout->GetLineMetrics(lines.data(), (UINT32)lines.size(), &actual)) || actual == 0)
		return text;
	std::wstring out;
	size_t pos = 0;
	for (UINT32 i = 0; i < actual; i++) {
		if (i) out += L'\n';
		if (lines[i].length > 0) out += text.substr(pos, lines[i].length);
		pos += lines[i].length;
	}
	if (pos < text.size()) out += text.substr(pos);
	return out;
}

WinSettingQuickTranslate::Session* WinSettingQuickTranslate::currentSession()
{
	for (auto& s : sessions)
		if (s.id == curId) return &s;
	return nullptr;
}

std::wstring WinSettingQuickTranslate::newId()
{
	static uint64_t seed = (uint64_t)GetTickCount64() * 2654435761u + 12345u;
	seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
	wchar_t buf[32];
	swprintf_s(buf, L"%016llX", seed);
	return buf;
}
