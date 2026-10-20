#include "pch.h"
#include <commdlg.h>
#include "../Win/AnnotHost.h"
#include "../Win/WinCap.h"
#include "../Win/CutMask.h"
#include "../Lang.h"
#include "../Setting.h"
#include "../Tip.h"
#include "ToolSub.h"
#include "ToolCap.h"
#include "ToolColorPanel.h"
#include "ToolNestedPanel.h"
#include "IconCodes.h"
#include "ToolbarChrome.h"
#include "ToolbarTheme.h"
#include "PropSlider.h"
#include "SerialEmoji.h"

using namespace Microsoft::WRL;

namespace {
	struct SliderCfg {
		const wchar_t* key;
		float min, max, def;
	};
	const std::pair<const wchar_t*, SliderCfg> sliderCfgs[]{
		{ L"rect",      { L"width",     1.f, 64.f,  4.f } },
		{ L"arrow",     { L"width",     1.f, 64.f,  4.f } },
		{ L"pen",       { L"width",     1.f, 64.f,  4.f } },
		{ L"number",    { L"radius",    6.f, 200.f, 24.f } },
		{ L"text",      { L"fontSize", 8.f, 200.f, 24.f } },
		{ L"mosaic",    { L"width",     0.f, 100.f, 30.f } },
		{ L"patina",    { L"width",     0.f, 100.f, 15.f } },
		{ L"watermark", { L"fontSize", 12.f, 200.f, 25.f } },
		{ L"highlight", { L"width",     1.f, 64.f,  4.f } },
		{ L"opacity",   { L"percent",  10.f, 100.f, 100.f } },
	};
	const SliderCfg* findSliderCfg(const std::wstring& tool)
	{
		for (auto& [id, cfg] : sliderCfgs) {
			if (tool == id) return &cfg;
		}
		return nullptr;
	}

	// 线迹标注类工具的默认宽度由「其他外观 → 标注默认值」决定（默认 4）
	float strokeDefaultWidth(const std::wstring& tool, float fallback)
	{
		if (tool != L"rect" && tool != L"arrow" && tool != L"pen" && tool != L"highlight")
			return fallback;
		auto* s = Setting::get();
		return s ? (float)s->getToolDefaultWidth() : fallback;
	}
}

ToolSub::ToolSub(AnnotHost* win) : Ling::WinBase(), win(win)
{
	dpi = win->dpi;
	onDpiChanged.add([this]() { dpiChanged = true; });
	onSizeChanged.add([this]() {
		if (!dpiChanged) return;
		dpiChanged = false;
		refreshSize();
		this->win->layoutTools();
	});
	onKeyDown.add([this](UINT key) {
		// 水印输入框编辑中：快捷键留给 TextBox，勿转发成复制截图
		if (wmTextBox && wmTextBox->isFocused()) {
			if (key == VK_ESCAPE) this->win->forwardKey(key);
			return;
		}
		this->win->forwardKey(key);
	});
	createNativeWindow(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, WS_POPUP);
	colorPanel = std::make_unique<ToolColorPanel>(this);
	nestedPanel = std::make_unique<ToolNestedPanel>(this);
}

ToolSub::~ToolSub()
{
}

void ToolSub::onCreated()
{
	ToolbarTheme::refresh();   // 属性栏建窗时也套用当前工具栏主题
	tip = std::make_unique<Tip>(this);
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(
		Ling::Color(ToolbarTheme::background).getD2DColor(), brushBg.GetAddressOf());
	canvas = body->makeChild<Ling::Canvas>();
	canvas->setPositionType(Ling::Position::Absolute);
	canvas->setSizePercent(100.f, 100.f);
	contentNode = body->makeChild<Ling::Node>();
	contentNode->setPositionType(Ling::Position::Absolute);
	const auto pad = ToolbarChrome::propBubbleContentPad(false);
	contentNode->setPosition(Ling::Edge::Left, ToolbarTheme::shadowPad);
	contentNode->setPosition(Ling::Edge::Right, ToolbarTheme::shadowPad);
	contentNode->setPosition(Ling::Edge::Top, pad.top);
	contentNode->setPosition(Ling::Edge::Bottom, pad.bottom);
	contentNode->setFlexDirection(Ling::FlexDirection::Row);
	contentNode->setAlignItems(Ling::Align::Center);
	contentNode->setPaddingLeft(ToolbarTheme::propPad);
	contentNode->setPaddingRight(ToolbarTheme::propPad);
	onMouseMove.add([this](POINT pos) {
		if (!slider) return;
		const bool onSlider = slider->isPosIn(pos)
			|| (sliderValue && sliderValue->isPosIn(pos));
		if (onSlider != sliderHover) {
			sliderHover = onSlider;
			if (onSlider) {
				tip->showAbove(slider, Lang::get(getSliderTipKey()));
				win->setSizePreviewHold(true);
			}
			else {
				tip->hide(slider);
				win->setSizePreviewHold(false);
			}
		}
	});
}

void ToolSub::beginTool(const std::wstring& id)
{
	closeColorPanel();
	closeNestedPanel();
	tip->hide();
	win->setMainBarAnchorId(L"");   // 默认跟随当前工具（白板栏会再单独指定）
	contentNode->removeAllChildren();
	colorTrigger = nullptr;
	colorTriggerRing = nullptr;
	colorTriggerSwatch = nullptr;
	slider = nullptr;
	sliderValue = nullptr;
	wmTextBox = nullptr;
	fillBtn = nullptr;
	dashBtn = nullptr;
	softBtn = nullptr;
	roundBtn = nullptr;
	lineBtn = nullptr;
	angleBtn = nullptr;
	patinaWmBtn = nullptr;
	emojiTrigger = nullptr;
	boardSwatches.clear();
	boardRings.clear();
	boardAdjustBtn = nullptr;
	boardPickBtn = nullptr;
	boardNoColorBtn = nullptr;
	boardCustomCanvas = nullptr;
	boardOpacityBtn = nullptr;
	recPlayBtn = nullptr;
	recAudioBtn = nullptr;
	recMicBtn = nullptr;
	recGifBtn = nullptr;
	recSaveBtn = nullptr;
	recCancelBtn = nullptr;
	recDoneBtn = nullptr;
	recTimerLabel = nullptr;
	overlayBtn = nullptr;
	overlayBaseLabel = nullptr;
	overlayTopLabel = nullptr;
	overlayFlag = nullptr;
	sizeExtraW = 0.f;
	numberStyleBtns.clear();
	arrowHeadBtns.clear();
	curToolId = id;
	auto cfg = findSliderCfg(id);
	if (!cfg) return;
	curSliderKey = cfg->key;
	sliderMin = cfg->min;
	sliderMax = cfg->max;
	// 会话内记忆；新截图会话 ToolSub 重建后回到默认
	sliderVal = rememberedSlider(id, strokeDefaultWidth(id, cfg->def), cfg->min, cfg->max);
	auto cit = colorIndexMem.find(id);
	selectColorIndex = (cit != colorIndexMem.end() && cit->second < colors.size()) ? cit->second : 0;
	colors[customColorIndex] = sessionCustomColor;
}

float ToolSub::rememberedSlider(const std::wstring& key, float def, float mn, float mx) const
{
	auto it = sliderMem.find(key);
	if (it == sliderMem.end()) return std::clamp(def, mn, mx);
	return std::clamp(it->second, mn, mx);
}

void ToolSub::rememberSlider(const std::wstring& key, float val)
{
	sliderMem[key] = val;
}

void ToolSub::showRectTools()
{
	if (curToolId == L"rect" && hasTools) return;
	beginTool(L"rect");
	if (isRectFill && isRectDash) isRectDash = false;
	initSize(4, true);
	auto ellipseBtn = makeToggleBtn(Icon::Ellipse, &isEllipse, L"tool.ellipse", L"ellipse");
	ellipseBtn->onClick.add([this](Ling::Button*) {
		if (isEllipse) {
			if (nestedPanel && nestedPanel->isOpen()
				&& nestedPanel->kind() == ToolNestedPanel::Kind::Corner)
				closeNestedPanel();
			refreshNestedTrigger();
		}
	});
	makeRectStyleBtns();
	initColorTrigger();
	initSlider();
}

