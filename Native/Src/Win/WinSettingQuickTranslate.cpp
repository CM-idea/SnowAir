#include "pch.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include "../Lang.h"
#include "../Setting.h"
#include "../App.h"
#include "WinSettingQuickTranslate.h"
#include "SettingTheme.h"
#include "SettingWidgets.h"
#include "../Tool/IconCodes.h"
#include "../Translate/TranslateService.h"
#include "../Translate/TranslateTypes.h"
#include "../Translate/DictLookup.h"

namespace {
	constexpr float kBubbleMaxW{ 340.f };      // 气泡最大逻辑宽度
	constexpr float kHistW{ 200.f };           // 历史侧栏展开宽度
	constexpr float kGapSidebar{ 8.f };        // 历史侧栏与主面板的间距
	constexpr UINT kAnimTimerId{ 0x4151 };     // 侧栏动画定时器

	// 时间戳(ms) -> "MM-DD HH:mm"
	std::wstring fmtTime(long long ts)
	{
		time_t t = (time_t)(ts / 1000);
		tm local{};
		localtime_s(&local, &t);
		wchar_t buf[32];
		swprintf_s(buf, L"%02d-%02d %02d:%02d", local.tm_mon + 1, local.tm_mday,
			local.tm_hour, local.tm_min);
		return buf;
	}

	// 下拉宽度：弹层与触发按钮取同一常量，两者始终等宽，避免"弹出项比下拉栏宽/窄"的错位。
	// 语言栏放得下「自动识别语言」，服务栏只需容下四字服务名，故比语言栏窄。
	constexpr float kLangDdlW{ 150.f };
	constexpr float kProviderDdlW{ 100.f };
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
	loadSessions();
}