void ToolSub::showArrowTools()
{
	beginTool(L"arrow");
	isArrowFill = true;
	if (arrowHead < ArrowDefault || arrowHead > ArrowAnnot) arrowHead = ArrowDefault;
	// 线段/各种箭头共用 arrow 滑条，不因切子样式改量程
	loadStrokeSlider(L"arrow");
	initSize(6, true);
	// 顺序：大箭头 | 线段 | 双箭头 | 标注 | 虚线 | 圆头
	makeArrowHeadBtn(Icon::ArrowBig, ArrowBig, L"tool.arrowBig");
	lineBtn = contentNode->makeChild<Ling::Button>();
	lineBtn->setText(Icon::Line);
	styleToolbarBtn(lineBtn);
	lineBtn->setFontFamily(Icon::Family);
	lineBtn->setFontSize(Icon::Size);
	applyToggleStyle(lineBtn, isLine);
	tip->bind(lineBtn, Lang::get(L"tool.line"));
	lineBtn->onClick.add([this](Ling::Button* b) {
		isLine = !isLine;
		applyToggleStyle(b, isLine);
		if (isLine) {
			arrowHead = ArrowDefault;
			refreshArrowHeadBtns();
		}
		win->onToolStyleChanged();
	});
	makeArrowHeadBtn(Icon::ArrowBoth, ArrowBoth, L"tool.arrowBoth");
	makeArrowHeadBtn(Icon::AnnotBoth, ArrowAnnot, L"tool.arrowAnnot");
	refreshArrowHeadBtns();
	makeToggleBtn(Icon::Dash, &isArrowDash, L"tool.dash", L"dash");
	makeToggleBtn(Icon::RoundCap, &isArrowRound, L"tool.roundCap", L"round");
	initColorTrigger();
	initSlider();
}

void ToolSub::setPenSlotMode(const std::wstring& slotId)
{
	if (slotId == L"laser") {
		isPenFade = true;
		isPenSoft = false;
		isPenDash = false;
	}
	else if (slotId == L"pen") {
		isPenFade = false;
	}
}

void ToolSub::showPenTools()
{
	beginTool(L"pen");
	if (isPenSoft && isPenDash) isPenDash = false;
	if (isPenFade && (isPenSoft || isPenDash)) {
		isPenSoft = false;
		isPenDash = false;
	}
	// 渐隐画笔属性栏只有「颜色 + 粗细滑条 + 数值」；渐隐画笔是全屏画布的
	// 专属工具，截图/贴图工具栏不再提供入口，所以画笔属性栏也不放渐隐开关。
	initSize(isPenFade ? 0 : 2, true);

	if (!isPenFade) {
	softBtn = contentNode->makeChild<Ling::Button>();
	softBtn->setText(Icon::SoftPen);
	styleToolbarBtn(softBtn);
	softBtn->setFontFamily(Icon::Family);
	softBtn->setFontSize(Icon::Size);
	applyToggleStyle(softBtn, isPenSoft);
	tip->bind(softBtn, Lang::get(L"tool.softPen"));
	softBtn->onClick.add([this](Ling::Button*) {
		isPenSoft = !isPenSoft;
		if (isPenSoft) {
			isPenFade = false;
			isPenDash = false;
			if (dashBtn) applyToggleStyle(dashBtn, false);
		}
		applyToggleStyle(softBtn, isPenSoft);
		win->onToolStyleChanged();
	});

	dashBtn = contentNode->makeChild<Ling::Button>();
	dashBtn->setText(Icon::Dash);
	styleToolbarBtn(dashBtn);
	dashBtn->setFontFamily(Icon::Family);
	dashBtn->setFontSize(Icon::Size);
	applyToggleStyle(dashBtn, isPenDash);
	tip->bind(dashBtn, Lang::get(L"tool.dash"));
	dashBtn->onClick.add([this](Ling::Button*) {
		isPenDash = !isPenDash;
		if (isPenDash) {
			isPenFade = false;
			isPenSoft = false;
			if (softBtn) applyToggleStyle(softBtn, false);
		}
		applyToggleStyle(dashBtn, isPenDash);
		win->onToolStyleChanged();
	});
	}   // !isPenFade

	initColorTrigger();
	initSlider();
}

void ToolSub::showNumberTools()
{
	beginTool(L"number");
	isNumberFill = true;
	if (numberStyle < NumberDigit || numberStyle > NumberEmoji) numberStyle = NumberDigit;
	if (serialEmoji.empty()) serialEmoji = SerialEmoji::Default;

	const bool emojiMode = numberStyle == NumberEmoji;
	if (emojiMode) {
		curSliderKey = L"emojiSize";
		sliderMin = 20.f;
		sliderMax = 240.f;
		sliderVal = rememberedSlider(L"numberEmoji", 40.f, sliderMin, sliderMax);
		initSize(3, false); // 字母 / Emoji开关 / 当前表情
	}
	else {
		loadStrokeSlider(L"number");
		initSize(2, true);
	}
	makeNumberStyleBtns();
	if (!emojiMode) initColorTrigger();
	initSlider();
}

void ToolSub::showTextTools()
{
	beginTool(L"text");
	initSize(2, true);
	makeToggleBtn(Icon::Bold, &isTextBold, L"tool.bold", L"bold");
	makeToggleBtn(Icon::Italic, &isTextItalic, L"tool.italic", L"italic");
	initColorTrigger();
	initSlider();
}

void ToolSub::showMosaicTools()
{
	beginTool(L"mosaic");
	initSize(1, false, true);
	// 模糊：复合图标 = 模糊2 全不透明 + 模糊1 @25%
	overlayBtn = makeOverlayToggleBtn(Icon::Blur2, Icon::Blur, 0.25f, &isMosaicBlur, L"tool.blur");
	initSlider();
}

void ToolSub::showPatinaTools()
{
	beginTool(L"patina");
	if (patinaPlan < 0 || patinaPlan > 2) patinaPlan = 0;
	if (patinaWmSize < 8) patinaWmSize = 8;
	initSize(2, false, true);
	makeToggleBtn(Icon::GreenScreen, &isPatinaGreen, L"tool.patinaGreen", L"green");
	patinaWmBtn = contentNode->makeChild<Ling::Button>();
	patinaWmBtn->setText(Icon::PatinaWatermark);
	styleToolbarBtn(patinaWmBtn);
	patinaWmBtn->setFontFamily(Icon::Family);
	patinaWmBtn->setFontSize(Icon::Size);
	applyToggleStyle(patinaWmBtn, isPatinaWatermark);
	tip->bind(patinaWmBtn, Lang::get(L"tool.patinaWm"));
	patinaWmBtn->onClick.add([this](Ling::Button*) {
		closeColorPanel();
		nestedPanel->openPatinaWm(workAreaRect(), patinaWmBtn);
	});
	initSlider();
}