WinSettingQuickTranslate::~WinSettingQuickTranslate()
{
	hideOptionsBox();
	if (histAnimRunning) win->killTimer(kAnimTimerId);
	win->onTimer.remove(timerTok);
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
		// 当前项：底色取与其它项 hover 相同的浅灰，hover 底色/文字色取同值 → 悬停不产生任何变化。
		it->setColor(SettingTheme::textPrimary);
		it->setBg(sel ? SettingTheme::accent : 0);
		it->setHoverBg(SettingTheme::accent);
		if (sel) it->setHoverColor(SettingTheme::textPrimary);
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
	p->setFlexDirection(Ling::FlexDirection::Row);
	p->setAlignItems(Ling::Align::Stretch);

	// —— 历史侧栏（宽度动画，内容被圆角剪裁） ——
	historyShell = p->makeChild<Ling::Node>();
	historyShell->setWidth(0.f);
	historyShell->setFlexShrink(0.f);
	// 强制侧栏撑满整行高度（= 主面板高度）。否则仅靠 alignItems:Stretch 时，内部
	// histInner(宽200) 的内容会把侧栏高度收缩到内容高，导致灰色底/圆角底部与
	// 输入框底边不对齐（侧栏"不够高"）。heightPercent 100 依赖 host 的可确定高度。
	historyShell->setHeightPercent(100.f);
	historyShell->setBorderRadius(SettingTheme::radius);
	historyShell->setBg(SettingTheme::secondary);
	// 折叠宽度为 0 时 margin 也必须为 0，否则会残留 8px 间距使主面板整体左移，
	// 造成折叠状态下的内容间距与其他内容页不一致。margin 由 animateHistory 随宽度同步。
	historyShell->setMarginRight(0.f);
	// 初始为折叠态：直接从布局树移除（display:none）。否则宽度为 0 的 historyShell 内部
	// histInner(宽200+x 内容) 以 alignItems:Stretch 撑高 flex 行，使折叠态主面板上下间距
	// 比其他内容页更大。
	historyShell->hide();

	histInner = historyShell->makeChild<Ling::Node>();
	histInner->setWidth(kHistW);
	histInner->setHeightPercent(100.f);
	histInner->setFlexDirection(Ling::FlexDirection::Column);
	histInner->setPadding(10.f, 10.f, 10.f, 10.f);

	auto titleRow = histInner->makeChild<Ling::Node>();
	titleRow->setFlexDirection(Ling::FlexDirection::Row);
	titleRow->setWidthPercent(100.f);
	titleRow->setAlignItems(Ling::Align::Center);
	titleRow->setMarginBottom(8.f);
	auto hTitle = titleRow->makeChild<Ling::Label>();
	hTitle->setText(Lang::get(L"quick.history"));
	hTitle->setFontSize(13.f);
	hTitle->setColor(SettingTheme::textPrimary);
	hTitle->setFlexGrow(1.f);
	auto clearBtn = titleRow->makeChild<Ling::Button>();
	clearBtn->setText(Lang::get(L"quick.clear"));
	clearBtn->setHeight(24.f);
	clearBtn->setPadding(8.f, 0, 8.f, 0);
	clearBtn->setBorderRadius(SettingTheme::radiusSm);
	clearBtn->setColor(SettingTheme::textMuted);
	clearBtn->setHoverBg(SettingTheme::accent);
	clearBtn->onClick.add([this](Ling::Button*) { clearSessions(); });

	auto newHistBtn = histInner->makeChild<Ling::Button>();
	newHistBtn->setText(Lang::get(L"quick.new"));
	newHistBtn->setHeight(32.f);
	newHistBtn->setWidthPercent(100.f);
	newHistBtn->setBorderRadius(SettingTheme::radiusSm);
	newHistBtn->setBorder(1.f, SettingTheme::primary);
	newHistBtn->setBg(SettingTheme::primary);
	newHistBtn->setColor(SettingTheme::primaryForeground);
	newHistBtn->setHoverBg(SettingTheme::primary);
	newHistBtn->setMarginBottom(8.f);
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

	// 左侧：侧栏切换（展开/折叠图标，随历史侧栏状态切换）+ 源语言 + 互换 + 目标语言
	histToggleBtn = topBar->makeChild<Ling::Button>();
	histToggleBtn->setText(Icon::Expand);      // 初始侧栏收起 → 显示「展开」
	histToggleBtn->setHeight(36.f);
	histToggleBtn->setWidth(36.f);
	histToggleBtn->setFontFamily(Icon::Family);
	histToggleBtn->setFontSize(Icon::SizeSm);
	histToggleBtn->setBorderRadius(SettingTheme::radiusSm);
	histToggleBtn->setColor(SettingTheme::textMuted);
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
	srcLangBtn->setHoverBg(SettingTheme::accent);
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
	tgtLangBtn->setHoverBg(SettingTheme::accent);
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
	inputBox->setBg(SettingTheme::cardBg);
	inputBox->setBorder(1.f, SettingTheme::input);
	inputBox->setBorderRadius(SettingTheme::radiusXl);
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
	providerBtn->setHoverBg(SettingTheme::accent);
	providerBtn->onClick.add([this](Ling::Button* btn) {
		if (optionsPopup) return;
		showProviderBox(btn);
		});

	auto toolGrow = toolRow->makeChild<Ling::Node>();
	toolGrow->setFlexGrow(1.f);

	counterLab = toolRow->makeChild<Ling::Label>();
	counterLab->setText(L"0 / 5000");
	counterLab->setFontSize(11.f);
	counterLab->setColor(SettingTheme::textTertiary);
	counterLab->setFlexShrink(0.f);
	counterLab->setMarginRight(8.f);

	sendBtn = toolRow->makeChild<Ling::Button>();
	sendBtn->setText(Icon::Send2);
	sendBtn->setFontFamily(Icon::Family);
	sendBtn->setFontSize(18.f);
	sendBtn->setWidth(36.f);
	sendBtn->setHeight(36.f);
	sendBtn->setBorderRadius(SettingTheme::radiusFull);
	sendBtn->onClick.add([this](Ling::Button*) {
		if (busy) return;
		doTranslate(L"");
		});
	updateSendBtn();   // 初始（无输入）：灰底黑图标

	// 回车翻译（TextBox 的 VK_RETURN 会插入 '\n'，在 onTextChanged 里捕获）
	auto weak = getWeakThis();
	input->onTextChanged.add([this, weak](Ling::TextBox*, const std::wstring& val) {
		if (!weak.lock()) return;
		updateSendBtn();   // 有无输入 → 切换发送按钮配色
		// 输入字数（Label::setText 内部会刷新窗口）
		if (counterLab)
			counterLab->setText(std::to_wstring(val.size()) + L" / 5000");
		if (val.empty()) return;
		wchar_t last = val.back();
		if (last != L'\r' && last != L'\n') return;
		std::wstring t = val;
		while (!t.empty() && (t.back() == L'\r' || t.back() == L'\n')) t.pop_back();
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

// 发送按钮配色：无输入 = #E5E5E5 底 + 黑图标；有输入 = #0FDC78 底 + 白图标。
// 悬停底色/图标色取与常态同值 —— Button 悬停时靠 hoverBg/hoverColor 落笔，取同值就没有悬停效果。
void WinSettingQuickTranslate::updateSendBtn()
{
	if (!sendBtn) return;
	const bool hasText = input && !input->getText().empty();
	const uint32_t bg = hasText ? 0x0FDC78FF : 0xE5E5E5FF;
	const uint32_t fg = hasText ? 0xFFFFFFFF : 0x000000FF;
	sendBtn->setBg(bg);
	sendBtn->setColor(fg);
	sendBtn->setHoverBg(bg);
	sendBtn->setHoverColor(fg);
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
	std::vector<std::wstring> ids, labs;
	if (!target) {   // 源语言最前多一项「自动检测」
		ids.push_back(L"auto");
		labs.push_back(Lang::get(L"setting.fnLangAuto"));
	}
	for (int i = 0; i < kPopularLangCount; i++) {
		ids.push_back(kPopularLangs[i].code);
		labs.push_back(Lang::get(kPopularLangs[i].labelKey));
	}
	auto* s = Setting::get();
	std::wstring cur = target ? s->getTranslateTargetLang() : s->getTranslateSourceLang();
	int curIdx = 0;
	for (int i = 0; i < (int)ids.size(); i++)
		if (_wcsicmp(ids[i].c_str(), cur.c_str()) == 0) curIdx = i;
	showOptionsBox(btn, labs, curIdx, kLangDdlW, [this, target, ids](int i) {
		if (i < 0 || i >= (int)ids.size()) return;
		auto* s = Setting::get();
		if (target) {
			s->setTranslateTargetLang(ids[i]);
			// 源为中文/自动时，用户手动把目标改成中文 → 锁定，不再自动纠偏
			const std::wstring src = s->getTranslateSourceLang();
			const bool srcZh = isChineseLang(src) || _wcsicmp(src.c_str(), L"auto") == 0;
			if (srcZh && isChineseLang(ids[i])) tgtPinnedZh = true;
			else if (!isChineseLang(ids[i])) tgtPinnedZh = false;
		}
		else {
			s->setTranslateSourceLang(ids[i]);
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
	win->refresh();
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
		refreshProviderBtn();
		});
}

// ============ 翻译 ============

void WinSettingQuickTranslate::appendBubble(const std::wstring& role, const std::wstring& text)
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
		pendingBubble = bubble;
		pendingText = nullptr;
	}
	else {
		auto lab = bubble->makeChild<Ling::Label>();
		lab->setFontSize(13.f);
		lab->setColor(role == L"user" ? SettingTheme::primaryForeground : SettingTheme::textPrimary);
		lab->setText(wrapText(text, kBubbleMaxW));
		if (role != L"user") {
			pendingBubble = bubble;
			pendingText = lab;
		}
		else {
			pendingBubble = nullptr;
			pendingText = nullptr;
		}
	}
	scrollToBottom();
	updateWelcome();
}

void WinSettingQuickTranslate::updatePendingBubble(const std::wstring& text)
{
	if (!pendingBubble) return;
	// 消息 block = row(气泡) 的父节点
	Ling::Node* block = (pendingBubble->parent ? pendingBubble->parent->parent : nullptr);
	// 内容重建 → 挂在该条上的释义卡一并失效
	if (dictCard && block && dictCard->parent == block) resetDictState();
	while (!pendingBubble->children.empty())
		pendingBubble->removeChild(pendingBubble->children.back().get());
	pendingText = nullptr;
	if (dictModeOn() && block) {
		buildResultContent(pendingBubble, block, text);
	}
	else {
		auto lab = pendingBubble->makeChild<Ling::Label>();
		lab->setFontSize(13.f);
		lab->setColor(SettingTheme::textPrimary);
		lab->setText(wrapText(text, kBubbleMaxW));
		pendingText = lab;
	}
	scrollToBottom();
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

	// 分词：连续的「字母/数字/'/-」为一个词元；其余（空白、标点）为普通文本
	struct Tok { std::wstring s; bool word; };
	auto isWordChar = [](wchar_t c) {
		return iswalnum(c) || c == L'\'' || c == L'\u2019' || c == L'-';
	};
	std::vector<Tok> toks;
	for (size_t i = 0, n = plain.size(); i < n;) {
		const bool word = isWordChar(plain[i]);
		size_t j = i;
		while (j < n && isWordChar(plain[j]) == word) j++;
		std::wstring s = plain.substr(i, j - i);
		bool alpha = false;
		if (word) for (wchar_t c : s) if (iswalpha(c)) { alpha = true; break; }
		toks.push_back({ s, word && alpha });   // 纯数字不当单词
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

	// 逐词手动折行：按行铺（行内 Row），保证气泡仍是自适应宽度（不用 flexWrap）
	auto* flow = bubble->makeChild<Ling::Node>();
	flow->setFlexDirection(Ling::FlexDirection::Column);
	flow->setAlignItems(Ling::Align::FlexStart);

	// 释义卡不在这里预建：点词时按需新建在该条消息的 block 下（见 makeDictCard）。
	// 预建一个隐藏空卡会在"点第二次收起"时闪出一帧只有内边距+描边的空框。

	Ling::Node* line = nullptr;
	float lineW = 0.f;
	int wordIndex = -1;
	auto newLine = [&]() {
		line = flow->makeChild<Ling::Node>();
		line->setFlexDirection(Ling::FlexDirection::Row);
		line->setAlignItems(Ling::Align::Center);
		lineW = 0.f;
	};

	for (auto& t : toks) {
		if (t.s.empty()) continue;
		if (t.word) wordIndex++;
		float w = measure(t.s);
		if (!line) newLine();
		if (lineW > 0.f && lineW + w > maxLineW) newLine();
		std::wstring s = t.s;
		if (lineW == 0.f && !t.word) {   // 行首空白裁掉
			size_t a = s.find_first_not_of(L" \t");
			if (a == std::wstring::npos) continue;
			s = s.substr(a);
		}
		if (t.word)
		{
			auto* b = line->makeChild<Ling::Button>();
			b->setText(s);
			b->setFontSize(fs);
			b->setColor(SettingTheme::textPrimary);
			b->setPadding(0, 0, 0, 0);
			b->setBorder(0.f, 0);
			b->setBorderRadius(0.f);
			b->setBg(0);
			b->setHoverBg(SettingTheme::accent);
			b->setHoverColor(SettingTheme::textPrimary);
			const int idx = wordIndex;
			auto weak = getWeakThis();
			b->onClick.add([this, weak, idx, s, block, b](Ling::Button*) {
				if (!weak.lock()) return;
				onDictWordClicked(block, b, s, idx);
			});
		}
		else
		{
			auto* l = line->makeChild<Ling::Label>();
			l->setText(s);
			l->setFontSize(fs);
			l->setColor(SettingTheme::textPrimary);
		}
		lineW += w;
	}
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

void WinSettingQuickTranslate::setWordActive(Ling::Button* btn, bool active)
{
	if (!btn) return;
	// 选中：加粗 + 绿色；常态：常规字重 + 正文色。hover 色取与常态同值，避免悬停时配色突变。
	const uint32_t c = active ? 0x34C759FF : SettingTheme::textPrimary;
	btn->setFontWeight(active ? 700 : 400);
	btn->setColor(c);
	btn->setHoverColor(c);
}

void WinSettingQuickTranslate::onDictWordClicked(Ling::Node* block, Ling::Button* btn,
	const std::wstring& word, int index)
{
	if (!block || !btn) return;

	// 再点同一个词 → 收起
	const bool same = (dictCard && dictCard->parent == block && dictActiveIndex == index &&
		_wcsicmp(dictActiveWord.c_str(), word.c_str()) == 0);
	if (same) {
		// 卡片子树里没有订阅窗口事件的节点（纯 Node + Label），可同步销毁；
		// 且它是被点按钮所在行的兄弟节点，销毁它不会动到正在派发事件的按钮本身。
		block->removeChild(dictCard);
		setWordActive(btn, false);
		resetDictState();
		win->refresh();
		return;
	}

	// 收起上一条（含"换到别的消息里点词"的情形）
	if (dictCard && dictCard->parent) dictCard->parent->removeChild(dictCard);
	if (dictActiveBtn && dictActiveBtn != btn) setWordActive(dictActiveBtn, false);

	dictCard = makeDictCard(block);
	dictActiveBtn = btn;
	dictActiveIndex = index;
	dictActiveWord = word;
	setWordActive(btn, true);
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
	DictLookup::instance().lookup(word, [this, weak, block, word, index](const DictEntry& e) {
		if (!weak.lock()) return;
		if (!dictCard || dictCard->parent != block) return;
		if (dictActiveIndex != index || _wcsicmp(dictActiveWord.c_str(), word.c_str()) != 0) return;
		fillDictCard(dictCard, e);
		win->refresh();
		scrollToBottom();
	});
}

void WinSettingQuickTranslate::fillDictCard(Ling::Node* card, const DictEntry& e)
{
	if (!card) return;
	while (!card->children.empty()) card->removeChild(card->children.back().get());
	auto addLab = [card](const std::wstring& s, float sz, uint32_t col, float mt) {
		auto* l = card->makeChild<Ling::Label>();
		l->setText(s);
		l->setFontSize(sz);
		l->setColor(col);
		if (mt > 0.f) l->setMarginTop(mt);
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
	dictActiveBtn = nullptr;
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
	if (busy) return;
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
	s->msgs.push_back(am);
	appendBubble(L"assistant", Lang::get(L"quick.translating"));
	busy = true;
	saveSessions();
	refreshHistoryList();

	TranslateRequest req;
	req.text = text;
	req.sourceLang = Setting::get()->getTranslateSourceLang();
	req.targetLang = Setting::get()->getTranslateTargetLang();
	req.provider = Setting::get()->getTranslateProvider();
	// 回调可能晚于页面销毁（快速切页）：getWeakThis() 哨兵保护，页面已销毁则直接丢弃。
	auto weak = getWeakThis();
	TranslateService::instance().translate(req, [this, weak](const TranslateResult& r) {
		Ling::App::get()->dq.TryEnqueue([this, weak, r]() {
			if (!weak.lock() || !busy) return;
			busy = false;
			Session* s = currentSession();
			if (s && !s->msgs.empty() && s->msgs.back().role == L"assistant") {
				s->msgs.back().content = r.ok ? r.text
					: (r.error.empty() ? Lang::get(L"quick.error") : r.error);
				updatePendingBubble(s->msgs.back().content);
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
	// margin 随宽度同步缩放，保证折叠(宽度0)时 margin=0、展开时 margin=8。
	historyShell->setMarginRight(kGapSidebar * histAnimW / kHistW);
	win->refresh();
}

void WinSettingQuickTranslate::refreshHistoryList()
{
	if (!histList) return;
	while (!histList->children.empty()) {
		histList->removeChild(histList->children.back().get());
	}
	std::vector<Session> list = sessions;
	std::sort(list.begin(), list.end(), [](const Session& a, const Session& b) { return a.ts > b.ts; });
	for (auto& s : list) {
		auto* item = histList->makeChild<Ling::Button>();
		std::wstring title = s.title.empty() ? Lang::get(L"quick.untitled") : s.title;
		item->setText(title + L"\n" + fmtTime(s.ts));
		item->setHeight(52.f);
		item->setWidthPercent(100.f);
		item->setFlexDirection(Ling::FlexDirection::Row);
		item->setJustifyContent(Ling::Justify::Start);
		item->setAlignItems(Ling::Align::Center);
		item->setPadding(8.f, 0, 8.f, 0);
		item->setMarginBottom(4.f);
		item->setBorderRadius(SettingTheme::radiusSm);
		bool sel = s.id == curId;
		item->setColor(sel ? SettingTheme::primaryForeground : SettingTheme::textPrimary);
		item->setBg(sel ? SettingTheme::primary : 0);
		item->setHoverBg(sel ? SettingTheme::primary : SettingTheme::accent);
		std::wstring sid = s.id;
		item->onClick.add([this, sid](Ling::Button*) {
			if (busy) return;
			loadSession(sid);
			});
	}
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
	pendingBubble = nullptr; pendingText = nullptr;
	resetDictState();
	for (auto& m : target->msgs) {
		appendBubble(m.role, m.content);
	}
	refreshHistoryList();
	scrollToBottom();
	updateWelcome();
}

void WinSettingQuickTranslate::startNewSession()
{
	if (busy) return;
	if (auto* s = currentSession()) {
		if (s->msgs.empty() && s->title.empty()) return;  // 空会话不重复建
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
	pendingBubble = nullptr; pendingText = nullptr;
	resetDictState();
	if (input) input->setText(L"");
	refreshHistoryList();
	scrollToBottom();
	updateWelcome();
}

void WinSettingQuickTranslate::clearSessions()
{
	if (busy) return;
	sessions.clear();
	curId.clear();
	while (!msgList->children.empty()) {
		msgList->removeChild(msgList->children.back().get());
	}
	pendingBubble = nullptr; pendingText = nullptr;
	resetDictState();
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
	// 直接新建一条空记录：快捷翻译没有"自动创建新会话"之类的开关
	if (sessions.empty()) {
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