void ToolSub::showWatermarkTools()
{
	beginTool(L"watermark");
	constexpr float wmInputMinW{ 96.f };
	constexpr float wmInputMaxW{ 280.f }; // 约 20 个汉字 + 左右 padding
	constexpr size_t wmMaxChars{ 20 };

	auto measureWmW = [this, wmInputMinW, wmInputMaxW](const std::wstring& text) -> float {
		const std::wstring sample = text.empty() ? L"请输入文字..." : text;
		auto layout = Ling::D2D::get()->makeTextLayout(sample, 12.f * dpi);
		float tw = 0.f;
		if (layout) {
			DWRITE_TEXT_METRICS m{};
			layout->GetMetrics(&m);
			tw = m.widthIncludingTrailingWhitespace / dpi;
		}
		return std::clamp(tw + 16.f, wmInputMinW, wmInputMaxW);
	};

	const float slot = Icon::Size + ToolbarTheme::propGap;
	const float marginH = (slot - iconInner) * 0.5f;

	auto syncWmWidth = [this, measureWmW, marginH]() {
		if (!wmTextBox) return;
		const float w = measureWmW(watermarkText);
		wmTextBox->setWidth(w);
		sizeExtraW = w + marginH * 2.f;
		refreshSize();
		win->layoutTools();
	};

	if (watermarkText.size() > wmMaxChars)
		watermarkText.resize(wmMaxChars);
	const float wmInputW = measureWmW(watermarkText);
	initSize(1, true, true, wmInputW + marginH * 2.f);

	angleBtn = contentNode->makeChild<Ling::Button>();
	angleBtn->setText(Icon::Angle);
	styleToolbarBtn(angleBtn);
	angleBtn->setFontFamily(Icon::Family);
	angleBtn->setFontSize(Icon::Size);
	applyToggleStyle(angleBtn, false);
	tip->bind(angleBtn, Lang::get(L"tool.wmAngle"));
	angleBtn->onClick.add([this](Ling::Button*) {
		closeColorPanel();
		nestedPanel->openWmAngle(workAreaRect(), angleBtn);
	});

	// autoSize：单行不折行 → 无滚动条；宽度再钳到 min/max
	wmTextBox = contentNode->makeChild<Ling::TextBox>();
	wmTextBox->setMargin(marginH, 0.f, marginH, 0.f);
	wmTextBox->setFlexGrow(0.f);
	wmTextBox->setFlexShrink(0.f);
	wmTextBox->setBg(Ling::Color(ToolbarTheme::hoverBg));
	wmTextBox->setBorder(0.f, 0);
	wmTextBox->setBorderRadius(6.f);
	wmTextBox->setPadding(8.f, 4.f, 8.f, 4.f);
	wmTextBox->setFontSize(12.f);
	wmTextBox->setColor(Ling::Color(0x333333FFu));
	wmTextBox->setCaretColor(Ling::Color(0x333333FFu));
	wmTextBox->setSelectionBgColor(Ling::Color(0x34C75966u));
	wmTextBox->setPlaceholder(L"请输入文字...");
	wmTextBox->setPlaceholderColor(Ling::Color(0x999999FFu));
	wmTextBox->setAutoSize(true);
	wmTextBox->setText(watermarkText);
	wmTextBox->setWidth(wmInputW); // 空文本时保住占位宽；autoSize 后仍可钳制
	wmTextBox->onTextChanged.add([this, syncWmWidth, wmMaxChars](Ling::TextBox*, const std::wstring& val) {
		auto t = val.size() > wmMaxChars ? val.substr(0, wmMaxChars) : val;
		if (t.size() != val.size() && wmTextBox) {
			wmTextBox->setText(t);
			return;
		}
		if (t == watermarkText) return;
		watermarkText = t;
		syncWmWidth();
		win->onToolStyleChanged();
	});
	initColorTrigger();
	initSlider();
}

void ToolSub::showHighlightTools()
{
	beginTool(L"highlight");
	initSize(2, true, true);
	makeToggleBtn(Icon::Ellipse, &isHighlightEllipse, L"tool.highlightEllipse", L"ellipse");
	makeToggleBtn(Icon::DashRect, &isHighlightBorder, L"tool.highlightBorder", L"border");
	initColorTrigger();
	initSlider();
}

void ToolSub::showEraserTools()
{
	win->shapeHover = nullptr;
	hideTools();
}

void ToolSub::showOpacityTools()
{
	if (curToolId == L"opacity" && hasTools) return;
	beginTool(L"opacity");
	// 初始值取当前贴图画面不透明度（而不是会话记忆值），保证与右键菜单/持久化设置一致
	sliderVal = std::clamp((float)win->pinOpacityPercent(), sliderMin, sliderMax);
	initSize(0, false, true);   // 只有滑条：无图标、无色板
	initSlider();
}

// 演示画布「白板」属性栏：无色 / 调整背景 / 选择背景 + 6 个预设 + 自定义
void ToolSub::showBoardTools()
{
	if (curToolId == L"board" && hasTools) { refreshBoardState(); return; }
	beginTool(L"board");
	// 4 个动作 + 7 个常驻色块；没有滑条，宽度把 initSize 预留给滑条的那段扣掉
	initSize(4 + 7, false, false, -PropSlider::rowLogicW());
	auto* cap = dynamic_cast<WinCap*>(win);

	auto makeIcon = [&](const wchar_t* code, const std::wstring& text,
		std::function<void()> act) {
		auto* b = contentNode->makeChild<Ling::Button>();
		b->setText(code);
		b->setFontFamily(Icon::Family);
		b->setFontSize(Icon::Size);
		styleToolbarBtn(b);
		applyToggleStyle(b, false);
		tip->bind(b, text);
		b->onClick.add([act](Ling::Button*) { act(); });
		return b;
	};
	boardNoColorBtn = makeIcon(Icon::NoColor, Lang::get(L"board.noColor"), [cap]() {
		if (cap) cap->demoBoardClear();
	});
	boardAdjustBtn = makeIcon(Icon::AdjustBackground, Lang::get(L"board.adjust"), [this, cap]() {
		if (cap) { cap->demoBoardToggleAdjust(); refreshBoardState(); }
	});
	boardPickBtn = makeIcon(Icon::ImageBackground, Lang::get(L"board.pick"), [cap]() {
		if (cap) cap->demoBoardPickBackground();
	});
	// 透明度：点开二级属性（滑条）；放在图片背景后面，紧挨着色块。图片/PPT 背景时不可用
	boardOpacityBtn = makeIcon(Icon::Opacity, Lang::get(L"board.opacity"), [this, cap]() {
		if (!cap) return;
		if (cap->demoBoardImages() || cap->demoBoardPpt()) return;   // 仅纯色背景可调
		if (nestedPanel && nestedPanel->isOpen()
			&& nestedPanel->kind() == ToolNestedPanel::Kind::BoardOpacity) {
			nestedPanel->closePanel();
			refreshBoardState();
			return;
		}
		closeColorPanel();
		if (nestedPanel) nestedPanel->openBoardOpacity(workAreaRect(), boardOpacityBtn,
			(float)cap->demoBoardOpacity());
		refreshBoardState();
	});

	// 7 个常驻色块：白 / 黑 / 274C43 / EC221F / 34C759 / 1677FF / 自定义
	// 结构与二级色板一致：按钮内一层环，选中时环描边，内侧才是色块
	boardSwatches.clear();
	boardRings.clear();
	auto makeSwatch = [&](UINT32 color, const std::wstring& tipText, bool custom) {
		auto* b = contentNode->makeChild<Ling::Button>();
		const float slot = Icon::Size + ToolbarTheme::propGap;
		b->setSize(iconInner, iconInner);
		b->setMargin((slot - iconInner) * 0.5f, (btnSize - iconInner) * 0.5f,
			(slot - iconInner) * 0.5f, (btnSize - iconInner) * 0.5f);
		b->setFlexGrow(0.f);
		b->setFlexShrink(0.f);
		b->setBorderRadius(ToolbarTheme::hoverRadius);
		b->setHoverBg(ToolbarTheme::hoverBg);
		b->setAlignItems(Ling::Align::Center);
		b->setJustifyContent(Ling::Justify::Center);
		tip->bind(b, tipText);
		b->onClick.add([this, cap, color, custom](Ling::Button*) {
			if (!cap) return;
			if (custom) cap->demoBoardPickColor();
			else cap->demoBoardSetColor(color);
			refreshBoardState();
		});
		auto* ring = b->makeChild<Ling::Node>();
		ring->setSize(ToolbarTheme::swatchRing, ToolbarTheme::swatchRing);
		ring->setBorderRadius(ToolbarTheme::swatchRadius + 1.f);
		ring->setAlignItems(Ling::Align::Center);
		ring->setJustifyContent(Ling::Justify::Center);
		if (custom) {
			// 自定义沿用颜色面板那颗七彩渐变块（同一份绘制）
			auto* wheel = ring->makeChild<Ling::Canvas>();
			wheel->setSize(ToolbarTheme::swatchSize, ToolbarTheme::swatchSize);
			wheel->setBorderRadius(ToolbarTheme::swatchRadius);
			boardCustomCanvas = wheel;
		}
		else {
			auto* dot = ring->makeChild<Ling::Label>();
			dot->setSize(ToolbarTheme::swatchSize, ToolbarTheme::swatchSize);
			dot->setBorderRadius(ToolbarTheme::swatchRadius);
			dot->setBg(color);
			if (color == 0xFFFFFFFF) dot->setBorder(1.f, 0xA8A8A8FF);
		}
		boardSwatches.push_back(b);
		boardRings.push_back(ring);
		return b;
	};
	makeSwatch(0xFFFFFFFFu, Lang::get(L"color.white"), false);
	makeSwatch(0x000000FFu, Lang::get(L"color.black"), false);
	makeSwatch(0x274C43FFu, Lang::get(L"color.darkGreen"), false);
	makeSwatch(0xEC221FFFu, Lang::get(L"color.red"), false);
	makeSwatch(0x34C759FFu, Lang::get(L"color.green"), false);
	makeSwatch(0x1677FFFFu, Lang::get(L"color.blue"), false);
	makeSwatch(0u, Lang::get(L"color.custom"), true);
	// 属性栏居中在白板按钮下方（白板不是"当前工具"，不能用画笔的位置）
	win->setMainBarAnchorId(L"whiteboard");
	refreshBoardState();
}

// 演示画布「屏幕录制」属性栏：开始/暂停 · 时间 · 系统声音 · 麦克风 · 动图 · 另存为 · 取消 · 完成
void ToolSub::showDemoRecTools()
{
	if (curToolId == L"demo-rec" && hasTools) { refreshDemoRecState(); return; }
	beginTool(L"demo-rec");
	const float timerW = 62.f;
	// 7 个图标按钮 + 一个定宽时间标签（把 initSize 预留的滑条宽度扣掉再补标签宽）
	initSize(7, false, false, timerW - PropSlider::rowLogicW());
	win->setMainBarAnchorId(L"extra");
	auto* cap = dynamic_cast<WinCap*>(win);

	auto makeIcon = [&](const wchar_t* code, const std::wstring& text,
		std::function<void()> act) {
		auto* b = contentNode->makeChild<Ling::Button>();
		b->setText(code);
		b->setFontFamily(Icon::Family);
		b->setFontSize(Icon::Size);
		styleToolbarBtn(b);
		applyToggleStyle(b, false);
		tip->bind(b, text);
		b->onClick.add([act](Ling::Button*) { act(); });
		return b;
	};

	recPlayBtn = makeIcon(Icon::Play, Lang::get(L"video.recordToggle"),
		[cap]() { if (cap) cap->demoRecToggle(); });
	// 时间轴：只读显示，1 秒一跳（由 WinCap 的计时器驱动刷新）
	{
		auto* slot = contentNode->makeChild<Ling::Node>();
		slot->setSize(timerW, btnSize);
		slot->setFlexGrow(0.f);
		slot->setFlexShrink(0.f);
		slot->setAlignItems(Ling::Align::Center);
		slot->setJustifyContent(Ling::Justify::Center);
		recTimerLabel = slot->makeChild<Ling::Label>();
		recTimerLabel->setFontSize(14.f);
		recTimerLabel->setColor(Icon::ColorNormal);
		recTimerLabel->setText(L"00:00");
	}
	recAudioBtn = makeIcon(Icon::Audio, Lang::get(L"video.recordSystem"),
		[cap]() { if (cap) cap->demoRecToggleAudio(); });
	recMicBtn = makeIcon(Icon::MicOn, Lang::get(L"video.recordMic"),
		[cap]() { if (cap) cap->demoRecToggleMic(); });
	recGifBtn = makeIcon(Icon::Gif, Lang::get(L"video.outputGif"),
		[cap]() { if (cap) cap->demoRecToggleGif(); });
	recSaveBtn = makeIcon(Icon::Save, Lang::get(L"video.saveAs"),
		[cap]() { if (cap) cap->demoRecSaveAs(); });
	recCancelBtn = makeIcon(Icon::Cancel, Lang::get(L"video.cancelRecord"),
		[cap]() { if (cap) cap->demoRecCancel(); });
	recDoneBtn = makeIcon(Icon::Done, Lang::get(L"video.doneRecord"),
		[cap]() { if (cap) cap->demoRecFinish(); });
	if (recCancelBtn) {
		recCancelBtn->setColor(Icon::ColorCancel);
		recCancelBtn->setHoverColor(Icon::ColorCancel);
	}
	if (recDoneBtn) {
		recDoneBtn->setColor(Icon::ColorDone);
		recDoneBtn->setHoverColor(Icon::ColorDone);
	}
	refreshDemoRecState();
}

void ToolSub::refreshDemoRecState()
{
	if (curToolId != L"demo-rec" || !hasTools) return;
	auto* cap = dynamic_cast<WinCap*>(win);
	if (!cap) return;
	const bool running = cap->demoRecRunning();
	const bool paused = cap->demoRecPaused();
	const bool gif = cap->demoRecGifMode();

	auto setDisabled = [](Ling::Button* b, bool disabled) {
		if (!b) return;
		if (disabled) {
			b->setBg(0);
			b->setHoverBg(0);
			b->setColor(Icon::ColorDisabled);
			b->setHoverColor(Icon::ColorDisabled);
		}
	};

	// 播放/暂停：录到一半显示暂停图标 + 选中态
	if (recPlayBtn) {
		recPlayBtn->setText((running && !paused) ? Icon::Pause : Icon::Play);
		applyToggleStyle(recPlayBtn, running && !paused);
	}
	if (recTimerLabel) {
		const int sec = running ? cap->demoRecElapsed() : 0;
		recTimerLabel->setText(std::format(L"{:02d}:{:02d}", sec / 60, sec % 60));
	}
	// 音频 / 麦克风：动图模式与录制中都不可改（GIF 无音轨）
	if (recAudioBtn) {
		applyToggleStyle(recAudioBtn, cap->demoRecAudioOn());
		setDisabled(recAudioBtn, running || gif);
	}
	if (recMicBtn) {
		applyToggleStyle(recMicBtn, cap->demoRecMicOn());
		setDisabled(recMicBtn, running || gif);
	}
	// 动图：开录后锁定
	if (recGifBtn) {
		applyToggleStyle(recGifBtn, gif);
		setDisabled(recGifBtn, running);
	}
	// 未录制时另存为/取消/完成没有意义，置灰（点击仍由 WinCap 拦下）
	const bool idle = !running;
	setDisabled(recSaveBtn, idle);
	setDisabled(recCancelBtn, idle);
	setDisabled(recDoneBtn, idle);
	if (!idle) {
		if (recSaveBtn) {
			recSaveBtn->setColor(Icon::ColorNormal);
			recSaveBtn->setHoverColor(Icon::ColorNormal);
		}
		if (recCancelBtn) {
			recCancelBtn->setColor(Icon::ColorCancel);
			recCancelBtn->setHoverColor(Icon::ColorCancel);
		}
		if (recDoneBtn) {
			recDoneBtn->setColor(Icon::ColorDone);
			recDoneBtn->setHoverColor(Icon::ColorDone);
		}
	}
}

void ToolSub::setBoardOpacity(int percent)
{
	if (auto* cap = dynamic_cast<WinCap*>(win)) cap->demoBoardSetOpacity(percent);
	refreshBoardState();
}

void ToolSub::refreshBoardState()
{
	auto* cap = dynamic_cast<WinCap*>(win);
	if (!cap) return;
	const bool ppt = cap->demoBoardPpt();
	// 当前背景是图片/PPT 时：「选择背景」按钮保持选中，7 个色块全部取消选中
	const bool media = ppt || cap->demoBoardImages();
	if (boardPickBtn) applyToggleStyle(boardPickBtn, media);
	// 透明度：100 时取消选中，小于 100 显示选中态；图片/PPT 时置灰不可用
	if (boardOpacityBtn) {
		const bool opacityOn = !media && cap->demoBoardOpacity() < 100;
		applyToggleStyle(boardOpacityBtn, opacityOn);
		if (media) boardOpacityBtn->setColor(Icon::ColorDisabled);
	}
	if (media && nestedPanel && nestedPanel->isOpen()
		&& nestedPanel->kind() == ToolNestedPanel::Kind::BoardOpacity)
		nestedPanel->closePanel();
	if (boardAdjustBtn) {
		applyToggleStyle(boardAdjustBtn, !ppt && cap->demoBoardAdjusting());
		// PPT 放映时调背景不可用：置灰（点击仍会被 WinCap 拦下）
		if (ppt) boardAdjustBtn->setColor(Icon::ColorDisabled);
	}
	static constexpr UINT32 kPresets[6] = {
		0xFFFFFFFFu, 0x000000FFu, 0x274C43FFu, 0xEC221FFFu, 0x34C759FFu, 0x1677FFFFu
	};
	int sel = -1;
	if (!media) {
		const UINT32 cur = cap->demoBoardColor();
		for (int i = 0; i < 6; ++i) {
			if (kPresets[i] == cur) { sel = i; break; }
		}
		if (sel < 0) sel = 6;   // 非预设色算作自定义（色块本身是七彩渐变，不跟色变化）
	}
	for (size_t i = 0; i < boardRings.size(); ++i) {
		if (!boardRings[i]) continue;
		const bool active = ((int)i == sel);
		boardRings[i]->setBorder(active ? 1.f : 0.f, active ? Icon::ColorActive : 0);
	}
}

float ToolSub::setShapeSliderVal(const std::wstring& tool, float px)
{
	if (tool == L"number" && numberStyle == NumberEmoji) {
		auto logical = std::clamp(px / dpi, 20.f, 240.f);
		rememberSlider(L"numberEmoji", logical);
		if (curToolId == tool) {
			sliderVal = logical;
			updateSliderValueLabel();
			if (slider) {
				suppressSliderStyleNotify = true;
				slider->setValue(logical);
				suppressSliderStyleNotify = false;
			}
		}
		return logical * dpi;
	}
	auto cfg = findSliderCfg(tool);
	if (!cfg) return px;
	auto logical = std::clamp(px / dpi, cfg->min, cfg->max);
	rememberSlider(tool, logical);
	if (curToolId == tool) {
		sliderVal = logical;
		updateSliderValueLabel();
		if (slider) {
			suppressSliderStyleNotify = true;
			slider->setValue(logical);
			suppressSliderStyleNotify = false;
		}
	}
	return logical * dpi;
}

void ToolSub::layout()
{
	Ling::WinBase::layout();
	if (!canvas) return;
	auto ctx = canvas->startPaint();
	if (!ctx) return;
	ctx->Clear(0);
	paintBorder(ctx);
	canvas->finishPaint();
	// 自定义色块的七彩渐变：与颜色面板共用同一份绘制，尺寸按当前布局重画
	PaintCustomColorSwatch(boardCustomCanvas, dpi);
}

void ToolSub::onMinMaxInfo(MINMAXINFO* mmi)
{
	mmi->ptMinTrackSize.x = 1;
	mmi->ptMinTrackSize.y = 1;
}

void ToolSub::paintBorder(ID2D1DeviceContext* ctx)
{
	ToolbarChrome::paintPropBubble(ctx, w, h, dpi, brushBg.Get(), arrowX, tipDown_);
}

void ToolSub::applyTipDirection(bool tipDown)
{
	tipDown_ = tipDown;
	ToolbarChrome::applyPropBubbleContentPad(contentNode, tipDown_);
}

void ToolSub::initColorTrigger()
{
	colorTrigger = contentNode->makeChild<Ling::Button>();
	styleToolbarBtn(colorTrigger);
	tip->bind(colorTrigger, L"颜色");
	colorTrigger->onClick.add([this](Ling::Button*) { toggleColorPanel(); });
	colorTriggerRing = colorTrigger->makeChild<Ling::Node>();
	colorTriggerRing->setSize(swatchRing, swatchRing);
	colorTriggerRing->setBorderRadius(swatchRadius + 1.f);
	colorTriggerRing->setAlignItems(Ling::Align::Center);
	colorTriggerRing->setJustifyContent(Ling::Justify::Center);
	colorTriggerSwatch = colorTriggerRing->makeChild<Ling::Label>();
	colorTriggerSwatch->setSize(swatchSize, swatchSize);
	colorTriggerSwatch->setBorderRadius(swatchRadius);
	updateColorTriggerSwatch();
}

void ToolSub::updateColorTriggerSwatch()
{
	if (!colorTriggerSwatch) return;
	const auto c = colors[selectColorIndex];
	colorTriggerSwatch->setBg(c);
	if (selectColorIndex == 5 || c == 0xFFFFFFFF) {
		colorTriggerSwatch->setBorder(1.f, 0xA8A8A8FF);
	}
	else {
		colorTriggerSwatch->setBorder(0.f, 0);
	}
}

void ToolSub::toggleColorPanel()
{
	if (!colorPanel) return;
	closeNestedPanel();
	if (colorPanel->isOpen()) {
		closeColorPanel();
		return;
	}
	colorPanel->open(workAreaRect());
	if (colorTriggerRing) {
		colorTriggerRing->setBorder(1.f, Icon::ColorActive);
	}
}

void ToolSub::closeColorPanel()
{
	if (colorPanel) colorPanel->closePanel();
	if (colorTriggerRing) {
		colorTriggerRing->setBorder(0.f, 0);
	}
}

void ToolSub::closeNestedPanel()
{
	if (nestedPanel) nestedPanel->closePanel();
	refreshNestedTrigger();
}

void ToolSub::notifyStyleChanged()
{
	win->onToolStyleChanged();
}

void ToolSub::refreshNestedTrigger()
{
	if (roundBtn) applyToggleStyle(roundBtn, cornerRadius > 0.f);
	if (angleBtn) {
		const bool on = nestedPanel && nestedPanel->isOpen()
			&& nestedPanel->kind() == ToolNestedPanel::Kind::WmAngle;
		applyToggleStyle(angleBtn, on);
	}
	if (patinaWmBtn) applyToggleStyle(patinaWmBtn, isPatinaWatermark);
	refreshNumberStyleBtns();
}

RECT ToolSub::workAreaRect() const
{
	RECT winRect{ x, y, x + static_cast<int>(w), y + static_cast<int>(h) };
	return ToolbarChrome::workAreaNear(winRect);
}

void ToolSub::onColorPicked(size_t index)
{
	if (index >= colors.size()) return;
	if (index == customColorIndex) {
		if (!pickCustomColor()) return;
	}
	else if (index == selectColorIndex) {
		closeColorPanel();
		return;
	}
	selectColorIndex = static_cast<UINT>(index);
	if (!curToolId.empty())
		colorIndexMem[curToolId] = selectColorIndex;
	updateColorTriggerSwatch();
	if (index == customColorIndex) {
		if (colorPanel && colorPanel->isOpen())
			colorPanel->refreshSelection();
	}
	else {
		closeColorPanel();
	}
	win->onToolStyleChanged();
}

void ToolSub::getColorTriggerAnchor(float& screenCenterX, float& screenBottom) const
{
	if (colorTrigger) {
		screenCenterX = static_cast<float>(x) + colorTrigger->x + colorTrigger->w * 0.5f;
		screenBottom = static_cast<float>(y) + colorTrigger->y + colorTrigger->h;
	}
	else {
		screenCenterX = static_cast<float>(x) + w * 0.5f;
		screenBottom = static_cast<float>(y) + h - ToolbarTheme::shadowPad * dpi;
	}
}

bool ToolSub::pickCustomColor()
{
	Ling::Color cur{ colors[customColorIndex] };
	COLORREF rgb = RGB(cur.r, cur.g, cur.b);
	static COLORREF custColors[16]{};
	CHOOSECOLOR cc{};
	cc.lStructSize = sizeof(cc);
	cc.hwndOwner = hwnd;
	cc.lpCustColors = custColors;
	cc.rgbResult = rgb;
	cc.Flags = CC_FULLOPEN | CC_RGBINIT;
	if (!ChooseColorW(&cc)) return false;
	auto r = GetRValue(cc.rgbResult);
	auto g = GetGValue(cc.rgbResult);
	auto b = GetBValue(cc.rgbResult);
	colors[customColorIndex] = (static_cast<UINT32>(r) << 24)
		| (static_cast<UINT32>(g) << 16)
		| (static_cast<UINT32>(b) << 8)
		| 0xFFu;
	sessionCustomColor = colors[customColorIndex];
	return true;
}

void ToolSub::styleToolbarBtn(Ling::Button* btn)
{
	// propGap=16：槽宽 = 字形 + 间距
	const float slot = Icon::Size + ToolbarTheme::propGap;
	const float marginH{ (slot - iconInner) * 0.5f };
	const float marginV{ (btnSize - iconInner) * 0.5f };
	btn->setSize(iconInner, iconInner);
	btn->setMargin(marginH, marginV, marginH, marginV);
	btn->setFlexGrow(0.f);
	btn->setFlexShrink(0.f);
	btn->setBorderRadius(hoverRadius);
	btn->setHoverBg(ToolbarTheme::hoverBg);
	btn->setAlignItems(Ling::Align::Center);
	btn->setJustifyContent(Ling::Justify::Center);
}

void ToolSub::applyToggleStyle(Ling::Button* btn, bool selected)
{
	btn->setBg(0);
	btn->setHoverBg(ToolbarTheme::hoverBg);
	if (selected) {
		btn->setColor(Icon::ColorActive);
		btn->setHoverColor(Icon::ColorActive);
	}
	else {
		btn->setColor(Icon::ColorNormal);
		btn->setHoverColor(Icon::ColorNormal);
	}
}

Ling::Button* ToolSub::makeToggleBtn(const std::wstring& text, bool* flag, const std::wstring& tipKey, const std::wstring& cfgKey)
{
	(void)cfgKey;
	auto btn = contentNode->makeChild<Ling::Button>();
	btn->setText(text);
	styleToolbarBtn(btn);
	btn->setFontFamily(Icon::Family);
	btn->setFontSize(Icon::Size);
	applyToggleStyle(btn, *flag);
	tip->bind(btn, Lang::get(tipKey));
	btn->onClick.add([this, flag](Ling::Button* b) {
		*flag = !*flag;
		applyToggleStyle(b, *flag);
		win->onToolStyleChanged();
	});
	return btn;
}

// 复合图标：一个 Node 里叠两个字形的 Label（双字形叠加）。
// 按钮自带的 Text 留空，命中/悬停仍走 Button 自身矩形。
Ling::Button* ToolSub::makeOverlayToggleBtn(const std::wstring& baseGlyph, const std::wstring& overlayGlyph,
	float alpha, bool* flag, const std::wstring& tipKey)
{
	auto btn = contentNode->makeChild<Ling::Button>();
	styleToolbarBtn(btn);

	auto stack = btn->makeChild<Ling::Node>();
	stack->setPositionType(Ling::Position::Absolute);
	stack->setPosition(Ling::Edge::Left, 0.f);
	stack->setPosition(Ling::Edge::Right, 0.f);
	stack->setPosition(Ling::Edge::Top, 0.f);
	stack->setPosition(Ling::Edge::Bottom, 0.f);

	auto addGlyph = [&](const std::wstring& code) {
		auto label = stack->makeChild<Ling::Label>();
		label->setText(code);
		label->setFontFamily(Icon::Family);
		label->setFontSize(Icon::Size);
		// 铺满叠层 + 居中：两个字形的中心必然重合
		label->setPositionType(Ling::Position::Absolute);
		label->setPosition(Ling::Edge::Left, 0.f);
		label->setPosition(Ling::Edge::Right, 0.f);
		label->setPosition(Ling::Edge::Top, 0.f);
		label->setPosition(Ling::Edge::Bottom, 0.f);
		label->setJustifyContent(Ling::Justify::Center);
		label->setAlignItems(Ling::Align::Center);
		return label;
	};

	overlayBtn = btn;
	overlayBaseLabel = addGlyph(baseGlyph);
	overlayTopLabel = addGlyph(overlayGlyph);
	overlayAlpha = std::clamp(alpha, 0.f, 1.f);
	overlayFlag = flag;
	applyOverlayToggleStyle(*flag);
	tip->bind(btn, Lang::get(tipKey));
	btn->onClick.add([this](Ling::Button*) {
		if (!overlayFlag) return;
		*overlayFlag = !*overlayFlag;
		applyOverlayToggleStyle(*overlayFlag);
		win->onToolStyleChanged();
	});
	return btn;
}

void ToolSub::applyOverlayToggleStyle(bool selected)
{
	const UINT32 base = selected ? Icon::ColorActive : Icon::ColorNormal;
	if (overlayBaseLabel) overlayBaseLabel->setColor(base);
	if (overlayTopLabel) {
		// 颜色按 RRGGBBAA 打包：叠字形只改 alpha
		const UINT32 a = (UINT32)(overlayAlpha * 255.f + 0.5f);
		overlayTopLabel->setColor((base & 0xFFFFFF00u) | (a & 0xFFu));
	}
}

void ToolSub::makeNumberStyleBtns()
{
	numberStyleBtns.clear();
	emojiTrigger = nullptr;
	// 默认就是数字序号，属性栏只放字母 / Emoji；再点同一项回到数字
	struct Item { const wchar_t* icon; int style; const wchar_t* tip; };
	const Item items[] = {
		{ Icon::NumberLetter, NumberLetter, L"tool.numberLetter" },
		{ Icon::NumberEmoji, NumberEmoji, L"tool.numberEmoji" },
	};
	for (auto& it : items) {
		auto btn = contentNode->makeChild<Ling::Button>();
		btn->setText(it.icon);
		styleToolbarBtn(btn);
		btn->setFontFamily(Icon::Family);
		btn->setFontSize(Icon::Size);
		tip->bind(btn, Lang::get(it.tip));
		btn->onClick.add([this, style = it.style](Ling::Button*) {
			numberStyle = (numberStyle == style) ? NumberDigit : style;
			showNumberTools(); // 重建：Emoji 模式多出表情入口 / 滑条范围
			win->onToolStyleChanged();
		});
		numberStyleBtns.push_back(btn);
	}
	if (numberStyle == NumberEmoji) {
		emojiTrigger = contentNode->makeChild<Ling::Button>();
		styleToolbarBtn(emojiTrigger);
		emojiTrigger->setFontFamily(L"Segoe UI Emoji");
		emojiTrigger->setFontSize(22.f);
		emojiTrigger->setColor(0x000000FFu);
		emojiTrigger->setHoverColor(0x000000FFu);
		updateEmojiTriggerGlyph();
		auto tipText = Lang::get(L"tool.numberEmojiPick");
		if (tipText.empty() || tipText == L"tool.numberEmojiPick") tipText = L"选择表情";
		tip->bind(emojiTrigger, tipText);
		emojiTrigger->onClick.add([this](Ling::Button*) {
			closeColorPanel();
			nestedPanel->openEmoji(workAreaRect(), emojiTrigger);
		});
	}
	refreshNumberStyleBtns();
}

void ToolSub::updateEmojiTriggerGlyph()
{
	if (!emojiTrigger) return;
	emojiTrigger->setText(serialEmoji.empty() ? SerialEmoji::Default : serialEmoji);
}

void ToolSub::refreshNumberStyleBtns()
{
	const int styles[] = { NumberLetter, NumberEmoji };
	for (size_t i = 0; i < numberStyleBtns.size() && i < 2; i++) {
		applyToggleStyle(numberStyleBtns[i], numberStyle == styles[i]);
	}
	if (emojiTrigger) {
		const bool on = nestedPanel && nestedPanel->isOpen()
			&& nestedPanel->kind() == ToolNestedPanel::Kind::Emoji;
		emojiTrigger->setBg(on ? ToolbarTheme::hoverBg : 0);
	}
}

void ToolSub::makeRectStyleBtns()
{
	fillBtn = contentNode->makeChild<Ling::Button>();
	fillBtn->setText(Icon::Fill);
	styleToolbarBtn(fillBtn);
	fillBtn->setFontFamily(Icon::Family);
	fillBtn->setFontSize(Icon::Size);
	applyToggleStyle(fillBtn, isRectFill);
	tip->bind(fillBtn, Lang::get(L"tool.rectFill"));
	fillBtn->onClick.add([this](Ling::Button*) {
		isRectFill = !isRectFill;
		if (isRectFill) {
			isRectDash = false;
			if (dashBtn) applyToggleStyle(dashBtn, false);
		}
		applyToggleStyle(fillBtn, isRectFill);
		win->onToolStyleChanged();
	});

	dashBtn = contentNode->makeChild<Ling::Button>();
	dashBtn->setText(Icon::DashRect);
	styleToolbarBtn(dashBtn);
	dashBtn->setFontFamily(Icon::Family);
	dashBtn->setFontSize(Icon::Size);
	applyToggleStyle(dashBtn, isRectDash);
	tip->bind(dashBtn, Lang::get(L"tool.dash"));
	dashBtn->onClick.add([this](Ling::Button*) {
		isRectDash = !isRectDash;
		if (isRectDash) {
			isRectFill = false;
			if (fillBtn) applyToggleStyle(fillBtn, false);
		}
		applyToggleStyle(dashBtn, isRectDash);
		win->onToolStyleChanged();
	});

	roundBtn = contentNode->makeChild<Ling::Button>();
	roundBtn->setText(Icon::RoundCorner);
	styleToolbarBtn(roundBtn);
	roundBtn->setFontFamily(Icon::Family);
	roundBtn->setFontSize(Icon::Size);
	applyToggleStyle(roundBtn, cornerRadius > 0.f && !isEllipse);
	tip->bind(roundBtn, Lang::get(isEllipse ? L"tool.roundCornerEllipse" : L"tool.roundCorner"));
	roundBtn->onClick.add([this](Ling::Button*) {
		if (isEllipse) return;
		closeColorPanel();
		// 二级已开再点 → 关圆角；否则保证 >0 并弹出滑条
		if (nestedPanel && nestedPanel->isOpen()
			&& nestedPanel->kind() == ToolNestedPanel::Kind::Corner) {
			closeNestedPanel();
			cornerRadius = 0.f;
			refreshNestedTrigger();
			win->onToolStyleChanged();
			return;
		}
		if (cornerRadius <= 0.f)
			cornerRadius = lastCornerRadius > 0.f ? lastCornerRadius : 10.f;
		refreshNestedTrigger();
		nestedPanel->openCorner(workAreaRect(), roundBtn);
		win->onToolStyleChanged();
	});
}

void ToolSub::makeArrowHeadBtn(const wchar_t* icon, int style, const wchar_t* tipKey)
{
	auto btn = contentNode->makeChild<Ling::Button>();
	btn->setText(icon);
	styleToolbarBtn(btn);
	btn->setFontFamily(Icon::Family);
	btn->setFontSize(Icon::Size);
	tip->bind(btn, Lang::get(tipKey));
	btn->onClick.add([this, style](Ling::Button*) {
		arrowHead = (arrowHead == style) ? ArrowDefault : style;
		if (arrowHead != ArrowDefault && isLine) {
			isLine = false;
			if (lineBtn) applyToggleStyle(lineBtn, false);
		}
		refreshArrowHeadBtns();
		win->onToolStyleChanged();
	});
	arrowHeadBtns.push_back(btn);
}

void ToolSub::makeArrowHeadBtns()
{
	arrowHeadBtns.clear();
	makeArrowHeadBtn(Icon::ArrowBig, ArrowBig, L"tool.arrowBig");
	makeArrowHeadBtn(Icon::ArrowBoth, ArrowBoth, L"tool.arrowBoth");
	makeArrowHeadBtn(Icon::AnnotBoth, ArrowAnnot, L"tool.arrowAnnot");
	refreshArrowHeadBtns();
}

void ToolSub::refreshArrowHeadBtns()
{
	const int styles[] = { ArrowBig, ArrowBoth, ArrowAnnot };
	for (size_t i = 0; i < arrowHeadBtns.size() && i < 3; i++) {
		applyToggleStyle(arrowHeadBtns[i], arrowHead == styles[i]);
	}
}

void ToolSub::loadStrokeSlider(const std::wstring& toolId)
{
	auto cfg = findSliderCfg(toolId);
	if (!cfg) return;
	curSliderKey = cfg->key;
	sliderMin = cfg->min;
	sliderMax = cfg->max;
	sliderVal = rememberedSlider(toolId, strokeDefaultWidth(toolId, cfg->def), cfg->min, cfg->max);
}

void ToolSub::initSlider()
{
	// 与二级属性栏共用 PropSlider（尺寸 / 间距同一套）
	auto pair = PropSlider::mount(contentNode, sliderMin, sliderMax, sliderVal,
		[this](float val) {
			sliderVal = val;
			// 贴图「透明度」：直接改画面不透明度（pin 级属性），不走形状样式管线
			if (curToolId == L"opacity") {
				if (!suppressSliderStyleNotify) win->setPinOpacityPercent((int)std::lround(val));
				return;
			}
			const auto key = (curToolId == L"number" && numberStyle == NumberEmoji)
				? L"numberEmoji" : curToolId;
			rememberSlider(key, val);
			if (suppressSliderStyleNotify) return; // 程序化同步（如拖文字角点）不重刷样式
			win->onToolStyleChanged();
		});
	slider = pair.slider;
	sliderValue = pair.value;
}

bool ToolSub::adjustPrimarySize(int dir)
{
	if (curToolId.empty() || dir == 0) return false;
	auto cfg = findSliderCfg(curToolId);
	float mn = sliderMin, mx = sliderMax;
	if (curToolId == L"number" && numberStyle == NumberEmoji) {
		mn = 20.f; mx = 240.f;
	}
	else if (cfg) {
		mn = cfg->min; mx = cfg->max;
	}
	else return false;
	const float step = std::max(1.f, (mx - mn) / 40.f);
	const float next = std::clamp(sliderVal + dir * step, mn, mx);
	if (next == sliderVal) return true;
	sliderVal = next;
	const auto key = (curToolId == L"number" && numberStyle == NumberEmoji)
		? L"numberEmoji" : curToolId;
	rememberSlider(key, sliderVal);
	updateSliderValueLabel();
	if (slider) {
		suppressSliderStyleNotify = true;
		slider->setValue(sliderVal);
		suppressSliderStyleNotify = false;
	}
	win->onToolStyleChanged();
	win->pulseSizePreview();
	return true;
}

void ToolSub::updateSliderValueLabel()
{
	if (!sliderValue) return;
	sliderValue->setText(std::format(L"{}", static_cast<int>(std::round(sliderVal))));
}

std::wstring ToolSub::getSliderTipKey() const
{
	if (curToolId == L"arrow") return L"tool.arrowWidth";
	if (curToolId == L"pen") {
		// 渐隐画笔就是画笔：粗细提示沿用「画笔粗细」，不再另起一个键
		if (isPenFade) return L"tool.penWidth";
		if (isPenSoft) return L"tool.softPenWidth";
		return L"tool.penWidth";
	}
	if (curToolId == L"text") return L"tool.fontSizeTip";
	if (curToolId == L"number") {
		return numberStyle == NumberEmoji ? L"tool.emojiSize" : L"tool.numberSize";
	}
	if (curToolId == L"mosaic") return isMosaicBlur ? L"tool.blurSize" : L"tool.mosaicSize";
	if (curToolId == L"patina") return L"tool.patinaSize";
	if (curToolId == L"watermark") return L"tool.wmSize";
	if (curToolId == L"highlight") return L"tool.strokeWidth";
	if (curToolId == L"opacity") return L"pin.opacityTool";
	return L"tool.strokeWidth";
}

float ToolSub::toPx(float logical) const
{
	return std::floor(logical * dpi);
}

float ToolSub::getDesiredHeight()
{
	return toPx(btnSize) + toPx(marginTop) + toPx(ToolbarTheme::shadowPad);
}

void ToolSub::initSize(int btnCount, bool withColors, bool centerOnBtn, float extraLogicW)
{
	hasTools = true;
	this->centerOnBtn = centerOnBtn;
	sizeBtnCount = btnCount;
	sizeWithColors = withColors;
	sizeExtraW = extraLogicW;
	const int nIcons = btnCount + (withColors ? 1 : 0);
	const float iconSlot = Icon::Size + ToolbarTheme::propGap;
	auto pxW = toPx(ToolbarTheme::propPad) * 2.f
		+ toPx(iconSlot) * nIcons
		+ toPx(sizeExtraW)
		+ toPx(PropSlider::rowLogicW())
		+ toPx(ToolbarTheme::shadowPad) * 2.f;
	setSize(pxW / dpi, getDesiredHeight() / dpi);
}

void ToolSub::refreshSize()
{
	if (!hasTools) return;
	initSize(sizeBtnCount, sizeWithColors, centerOnBtn, sizeExtraW);
}

D2D1_COLOR_F ToolSub::getSelectedColor() const
{
	return Ling::Color(colors[selectColorIndex]).getD2DColor();
}

UINT32 ToolSub::getSelectedColorValue() const
{
	return colors[selectColorIndex];
}

float ToolSub::getSliderVal() const
{
	return sliderVal * dpi;
}

bool ToolSub::hasContent()
{
	return hasTools;
}

void ToolSub::hideTools()
{
	hasTools = false;
	numberStyleBtns.clear();
	arrowHeadBtns.clear();
	fillBtn = nullptr;
	dashBtn = nullptr;
	roundBtn = nullptr;
	lineBtn = nullptr;
	if (sliderHover) {
		sliderHover = false;
		win->setSizePreviewHold(false);
	}
	closeColorPanel();
	closeNestedPanel();
	colorTrigger = nullptr;
	colorTriggerRing = nullptr;
	colorTriggerSwatch = nullptr;
	wmTextBox = nullptr;
	angleBtn = nullptr;
	patinaWmBtn = nullptr;
	emojiTrigger = nullptr;
	overlayBtn = nullptr;
	overlayBaseLabel = nullptr;
	overlayTopLabel = nullptr;
	overlayFlag = nullptr;
	boardCustomCanvas = nullptr;
	boardOpacityBtn = nullptr;
	recPlayBtn = nullptr;
	recAudioBtn = nullptr;
	recMicBtn = nullptr;
	recGifBtn = nullptr;
	recSaveBtn = nullptr;
	recCancelBtn = nullptr;
	recDoneBtn = nullptr;
	recTimerLabel = nullptr;
	tip->hide();
	if (!isVisible) return;
	hide();
	isVisible = false;
}

void ToolSub::suspendChrome()
{
	hideHoverTip();
	if (colorPanel && colorPanel->isOpen()) colorPanel->hide();
	if (nestedPanel && nestedPanel->isOpen()) nestedPanel->hide();
	if (!isVisible) return;
	hide();
	isVisible = false;
}

void ToolSub::resumeChrome(const RECT& workArea)
{
	if (!hasTools) return;
	// layoutTools 可能已摆过主属性栏；此处补 show，并兜底尚未显示的情况
	if (!isVisible) updatePosition(workArea);
	if (colorPanel && colorPanel->isOpen()) colorPanel->show();
	if (nestedPanel && nestedPanel->isOpen()) nestedPanel->show();
}

void ToolSub::bindOwner(HWND ownerHwnd)
{
	if (!hwnd || !ownerHwnd) return;
	SetWindowLongPtr(hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(ownerHwnd));
}

LRESULT ToolSub::onHitTest(const POINT screenPos)
{
	// 属性栏若盖住选区边/角，穿透给 WinCap 以保持 ↔ 光标与拖边
	auto* cap = dynamic_cast<WinCap*>(win);
	if (cap && cap->hwnd && cap->cutMask && cap->cutMask->hasRect()) {
		POINT client = screenPos;
		ScreenToClient(cap->hwnd, &client);
		const auto hit = cap->cutMask->hitTest(client);
		if (hit != MaskHit::None && hit != MaskHit::Inside)
			return HTTRANSPARENT;
	}
	return HTCLIENT;
}

void ToolSub::hideHoverTip()
{
	if (tip) tip->hide();
	if (colorPanel) colorPanel->hideHoverTip();
	if (nestedPanel) nestedPanel->hideHoverTip();
}

void ToolSub::updatePosition(const RECT& workArea)
{
	if (!hasTools) return;
	AnnotHost::MainBarAnchor bar{};
	if (!win->queryMainBarAnchor(bar)) return;
	// 工作区按「主工具栏所在显示器」取：多屏时调用方传进来的往往是截图窗口 /
	// 贴图窗口的矩形，主栏被拖到副屏后就属于另一块屏，照那个工作区夹紧 x
	// 会把属性栏甩到主屏上（用户看到的就是"属性栏跑到另一个屏幕"）。
	RECT barRect{ static_cast<LONG>(bar.x), static_cast<LONG>(bar.y),
		static_cast<LONG>(bar.x + bar.w), static_cast<LONG>(bar.y + bar.h) };
	const RECT wa = (bar.w > 0.f && bar.h > 0.f)
		? ToolbarChrome::workAreaNear(barRect) : workArea;
	auto btnCenterX = bar.btnCenterX;
	auto mainX = bar.x;
	const float mainW = bar.w;
	float px;
	if (w < mainW) {
		// 短于主栏：尽量以选中工具居中，但不允许左右超出主栏
		px = mainX + btnCenterX - w / 2.f;
		if (px < mainX) px = mainX;
		if (px + w > mainX + mainW) px = mainX + mainW - w;
	}
	else {
		px = mainX;
	}
	const float mainPad = ToolbarTheme::shadowPad * bar.dpi;
	const float refTop = bar.y + mainPad;
	const float refBottom = bar.y + bar.h - mainPad;
	const float arrowAnchor = mainX + btnCenterX;
	auto place = ToolbarChrome::placeBubble(
		wa, refTop, refBottom, w, h, mainGap,
		// 属性栏优先在主栏下方；下方不够再上弹
		px, arrowAnchor, true, tipDown_, arrowX, x, y);

	// 属性栏不能压在框选上：主栏在选区上方时，属性栏默认落在主栏下方正好会盖住选区。
	// 这里再算一次"翻到主栏上方"的落点，两个落点里挑与选区重叠面积更小的那个。
	RECT avoid{};
	bool hasAvoid = false;
	if (auto* cap = dynamic_cast<WinCap*>(win)) {
		if (cap->hwnd && cap->cutMask && cap->cutMask->hasRect()) {
			POINT tl{ (LONG)cap->cutMask->maskRect.left, (LONG)cap->cutMask->maskRect.top };
			POINT br{ (LONG)cap->cutMask->maskRect.right, (LONG)cap->cutMask->maskRect.bottom };
			ClientToScreen(cap->hwnd, &tl);
			ClientToScreen(cap->hwnd, &br);
			avoid = RECT{ (LONG)std::min(tl.x, br.x), (LONG)std::min(tl.y, br.y),
				(LONG)std::max(tl.x, br.x), (LONG)std::max(tl.y, br.y) };
			hasAvoid = avoid.right > avoid.left && avoid.bottom > avoid.top;
		}
	}
	if (hasAvoid) {
		auto overlapArea = [&](const ToolbarChrome::BubblePlacement& p) -> LONGLONG {
			const LONG l = (std::max)((LONG)p.x, avoid.left);
			const LONG t = (std::max)((LONG)p.y, avoid.top);
			const LONG r = (std::min)((LONG)p.x + (LONG)w, avoid.right);
			const LONG b = (std::min)((LONG)p.y + (LONG)h, avoid.bottom);
			return (r > l && b > t) ? (LONGLONG)(r - l) * (LONGLONG)(b - t) : 0;
		};
		if (overlapArea(place) > 0) {
			const auto flipped = ToolbarChrome::placeBubble(
				wa, refTop, refBottom, w, h, mainGap, px, arrowAnchor,
				false, tipDown_, arrowX, x, y);
			if (overlapArea(flipped) < overlapArea(place)) place = flipped;
		}
	}
	if (place.tipFlipped) applyTipDirection(place.tipDown);
	arrowX = place.arrowX;
	if (place.moved) setPosition(place.x, place.y);
	if (!isVisible) {
		show();
		isVisible = true;
	}
	else if (place.needPaint) {
		refresh();
	}
	if (colorPanel && colorPanel->isOpen()) {
		colorPanel->updatePosition(wa);
	}
	if (nestedPanel && nestedPanel->isOpen()) {
		nestedPanel->updatePosition(wa);
	}
}
