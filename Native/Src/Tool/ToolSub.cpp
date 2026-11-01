#include "pch.h"
#include "DesignKeys.h"
#include <commdlg.h>
#include <cstdlib>
#include <cwctype>
#include "../App.h"
#include "../Win/AnnotHost.h"
#include "../Win/WinCap.h"
#include "../Win/CutMask.h"
#include "../Lang.h"
#include "../Setting.h"
#include "../Tip.h"
#include "ToolSub.h"
#include "ToolCap.h"
#include "ToolInitialDefs.h"
#include "ToolColorPanel.h"
#include "ToolNestedPanel.h"
#include "IconCodes.h"
#include "ToolbarChrome.h"
#include "ToolbarTheme.h"
#include "PropSlider.h"
#include "SerialEmoji.h"
#include "../Shape/ShapeNumber.h"
#include "../Shape/ShapeRect.h"
#include "../History.h"

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
		{ L"text",      { L"fontSize", 8.f, 333.f, 24.f } },
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

	// 初始属性（外观 > 工具栏外观 > 初始属性）里存过的整型项；没存过返回 def
	int designInt(const wchar_t* tool, const wchar_t* key, int def)
	{
		auto* s = Setting::get();
		return s ? s->getToolDesignInt(tool, key, def) : def;
	}

	// 属性栏要显示的形态：按「初始属性」里配置的顺序排，并去掉被设为工具栏默认的那一款
	// （默认形态已经在工具栏按钮上，属性栏里不再重复）
	std::vector<ToolInitial::InitialChoice> orderedFormsFor(const wchar_t* defId, int defaultForm)
	{
		std::vector<ToolInitial::InitialChoice> out;
		const auto* def = ToolInitial::find(defId);
		if (!def || !def->forms) return out;
		out = *def->forms;
		auto* s = Setting::get();
		const std::wstring order = s ? s->getToolDesignStr(def->designId, L"order", L"") : L"";
		if (!order.empty()) {
			std::vector<ToolInitial::InitialChoice> sorted;
			std::vector<bool> used(out.size(), false);
			size_t pos = 0;
			while (pos < order.size()) {
				const size_t comma = order.find(L',', pos);
				const std::wstring tok = order.substr(pos, comma == std::wstring::npos ? std::wstring::npos : comma - pos);
				const int v = tok.empty() ? -1 : _wtoi(tok.c_str());
				for (size_t i = 0; i < out.size(); ++i) {
					if (!used[i] && out[i].value == v) { used[i] = true; sorted.push_back(out[i]); break; }
				}
				if (comma == std::wstring::npos) break;
				pos = comma + 1;
			}
			for (size_t i = 0; i < out.size(); ++i) if (!used[i]) sorted.push_back(out[i]);
			out.swap(sorted);
		}
		std::vector<ToolInitial::InitialChoice> shown;
		shown.reserve(out.size());
		for (const auto& c : out) if (c.value != defaultForm) shown.push_back(c);
		return shown;
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

ToolSub::ToolSub(AnnotHost* win) : ToolbarWindow(), win(win)
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
		// 属性栏输入框编辑中：键盘只给输入框，不能再转发成删除标注/全局快捷键。
		const bool wmEditing = wmTextBox && wmTextBox->isFocused();
		const bool numberEditing = numberInput && numberInput->isFocused();
		if (wmEditing || numberEditing) {
			if (key == VK_ESCAPE && wmEditing) this->win->forwardKey(key);
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
	auto pad = ToolbarChrome::propBubbleContentPad(false);
	initChrome(ToolbarTheme::propPad, ToolbarTheme::propPad, pad.top, pad.bottom,
		Ling::FlexDirection::Row);
	onMouseMove.add([this](POINT pos) {
		if (!slider) return;
		const bool onSlider = slider->isPosIn(pos)
			|| (sliderValue && sliderValue->isPosIn(pos));
		if (onSlider != sliderHover) {
			sliderHover = onSlider;
			if (onSlider) {
				tip->showAbove(slider, Lang::get(getSliderTipKey()));
				// 二次选中本工具的标注时，改的就是它本身，不再画独立的画布预览
				if (!win->sizePreviewSuppressed()) win->setSizePreviewHold(true, false);
			}
			else {
				tip->hide();
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
	strokeBtn = nullptr;
	slider = nullptr;
	sliderValue = nullptr;
	wmTextBox = nullptr;
	numberInput = nullptr;
	numberShowsSelected_ = false;
	selectedNumberLabel_.clear();
	fillBtn = nullptr;
	dashBtn = nullptr;
	softBtn = nullptr;
	boldBtn = nullptr;
	italicBtn = nullptr;
	hiEllipseBtn = nullptr;
	hiRectBtn = nullptr;
	hiBorderBtn = nullptr;
	arrowDashBtn = nullptr;
	arrowRoundBtn = nullptr;
	lineBtn = nullptr;
	markerBtn = nullptr;
	roundBtn = nullptr;
	ellipseBtn = nullptr;
	polyBtn = nullptr;
	starBtn = nullptr;
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
	styleToggles.clear();
	sizeExtraW = 0.f;
	numberStyleBtns.clear();
	arrowHeadBtns.clear();
	arrowHeadStyles.clear();
	lineKindBtns.clear();
	// 形态按钮每栏重建：先清空旧指针，避免某形态被设为默认后不再创建却留下悬空引用
	dashBtn = nullptr;
	softBtn = nullptr;
	markerBtn = nullptr;
	penBaseBtn = nullptr;
	ellipseBtn = nullptr;
	rectBtn = nullptr;
	polyBtn = nullptr;
	starBtn = nullptr;
	bubbleKindBtn = nullptr;
	angleKindBtn = nullptr;
	waveKindBtn = nullptr;
	dimKindBtn = nullptr;
	curToolId = id;
	auto cfg = findSliderCfg(id);
	if (!cfg) return;
	curSliderKey = cfg->key;
	sliderMin = cfg->min;
	sliderMax = cfg->max;
	// 会话内记忆；新截图会话 ToolSub 重建后回到默认
	sliderVal = rememberedSlider(id, strokeDefaultWidth(id, cfg->def), cfg->min, cfg->max);
	// 初始属性里的默认数值（粗细 / 字号 / 大小）优先于会话记忆
	{
		const int dw = designInt(id.c_str(), L"width", -1);
		if (dw >= 0) sliderVal = std::clamp((float)dw, sliderMin, sliderMax);
	}
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
	// 初始属性：默认形态（0 矩形 / 1 椭圆 / 2 多边形 / 3 星形）+ 边数 / 角数
	int defaultForm = 0;
	{
		const int form = designInt(ToolKey::Rect, DesignKey::Form, -1);
		if (form >= 0) {
			defaultForm = std::clamp(form, 0, 3);
			isEllipse = (form == 1);
			rectKind = (form == 2) ? PolygonShape : (form == 3) ? StarShape : RectShape;
		}
		const int sides = designInt(ToolKey::Rect, DesignKey::Sides, -1);
		if (sides >= 0) polygonSides = std::clamp(sides, 3, 20);
		const int pts = designInt(ToolKey::Rect, DesignKey::Points, -1);
		if (pts >= 0) starPoints = std::clamp(pts, 3, 20);
	}
	if (isRectFill && isRectDash) isRectDash = false;
	initSize(6, true);   // 三个形态 + 填充 | 虚线 | 圆角
	// 多边形 / 星形：点开 = 选中并弹出"边数/角数"滑条；再点同一个 = 回矩形
	auto mkShapeKindBtn = [this](const wchar_t* icon, const wchar_t* tipKey, int kind) {
		auto b = contentNode->makeChild<Ling::Button>();
		b->setText(icon);
		styleToolbarBtn(b);
		b->setFontFamily(Icon::Family);
		b->setFontSize(Icon::Size);
		applyToggleStyle(b, rectKind == kind && !isEllipse);
		tip->bind(b, Lang::get(tipKey));
		b->onClick.add([this, kind](Ling::Button* btn) {
			closeColorPanel();
			const bool same = (rectKind == kind && !isEllipse);
			isEllipse = false;
			if (ellipseBtn) applyToggleStyle(ellipseBtn, false);
			rectKind = same ? RectShape : kind;
			refreshRectKindBtns();
			if (rectKind == kind) {
				nestedPanel->openShapeSides(workAreaRect(), btn, kind == StarShape);
			}
			else if (nestedPanel && nestedPanel->isOpen()
				&& nestedPanel->kind() == ToolNestedPanel::Kind::ShapeSides) {
				closeNestedPanel();
			}
			win->onToolStyleChanged();
		});
		registerStyleToggle([this, kind] { return !isEllipse && rectKind == kind; }, [this] {
			const int f = designInt(ToolKey::Rect, DesignKey::Form, 0);
			isEllipse = (f == 1);
			rectKind = (f == 2) ? PolygonShape : (f == 3 ? StarShape : RectShape);
		});
		return b;
	};
	// 形态顺序跟随「初始属性」；默认形态已在工具栏，属性栏里不再重复
	for (const auto& c : orderedFormsFor(L"rect", defaultForm)) {
		if (c.value == 1) {
			ellipseBtn = makeToggleBtn(c.glyph ? c.glyph : Icon::Ellipse,
				&isEllipse, c.tipKey ? c.tipKey : L"tool.ellipse", L"ellipse");
			ellipseBtn->onClick.add([this](Ling::Button*) {
				if (isEllipse) {
					// 椭圆与多边形/星形互斥
					rectKind = RectShape;
					refreshRectKindBtns();
					if (nestedPanel && nestedPanel->isOpen()
						&& nestedPanel->kind() == ToolNestedPanel::Kind::ShapeSides)
						closeNestedPanel();
				}
				refreshNestedTrigger();
			});
			registerStyleToggle([this] { return isEllipse; }, [this] {
				const int f = designInt(ToolKey::Rect, DesignKey::Form, 0);
				isEllipse = (f == 1);
				rectKind = (f == 2) ? PolygonShape : (f == 3 ? StarShape : RectShape);
			});
		}
		else if (c.value == 2 || c.value == 3) {
			auto* b = mkShapeKindBtn(c.glyph ? c.glyph : Icon::Polygon,
				c.tipKey ? c.tipKey : L"tool.polygon", c.value == 2 ? PolygonShape : StarShape);
			if (c.value == 2) polyBtn = b; else starBtn = b;
		}
		else {
			// 形态默认不是矩形时，栏里保留「矩形」入口，点一下回到基础形态
			rectBtn = contentNode->makeChild<Ling::Button>();
			rectBtn->setText(c.glyph ? c.glyph : Icon::Rect);
			styleToolbarBtn(rectBtn);
			rectBtn->setFontFamily(Icon::Family);
			rectBtn->setFontSize(Icon::Size);
			applyToggleStyle(rectBtn, !isEllipse && rectKind == RectShape);
			tip->bind(rectBtn, Lang::get(c.tipKey ? c.tipKey : L"tool.rectDefault"));
			rectBtn->onClick.add([this](Ling::Button*) {
				closeColorPanel();
				isEllipse = false;
				rectKind = RectShape;
				refreshRectKindBtns();
				refreshNestedTrigger();
				if (nestedPanel && nestedPanel->isOpen()
					&& nestedPanel->kind() == ToolNestedPanel::Kind::ShapeSides)
					closeNestedPanel();
				win->onToolStyleChanged();
			});
			registerStyleToggle([this] { return !isEllipse && rectKind == RectShape; }, [this] {
				const int f = designInt(ToolKey::Rect, DesignKey::Form, 0);
				isEllipse = (f == 1);
				rectKind = (f == 2) ? PolygonShape : (f == 3 ? StarShape : RectShape);
			});
		}
	}
	makeRectStyleBtns();
	initColorTrigger();
	initSlider();
}

void ToolSub::refreshRectKindBtns()
{
	if (rectBtn) applyToggleStyle(rectBtn, !isEllipse && rectKind == RectShape);
	if (polyBtn) applyToggleStyle(polyBtn, rectKind == PolygonShape && !isEllipse);
	if (starBtn) applyToggleStyle(starBtn, rectKind == StarShape && !isEllipse);
	refreshRoundBtnStyle();
}

// 圆角按钮：椭圆没有圆角 → 置灰（点了也不响应）；矩形/多边形/星形按是否开着圆角高亮
void ToolSub::refreshRoundBtnStyle()
{
	if (!roundBtn) return;
	if (isEllipse) {
		roundBtn->setBg(0);
		roundBtn->setHoverBg(0);
		roundBtn->setColor(Icon::ColorDisabled);
		roundBtn->setHoverColor(Icon::ColorDisabled);
		return;
	}
	applyToggleStyle(roundBtn, cornerRadius > 0.f);
}

// 滑条实时作用于：优先当前选中的同形态标注，否则作用于刚画完的标注
void ToolSub::applyShapeSidesLive(bool star, int sides)
{
	auto* r = dynamic_cast<ShapeRect*>(win->liveStyleShape());
	if (!r) return;
	if (r->kind() != (star ? ShapeRect::Star : ShapeRect::Polygon)) return;
	r->setSides(sides);
}

// 圆角滑条：实时作用于选中的或刚画完的矩形/多边形/星形
void ToolSub::applyCornerRadiusLive()
{
	auto* r = dynamic_cast<ShapeRect*>(win->liveStyleShape());
	if (!r) return;
	r->setCornerRadius(cornerRadius);   // 0–100
}

void ToolSub::setShapePreviewHold(bool hold)
{
	// 边数/角数/圆角：预览形状本身；二次选中同工具标注时由 AnnotHost 统一抑制
	if (win) win->setSizePreviewHold(hold && !win->sizePreviewSuppressed(), true);
}

void ToolSub::pulseShapePreview()
{
	if (win) win->pulseSizePreview(true);
}

void ToolSub::showArrowTools()
{
	beginTool(L"arrow");
	isArrowFill = true;
	isLine = false;   // 默认画箭头；需要线段时点属性栏的「线段」临时开关
	if (arrowHead < ArrowDefault || arrowHead > ArrowFun) arrowHead = ArrowDefault;
	// 初始属性（外观 > 工具栏外观）：存过默认形态就优先用它
	auto* designSetting = Setting::get();
	const bool arrowDesign = designSetting && designSetting->hasToolDesign(L"arrow");
	if (arrowDesign)
		arrowHead = std::clamp(designSetting->getToolDesignInt(ToolKey::Arrow, DesignKey::Head, arrowHead),
			(int)ArrowDefault, (int)ArrowFun);
	// 各种箭头共用 arrow 滑条，不因切子样式改量程
	loadStrokeSlider(L"arrow");
	// 初始属性里的默认粗细（没存过就沿用滑条记忆 / 全局默认）
	if (arrowDesign) {
		const int dw = designSetting->getToolDesignInt(ToolKey::Arrow, DesignKey::Width, -1);
		if (dw > 0) sliderVal = std::clamp((float)dw, sliderMin, sliderMax);
	}
	// 被提为工具栏默认形态的那一款已从属性栏移走（工具栏按钮就是它），其余形态按
	// 「初始属性」里配置的顺序排列；基础箭头不在默认位时自然留在属性栏里
	const int defaultHead = arrowDesign ? arrowHead : (int)ArrowDefault;
	initSize(7, true); // 4 个箭头样式 + 线段 + 虚线 + 圆头，再加色块与滑条
	// 线段：临时开关（与箭头样式互斥；切走工具后回到箭头）
	lineBtn = contentNode->makeChild<Ling::Button>();
	lineBtn->setText(Icon::Line);
	styleToolbarBtn(lineBtn);
	lineBtn->setFontFamily(Icon::Family);
	lineBtn->setFontSize(Icon::Size);
	applyToggleStyle(lineBtn, isLine);
	tip->bind(lineBtn, Lang::get(L"tool.lineDefault"));
	lineBtn->onClick.add([this](Ling::Button*) {
		isLine = !isLine;
		if (isLine) lineKind = LineStraight;
		refreshArrowHeadBtns();
		win->onToolStyleChanged();
	});
	for (const auto& c : orderedFormsFor(L"arrow", defaultHead)) {
		makeArrowHeadBtn(c.glyph ? c.glyph : Icon::Arrow, c.value,
			c.tipKey ? c.tipKey : L"tool.arrowDefault");
	}
	refreshArrowHeadBtns();
	arrowDashBtn = makeToggleBtn(Icon::Dash, &isArrowDash, L"tool.dash", L"dash");
	registerStyleToggle([this] { return isArrowDash; }, [this] { isArrowDash = false; });
	arrowRoundBtn = makeToggleBtn(Icon::RoundCap, &isArrowRound, L"tool.roundCap", L"round");
	registerStyleToggle([this] { return isArrowRound; }, [this] { isArrowRound = false; });
	initColorTrigger();
	initSlider();
}

// 荧光笔 / 软笔不支持虚线：把虚线按钮置灰并强制关掉
void ToolSub::updateDashAvailability()
{
	// 只服务画笔属性栏：其它工具的 dashBtn 是各自独立的按钮，不能被 isPenDash 覆盖
	if (curToolId != L"pen" || !dashBtn) return;
	const bool disabled = isPenMarker || isPenSoft;
	if (disabled) isPenDash = false;
	if (disabled) {
		dashBtn->setBg(0);
		dashBtn->setHoverBg(0);
		dashBtn->setColor(Icon::ColorDisabled);
		dashBtn->setHoverColor(Icon::ColorDisabled);
	}
	else {
		applyToggleStyle(dashBtn, isPenDash);
	}
}

void ToolSub::applyPenMarkerState(bool on)
{
	if (isPenMarker == on) return;
	isPenMarker = on;
	if (markerBtn) applyToggleStyle(markerBtn, on);
	if (on) {
		// 荧光笔与软笔 / 虚线 / 渐隐互斥；粗细沿用画笔滑条（绘制时按倍数加宽）
		isPenFade = false;
		isPenSoft = false;
		isPenDash = false;
		if (softBtn) applyToggleStyle(softBtn, false);
		if (dashBtn) applyToggleStyle(dashBtn, false);
		// 初始属性：荧光笔自己的默认不透明度 / 默认粗细
		const int op = designInt(ToolKey::Marker, DesignKey::Opacity, -1);
		if (op >= 0) penMarkerOpacity = std::clamp(op, 10, 100);
		const int mw = designInt(ToolKey::Marker, DesignKey::Width, -1);
		if (mw >= 0) {
			sliderVal = std::clamp((float)mw, sliderMin, sliderMax);
			updateSliderValueLabel();
			if (slider) {
				suppressSliderStyleNotify = true;
				slider->setValue(sliderVal);
				suppressSliderStyleNotify = false;
			}
		}
	}
	else {
		closeNestedPanel();
	}
	updateDashAvailability();
}

void ToolSub::showLineTools()
{
	// 标注 = 只有轴线：形态（尺寸 / 气泡 / 角度 / 波浪线）+ 虚线 / 圆头 / 颜色 / 粗细。
	// 直线（线段）不在这里：它是箭头属性栏的临时开关（见 showArrowTools）。
	beginTool(L"arrow");
	isArrowFill = true;
	isLine = true;
	loadStrokeSlider(L"arrow");
	// 初始属性：标注默认形态是「尺寸」；旧配置里的 0（直线）按尺寸处理
	int defaultLine = LineDimension;
	lineKind = LineDimension;
	{
		const int form = designInt(ToolKey::Line, DesignKey::Form, -1);
		if (form == LineBubble || form == LineBurst || form == LineAngle
			|| form == LineWave || form == LineDimension) {
			lineKind = form;
			defaultLine = form;
		}
		const int lw = designInt(ToolKey::Line, DesignKey::Width, -1);
		if (lw >= 0) sliderVal = std::clamp((float)lw, sliderMin, sliderMax);
	}
	initSize(6, true);   // 4 个非默认形态 + 虚线 + 圆头，再加色块与滑条
	lineKindBtns.clear();
	bubbleKindBtn = nullptr;
	angleKindBtn = nullptr;
	waveKindBtn = nullptr;
	dimKindBtn = nullptr;
	auto addKindBtn = [this](const wchar_t* icon, int kind, const wchar_t* tipKey) {
		auto b = contentNode->makeChild<Ling::Button>();
		b->setText(icon);
		styleToolbarBtn(b);
		b->setFontFamily(Icon::Family);
		b->setFontSize(Icon::Size);
		applyToggleStyle(b, lineKind == kind);
		tip->bind(b, Lang::get(tipKey));
		b->onClick.add([this, kind](Ling::Button*) {
			lineKind = kind;
			refreshLineKindBtns();
			win->onToolStyleChanged();
		});
		lineKindBtns.push_back({ b, kind });
		registerStyleToggle([this, kind] { return lineKind == kind; }, [this] {
			lineKind = designInt(ToolKey::Line, DesignKey::Form, LineDimension);
		});
		return b;
	};
	// 形态顺序跟随「初始属性」；默认形态已在工具栏，属性栏里不再重复
	for (const auto& c : orderedFormsFor(L"line", defaultLine)) {
		auto* b = addKindBtn(c.glyph ? c.glyph : Icon::Line, c.value,
			c.tipKey ? c.tipKey : L"tool.lineDefault");
		if (c.value == LineBubble) bubbleKindBtn = b;
		else if (c.value == LineAngle) angleKindBtn = b;
		else if (c.value == LineWave) waveKindBtn = b;
		else if (c.value == LineDimension) dimKindBtn = b;
	}
	arrowDashBtn = makeToggleBtn(Icon::Dash, &isArrowDash, L"tool.dash", L"dash");
	registerStyleToggle([this] { return isArrowDash; }, [this] { isArrowDash = false; });
	arrowRoundBtn = makeToggleBtn(Icon::RoundCap, &isArrowRound, L"tool.roundCap", L"round");
	registerStyleToggle([this] { return isArrowRound; }, [this] { isArrowRound = false; });
	refreshLineKindBtns();
	initColorTrigger();
	initSlider();
}

// 标注形态按钮的选中态
void ToolSub::refreshLineKindBtns()
{
	for (auto& [btn, k] : lineKindBtns) {
		applyToggleStyle(btn, lineKind == k);
	}
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
	// 初始属性：0 普通 / 1 软笔 / 2 虚线 / 3 荧光笔（渐隐画笔不参与）
	int defaultPen = 0;
	if (!isPenFade) {
		const int form = designInt(ToolKey::Pen, DesignKey::Form, -1);
		if (form == 0 || form == 1 || form == 3) defaultPen = form;
		if (form == 3) {
			isPenMarker = true;
			isPenSoft = false;
			isPenDash = false;
		}
		else if (!isPenMarker) {
			if (form == 0) { isPenSoft = false; isPenDash = false; }
			else if (form == 1) { isPenSoft = true; isPenDash = false; }
			else if (form == 2) { isPenSoft = false; isPenDash = true; }
		}
	}
	if (isPenSoft && isPenDash) isPenDash = false;
	if (isPenFade && (isPenSoft || isPenDash)) {
		isPenSoft = false;
		isPenDash = false;
	}
	// 渐隐画笔属性栏只有「颜色 + 粗细滑条 + 数值」；渐隐画笔是全屏画布的
	// 专属工具，截图/贴图工具栏不再提供入口，所以画笔属性栏也不放渐隐开关。
	initSize(isPenFade ? 0 : 3, true);

	if (!isPenFade) {
	auto refreshPenForms = [this]() {
		if (penBaseBtn) applyToggleStyle(penBaseBtn, !isPenMarker && !isPenSoft);
		if (markerBtn) applyToggleStyle(markerBtn, isPenMarker);
		if (softBtn) applyToggleStyle(softBtn, isPenSoft);
	};
	// 形态顺序跟随「初始属性」；默认形态已在工具栏，属性栏里不再重复
	for (const auto& c : orderedFormsFor(L"pen", defaultPen)) {
		if (c.value == 3) {
			// 荧光笔：与普通画笔一致，只是更宽、半透明；单击打开「透明度」二级气泡（与水印角度同一套外壳）
			markerBtn = contentNode->makeChild<Ling::Button>();
			markerBtn->setText(c.glyph ? c.glyph : Icon::Marker);
			styleToolbarBtn(markerBtn);
			markerBtn->setFontFamily(Icon::Family);
			markerBtn->setFontSize(Icon::Size);
			applyToggleStyle(markerBtn, isPenMarker);
			tip->bind(markerBtn, Lang::get(c.tipKey ? c.tipKey : L"tool.marker"));
			markerBtn->onClick.add([this, refreshPenForms](Ling::Button* b) {
				const bool next = !isPenMarker;
				applyPenMarkerState(next);
				refreshPenForms();
				win->onToolStyleChanged();
				if (next) nestedPanel->openPenOpacity(workAreaRect(), b);
			});
			registerStyleToggle([this] { return isPenMarker; }, [this] { applyPenMarkerState(false); });
		}
		else if (c.value == 1) {
			softBtn = contentNode->makeChild<Ling::Button>();
			softBtn->setText(c.glyph ? c.glyph : Icon::SoftPen);
			styleToolbarBtn(softBtn);
			softBtn->setFontFamily(Icon::Family);
			softBtn->setFontSize(Icon::Size);
			applyToggleStyle(softBtn, isPenSoft);
			tip->bind(softBtn, Lang::get(c.tipKey ? c.tipKey : L"tool.softPen"));
			softBtn->onClick.add([this, refreshPenForms](Ling::Button*) {
				isPenSoft = !isPenSoft;
				if (isPenSoft) {
					isPenFade = false;
					isPenDash = false;
					if (dashBtn) applyToggleStyle(dashBtn, false);
					applyPenMarkerState(false);
				}
				refreshPenForms();
				updateDashAvailability();
				win->onToolStyleChanged();
			});
			registerStyleToggle([this] { return isPenSoft; }, [this] { isPenSoft = false; });
		}
		else {
			// 形态默认不是常规笔时，栏里保留「常规画笔」入口
			penBaseBtn = contentNode->makeChild<Ling::Button>();
			penBaseBtn->setText(c.glyph ? c.glyph : Icon::Pen);
			styleToolbarBtn(penBaseBtn);
			penBaseBtn->setFontFamily(Icon::Family);
			penBaseBtn->setFontSize(Icon::Size);
			applyToggleStyle(penBaseBtn, !isPenMarker && !isPenSoft);
			tip->bind(penBaseBtn, Lang::get(c.tipKey ? c.tipKey : L"tool.penDefault"));
			penBaseBtn->onClick.add([this, refreshPenForms](Ling::Button*) {
				isPenFade = false;
				isPenMarker = false;
				isPenSoft = false;
				isPenDash = false;
				if (dashBtn) applyToggleStyle(dashBtn, false);
				refreshPenForms();
				updateDashAvailability();
				if (nestedPanel && nestedPanel->isOpen()
					&& nestedPanel->kind() == ToolNestedPanel::Kind::PenOpacity)
					closeNestedPanel();
				win->onToolStyleChanged();
			});
		}
	}
	refreshPenForms();

	dashBtn = contentNode->makeChild<Ling::Button>();
	dashBtn->setText(Icon::Dash);
	styleToolbarBtn(dashBtn);
	dashBtn->setFontFamily(Icon::Family);
	dashBtn->setFontSize(Icon::Size);
	applyToggleStyle(dashBtn, isPenDash);
	tip->bind(dashBtn, Lang::get(L"tool.dash"));
	dashBtn->onClick.add([this](Ling::Button*) {
		if (isPenMarker || isPenSoft) return;   // 荧光笔/软笔不支持虚线
		isPenDash = !isPenDash;
		if (isPenDash) {
			isPenFade = false;
			isPenSoft = false;
			if (softBtn) applyToggleStyle(softBtn, false);
			applyPenMarkerState(false);
		}
		applyToggleStyle(dashBtn, isPenDash);
		updateDashAvailability();
		win->onToolStyleChanged();
	});
	registerStyleToggle([this] { return isPenDash; }, [this] { isPenDash = false; });
	updateDashAvailability();
	}   // !isPenFade

	initColorTrigger();
	initSlider();
}

void ToolSub::showNumberTools()
{
	// 同一栏因点击重建时不要再套用初始属性，否则刚选的样式会被默认形态顶回去
	const bool fresh = (curToolId != L"number" || !hasTools);
	beginTool(L"number");
	isNumberFill = true;
	// 初始属性：默认序号类型（0 数字 / 1 字母 / 2 Emoji）
	int defaultNum = NumberDigit;
	{
		const int form = designInt(ToolKey::Number, DesignKey::Form, -1);
		if (form >= NumberDigit && form <= NumberEmoji) defaultNum = form;
	}
	if (fresh) numberStyle = defaultNum;
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
		// 字母序号右边带「当前序号」输入框：把它的宽度一起算进属性栏
		const float inputW = measureNumberInputW(numberInputDisplay());
		initSize(2, true, false, inputW + 8.f);
	}
	makeNumberStyleBtns();
	if (!emojiMode) initColorTrigger();
	initSlider();
}

void ToolSub::showTextTools()
{
	const bool newBar = (curToolId != L"text" || !hasTools);
	beginTool(L"text");
	// 初始属性：0 常规 / 1 粗体 / 2 斜体 / 3 粗斜体
	{
		const int form = designInt(ToolKey::Text, DesignKey::Form, -1);
		if (form >= 0) {
			isTextBold = (form & 1) != 0;
			isTextItalic = (form & 2) != 0;
		}
	}
	if (newBar) {
		isTextStroke = false;
		textStrokeWidth = 2.f;
		strokeColorIndex = 5;   // 默认白色
	}
	initSize(3, true);
	boldBtn = makeToggleBtn(Icon::Bold, &isTextBold, L"tool.bold", L"bold");
	italicBtn = makeToggleBtn(Icon::Italic, &isTextItalic, L"tool.italic", L"italic");
	// 描边按钮自己处理切换：开启时弹出二级属性栏，关闭时收起来。
	// 不能拆成 makeToggleBtn + 第二个 onClick —— 事件触发顺序不保证，面板会时开时不开。
	strokeBtn = contentNode->makeChild<Ling::Button>();
	strokeBtn->setText(Icon::Stroke);
	styleToolbarBtn(strokeBtn);
	strokeBtn->setFontFamily(Icon::Family);
	strokeBtn->setFontSize(Icon::Size);
	applyToggleStyle(strokeBtn, isTextStroke);
	tip->bind(strokeBtn, Lang::get(L"tool.textStroke"));
	strokeBtn->onClick.add([this, b = strokeBtn](Ling::Button*) {
		const bool strokePanelOn = nestedPanel && nestedPanel->isOpen()
			&& nestedPanel->kind() == ToolNestedPanel::Kind::TextStroke;
		const bool colorPopupOn = colorPanel && colorPanel->isOpen();
		const bool otherSecondaryOn = nestedPanel && nestedPanel->isOpen()
			&& nestedPanel->kind() != ToolNestedPanel::Kind::TextStroke;

		// 描边已开启且描边二级栏就是当前栏、又没有三级颜色面板压在上面：
		// 这才算“再次点按描边”，关闭描边。
		if (isTextStroke && strokePanelOn && !colorPopupOn && !otherSecondaryOn) {
			isTextStroke = false;
			applyToggleStyle(b, false);
			closeNestedPanel();
			closeColorPopup();
			win->onToolStyleChanged();
			return;
		}

		// 其余情况都是“继续打开描边属性栏”：
		// 例如二级栏里点开颜色后，描边仍高亮，再点描边只收起颜色面板，不关闭描边。
		isTextStroke = true;
		applyToggleStyle(b, true);
		closeColorPopup();
		if (!strokePanelOn && nestedPanel)
			nestedPanel->openTextStroke(workAreaRect(), b);
		win->onToolStyleChanged();
	});
	registerStyleToggle([this] { return isTextBold; }, [this] { isTextBold = false; });
	registerStyleToggle([this] { return isTextItalic; }, [this] { isTextItalic = false; });
	registerStyleToggle([this] { return isTextStroke; }, [this] {
		isTextStroke = false;
		updateTextStrokeControls();
		});
	initColorTrigger();
	initSlider();
	updateTextStrokeControls();
}

void ToolSub::showMosaicTools()
{
	beginTool(L"mosaic");
	// 初始属性：0 马赛克 / 1 模糊
	int defaultMosaic = 0;
	{
		const int form = designInt(ToolKey::Mosaic, DesignKey::Form, -1);
		if (form >= 0) {
			defaultMosaic = (form == 1) ? 1 : 0;
			isMosaicBlur = (defaultMosaic == 1);
		}
	}
	initSize(1, false, true);
	// 属性栏只放非默认形态；默认形态已经在工具栏按钮上
	for (const auto& c : orderedFormsFor(L"mosaic", defaultMosaic)) {		if (c.value == 1) {
			// 模糊：复合图标 = 模糊2 全不透明 + 模糊1 @25%
			overlayBtn = makeOverlayToggleBtn(Icon::Blur2, Icon::Blur, 0.25f, &isMosaicBlur, L"tool.blur");
		}
		else {
			overlayBtn = contentNode->makeChild<Ling::Button>();
			overlayBtn->setText(c.glyph ? c.glyph : Icon::Mosaic);
			styleToolbarBtn(overlayBtn);
			overlayBtn->setFontFamily(Icon::Family);
			overlayBtn->setFontSize(Icon::Size);
			applyToggleStyle(overlayBtn, !isMosaicBlur);
			tip->bind(overlayBtn, Lang::get(c.tipKey ? c.tipKey : L"tool.mosaic"));
			overlayBtn->onClick.add([this](Ling::Button*) {
				isMosaicBlur = false;
				if (overlayBtn) applyToggleStyle(overlayBtn, true);
				win->onToolStyleChanged();
			});
		}
	}
	registerStyleToggle([this] { return isMosaicBlur != (designInt(ToolKey::Mosaic, DesignKey::Form, 0) == 1); },
		[this] { isMosaicBlur = designInt(ToolKey::Mosaic, DesignKey::Form, 0) == 1; });
	initSlider();
}

void ToolSub::showPatinaTools()
{
	beginTool(L"patina");
	// 初始属性：按屏幕放置方案（0 右下 / 1 右下角 / 2 中下）
	{
		const int form = designInt(ToolKey::Patina, DesignKey::Form, -1);
		if (form >= 0) patinaPlan = std::clamp(form, 0, 2);
	}
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
	// 初始属性：默认倾斜角度
	{
		const int angle = designInt(ToolKey::Watermark, DesignKey::Angle, -1);
		if (angle >= 0) watermarkAngle = (float)std::clamp(angle, 0, 360);
	}
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
	// 初始属性：0 矩形 / 1 椭圆；边框开关单独存
	int defaultHi = 0;
	{
		const int form = designInt(ToolKey::Highlight, DesignKey::Form, -1);
		if (form >= 0) {
			defaultHi = (form == 1) ? 1 : 0;
			isHighlightEllipse = (form == 1);
		}
		const int border = designInt(ToolKey::Highlight, DesignKey::Border, -1);
		if (border >= 0) isHighlightBorder = (border == 1);
	}
	initSize(2, true, true);
	// 属性栏只放非默认形态；默认形态已经在工具栏按钮上
	for (const auto& c : orderedFormsFor(L"highlight", defaultHi)) {
		if (c.value == 1) {
			hiEllipseBtn = makeToggleBtn(c.glyph ? c.glyph : Icon::Ellipse, &isHighlightEllipse,
				c.tipKey ? c.tipKey : L"tool.highlightEllipse", L"ellipse");
		}
		else {
			auto* b = contentNode->makeChild<Ling::Button>();
			hiRectBtn = b;
			b->setText(c.glyph ? c.glyph : Icon::Rect);
			styleToolbarBtn(b);
			b->setFontFamily(Icon::Family);
			b->setFontSize(Icon::Size);
			applyToggleStyle(b, !isHighlightEllipse);
			tip->bind(b, Lang::get(c.tipKey ? c.tipKey : L"tool.highlightRect"));
			b->onClick.add([this, b](Ling::Button*) {
				isHighlightEllipse = false;
				applyToggleStyle(b, true);
				win->onToolStyleChanged();
			});
		}
	}
	registerStyleToggle([this] { return isHighlightEllipse != (designInt(ToolKey::Highlight, DesignKey::Form, 0) == 1); },
		[this] { isHighlightEllipse = designInt(ToolKey::Highlight, DesignKey::Form, 0) == 1; });
	hiBorderBtn = makeToggleBtn(Icon::DashRect, &isHighlightBorder, L"tool.highlightBorder", L"border");
	registerStyleToggle([this] { return isHighlightBorder; }, [this] { isHighlightBorder = false; });
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
		recTimerLabel->setText(wfmt(L"%02d:%02d", (int)(sec / 60), (int)(sec % 60)));
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

void ToolSub::drawChrome(ID2D1DeviceContext* ctx)
{
	ToolbarChrome::paintPropBubble(ctx, w, h, dpi, brushBg.Get(), arrowX, tipDown_);
}

void ToolSub::layout()
{
	ToolbarWindow::layout();
	// 自定义色块的七彩渐变：与颜色面板共用同一份绘制，尺寸按当前布局重画
	PaintCustomColorSwatch(boardCustomCanvas, dpi);
}

void ToolSub::applyTipDirection(bool tipDown)
{
	tipDown_ = tipDown;
	ToolbarChrome::applyPropBubbleContentPad(contentNode, tipDown_);
}

ToolSub::ColorTriggerUi ToolSub::makeColorTrigger(Ling::Node* parent)
{
	ColorTriggerUi ui;
	if (!parent) return ui;
	ui.btn = parent->makeChild<Ling::Button>();
	styleToolbarBtn(ui.btn);
	ui.ring = ui.btn->makeChild<Ling::Node>();
	ui.ring->setSize(swatchRing, swatchRing);
	ui.ring->setBorderRadius(swatchRadius + 1.f);
	ui.ring->setAlignItems(Ling::Align::Center);
	ui.ring->setJustifyContent(Ling::Justify::Center);
	ui.swatch = ui.ring->makeChild<Ling::Label>();
	ui.swatch->setSize(swatchSize, swatchSize);
	ui.swatch->setBorderRadius(swatchRadius);
	return ui;
}

void ToolSub::initColorTrigger()
{
	auto ui = makeColorTrigger(contentNode);
	colorTrigger = ui.btn;
	colorTriggerRing = ui.ring;
	colorTriggerSwatch = ui.swatch;
	tip->bind(colorTrigger, L"颜色");
	colorTrigger->onClick.add([this](Ling::Button*) { toggleColorPanel(false); });
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

void ToolSub::openStrokeColorPanel(float screenCenterX, float screenBottom, const RECT& parentRect)
{
	if (!colorPanel) return;
	// 同一个描边色块再次点击 = 收起颜色面板
	if (colorPanel->isOpen() && colorPanelForStroke_) {
		closeColorPanel();
		return;
	}
	colorPanelForStroke_ = true;
	colorAnchorCX_ = screenCenterX;
	colorAnchorBottom_ = screenBottom;
	colorParentOverride_ = true;
	colorParentRect_ = parentRect;
	if (colorPanel->isOpen()) colorPanel->closePanel();
	colorPanel->open(workAreaRect());
}

RECT ToolSub::colorPanelParentBodyRect() const
{
	RECT r = colorParentOverride_
		? colorParentRect_
		: RECT{ x, y, x + static_cast<int>(w), y + static_cast<int>(h) };
	const bool parentAbove = colorParentOverride_ && nestedPanel
		? nestedPanel->tipDown() : tipDown();
	const int caret = static_cast<int>(std::lround(ToolbarTheme::caretSize * dpi));
	if (parentAbove) r.bottom -= caret;
	else r.top += caret;
	return r;
}

bool ToolSub::colorPanelPreferBelow() const
{
	// 颜色面板往父栏的外侧展开：父栏在上就继续上弹，在下就下弹
	if (colorParentOverride_ && nestedPanel) return !nestedPanel->tipDown();
	return !tipDown();
}

void ToolSub::toggleColorPanel(bool stroke)
{
	if (!colorPanel) return;
	closeNestedPanel();
	if (colorPanel->isOpen() && colorPanelForStroke_ == stroke) {
		closeColorPanel();
		return;
	}
	// 切换目标色块：先清掉旧高亮，再以新目标重建色板
	colorPanelForStroke_ = stroke;
	colorPanel->closePanel();
	if (colorTriggerRing) colorTriggerRing->setBorder(0.f, 0);
	colorPanel->open(workAreaRect());
	if (!stroke && colorTriggerRing)
		colorTriggerRing->setBorder(1.f, Icon::ColorActive);
}

void ToolSub::closeColorPanel()
{
	if (colorPanel) colorPanel->closePanel();
	if (colorTriggerRing) {
		colorTriggerRing->setBorder(0.f, 0);
	}
	colorPanelForStroke_ = false;
	colorAnchorCX_ = -1.f;
	colorAnchorBottom_ = -1.f;
	colorParentOverride_ = false;
	if (nestedPanel && nestedPanel->kind() == ToolNestedPanel::Kind::TextStroke)
		nestedPanel->refreshTextStrokeSwatch();
}

void ToolSub::closeColorPopup()
{
	closeColorPanel();
}

void ToolSub::updateTextStrokeControls()
{
	if (curToolId != L"text" || !hasTools) return;
	if (strokeBtn) applyToggleStyle(strokeBtn, isTextStroke);
	// 关闭描边时同步收起二级属性栏与描边颜色面板
	if (!isTextStroke) {
		if (nestedPanel && nestedPanel->kind() == ToolNestedPanel::Kind::TextStroke)
			closeNestedPanel();
		if (colorPanelForStroke_) closeColorPanel();
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

void ToolSub::resetStyleToDefaults()
{
	// 关掉工具再打开 = 属性栏回到「初始属性」，不沿用上一次的选择
	const int rf = designInt(ToolKey::Rect, DesignKey::Form, 0);
	isEllipse = (rf == 1);
	rectKind = (rf == 2) ? PolygonShape : (rf == 3 ? StarShape : RectShape);
	isRectFill = false;
	isRectDash = false;
	cornerRadius = 0.f;
	lastCornerRadius = 0.f;
	isArrowDash = false;
	isArrowRound = false;
	arrowHead = designInt(ToolKey::Arrow, DesignKey::Head, ArrowDefault);
	isLine = false;
	lineKind = designInt(ToolKey::Line, DesignKey::Form, LineDimension);
	isPenDash = false;
	isPenSoft = false;
	isPenMarker = false;
	const int tf = designInt(ToolKey::Text, DesignKey::Form, 0);
	isTextBold = (tf & 1) != 0;
	isTextItalic = (tf & 2) != 0;
	isTextStroke = false;
	textStrokeWidth = 2.f;
	strokeColorIndex = 5;
	isHighlightBorder = designInt(ToolKey::Highlight, DesignKey::Border, 0) != 0;
	isHighlightEllipse = designInt(ToolKey::Highlight, DesignKey::Form, 0) == 1;
	isMosaicBlur = designInt(ToolKey::Mosaic, DesignKey::Form, 0) == 1;
	numberStyle = designInt(ToolKey::Number, DesignKey::Form, NumberDigit);
	// 颜色/粗细的会话记忆也清掉：重新打开按初始属性来
	selectColorIndex = 0;
	sliderMem.clear();
	colorIndexMem.clear();
	updateDashAvailability();
}

void ToolSub::beginShapeStylePreview()
{
	if (stylePreviewActive_) return;
	auto& s = stylePreview_;
	s.isEllipse = isEllipse; s.isRectFill = isRectFill; s.isRectDash = isRectDash;
	s.isArrowDash = isArrowDash; s.isArrowRound = isArrowRound; s.isLine = isLine;
	s.isPenDash = isPenDash; s.isPenSoft = isPenSoft; s.isPenMarker = isPenMarker;
	s.isTextBold = isTextBold; s.isTextItalic = isTextItalic; s.isTextStroke = isTextStroke;
	s.textStrokeWidth = textStrokeWidth; s.strokeColorIndex = strokeColorIndex;
	s.isHighlightBorder = isHighlightBorder; s.isHighlightEllipse = isHighlightEllipse;
	s.isMosaicBlur = isMosaicBlur;
	s.rectKind = rectKind; s.polygonSides = polygonSides; s.starPoints = starPoints;
	s.arrowHead = arrowHead; s.lineKind = lineKind; s.numberStyle = numberStyle;
	s.cornerRadius = cornerRadius; s.lastCornerRadius = lastCornerRadius;
	s.sliderVal = sliderVal; s.sliderMin = sliderMin; s.sliderMax = sliderMax;
	s.curSliderKey = curSliderKey;
	s.selectColorIndex = selectColorIndex;
	s.sessionCustomColor = sessionCustomColor;
	s.customColor = colors[customColorIndex];
	s.sliderMem = sliderMem;
	s.colorIndexMem = colorIndexMem;
	stylePreviewActive_ = true;
}

void ToolSub::endShapeStylePreview()
{
	if (!stylePreviewActive_) return;
	stylePreviewActive_ = false;
	auto& s = stylePreview_;
	isEllipse = s.isEllipse; isRectFill = s.isRectFill; isRectDash = s.isRectDash;
	isArrowDash = s.isArrowDash; isArrowRound = s.isArrowRound; isLine = s.isLine;
	isPenDash = s.isPenDash; isPenSoft = s.isPenSoft; isPenMarker = s.isPenMarker;
	isTextBold = s.isTextBold; isTextItalic = s.isTextItalic; isTextStroke = s.isTextStroke;
	textStrokeWidth = s.textStrokeWidth; strokeColorIndex = s.strokeColorIndex;
	isHighlightBorder = s.isHighlightBorder; isHighlightEllipse = s.isHighlightEllipse;
	isMosaicBlur = s.isMosaicBlur;
	rectKind = s.rectKind; polygonSides = s.polygonSides; starPoints = s.starPoints;
	arrowHead = s.arrowHead; lineKind = s.lineKind; numberStyle = s.numberStyle;
	cornerRadius = s.cornerRadius; lastCornerRadius = s.lastCornerRadius;
	sliderVal = s.sliderVal; sliderMin = s.sliderMin; sliderMax = s.sliderMax;
	curSliderKey = s.curSliderKey;
	selectColorIndex = s.selectColorIndex;
	sessionCustomColor = s.sessionCustomColor;
	colors[customColorIndex] = s.customColor;
	sliderMem = s.sliderMem;
	colorIndexMem = s.colorIndexMem;
	refreshStyleToggles();
	updateSliderValueLabel();
	if (slider) {
		suppressSliderStyleNotify = true;
		slider->setValue(sliderVal);
		suppressSliderStyleNotify = false;
	}
	updateColorTriggerSwatch();
	updateTextStrokeControls();
	refreshNestedTrigger();
	if (curToolId == L"number") syncNumberInput();
}

void ToolSub::commitShapeStylePreview()
{
	if (!stylePreviewActive_) return;
	stylePreviewActive_ = false;
}

void ToolSub::registerStyleToggle(std::function<bool()> active, std::function<void()> reset)
{
	styleToggles.push_back({ std::move(active), std::move(reset) });
}

// 属性栏里是否有开着的选项：右键分层回退时先清这些，再退工具
bool ToolSub::hasActiveStyleOption() const
{
	for (const auto& t : styleToggles) {
		if (t.active && t.active()) return true;
	}
	return false;
}

// 从右到左关闭一个：按登记顺序（即属性栏按钮创建顺序，左到右）倒序查找第一个仍开启的选项。
// 不按工具硬编码顺序；属性栏调整顺序或去掉某个按钮时，此处行为自动一致。
bool ToolSub::resetStyleOptions()
{
	for (auto it = styleToggles.rbegin(); it != styleToggles.rend(); ++it) {
		if (!it->active || !it->active()) continue;
		if (it->reset) it->reset();
		refreshStyleToggles();
		if (curToolId == L"number") syncNumberInput();
		win->onToolStyleChanged();
		return true;
	}
	return false;
}

// 二次选中并把标注属性写回工具状态后，刷新区间里所有开关按钮的选中态
void ToolSub::refreshStyleToggles()
{
	if (!hasTools) return;
	if (fillBtn) applyToggleStyle(fillBtn, isRectFill);
	if (dashBtn) {
		bool on = isArrowDash;
		if (curToolId == L"rect") on = isRectDash;
		else if (curToolId == L"pen") on = isPenDash;
		applyToggleStyle(dashBtn, on);
	}
	if (roundBtn) applyToggleStyle(roundBtn, cornerRadius > 0.f);
	if (ellipseBtn) applyToggleStyle(ellipseBtn, isEllipse);
	if (lineBtn) applyToggleStyle(lineBtn, isLine);
	if (softBtn) applyToggleStyle(softBtn, isPenSoft);
	if (markerBtn) applyToggleStyle(markerBtn, isPenMarker);
	if (penBaseBtn) applyToggleStyle(penBaseBtn, !isPenMarker && !isPenSoft);
	if (boldBtn) applyToggleStyle(boldBtn, isTextBold);
	if (italicBtn) applyToggleStyle(italicBtn, isTextItalic);
	if (strokeBtn) applyToggleStyle(strokeBtn, isTextStroke);
	if (hiEllipseBtn) applyToggleStyle(hiEllipseBtn, isHighlightEllipse);
	if (hiRectBtn) applyToggleStyle(hiRectBtn, !isHighlightEllipse);
	if (hiBorderBtn) applyToggleStyle(hiBorderBtn, isHighlightBorder);
	if (arrowDashBtn) applyToggleStyle(arrowDashBtn, isArrowDash);
	if (arrowRoundBtn) applyToggleStyle(arrowRoundBtn, isArrowRound);
	if (overlayBtn) {
		if (overlayFlag) applyOverlayToggleStyle(*overlayFlag);
		else applyToggleStyle(overlayBtn, !isMosaicBlur);
	}
	refreshRectKindBtns();
	refreshLineKindBtns();
	refreshArrowHeadBtns();
	refreshNumberStyleBtns();
	refreshNestedTrigger();
	updateDashAvailability();
}

void ToolSub::refreshNestedTrigger()
{
	refreshRoundBtnStyle();
	if (angleBtn) {
		const bool on = nestedPanel && nestedPanel->isOpen()
			&& nestedPanel->kind() == ToolNestedPanel::Kind::WmAngle;
		applyToggleStyle(angleBtn, on);
	}
	if (patinaWmBtn) applyToggleStyle(patinaWmBtn, isPatinaWatermark);
	if (markerBtn) applyToggleStyle(markerBtn, isPenMarker);
	refreshNumberStyleBtns();
}

RECT ToolSub::workAreaRect() const
{
	RECT winRect{ x, y, x + static_cast<int>(w), y + static_cast<int>(h) };
	return ToolbarChrome::workAreaNear(winRect);
}

// 将色板指向指定颜色（0xRRGGBBAA）：二次选中标注时展示其自身颜色。
void ToolSub::selectColorByValue(UINT32 rgba)
{
	if (rgba == 0) return;
	UINT idx = static_cast<UINT>(customColorIndex);
	bool found = false;
	for (size_t i = 0; i < colors.size(); ++i) {
		if (colors[i] == rgba) { idx = static_cast<UINT>(i); found = true; break; }
	}
	if (!found) {
		sessionCustomColor = rgba;
		colors[customColorIndex] = rgba;
	}
	if (!curToolId.empty()) colorIndexMem[curToolId] = idx;
	if (selectColorIndex != idx) {
		selectColorIndex = idx;
		if (colorPanel && colorPanel->isOpen()) colorPanel->refreshSelection();
	}
	updateColorTriggerSwatch();
}

void ToolSub::setStrokeColorIndex(size_t index)
{
	if (index >= colors.size()) return;
	strokeColorIndex = static_cast<UINT>(index);
	if (nestedPanel && nestedPanel->kind() == ToolNestedPanel::Kind::TextStroke)
		nestedPanel->refreshTextStrokeSwatch();
	win->onToolStyleChanged();
}

void ToolSub::selectStrokeColorByValue(UINT32 rgba)
{
	if (rgba == 0) return;
	UINT idx = static_cast<UINT>(customColorIndex);
	bool found = false;
	for (size_t i = 0; i < colors.size(); ++i) {
		if (colors[i] == rgba) { idx = static_cast<UINT>(i); found = true; break; }
	}
	if (!found) {
		sessionCustomColor = rgba;
		colors[customColorIndex] = rgba;
	}
	if (strokeColorIndex != idx) {
		strokeColorIndex = idx;
		if (colorPanel && colorPanel->isOpen() && colorPanelForStroke_)
			colorPanel->refreshSelection();
	}
	if (nestedPanel && nestedPanel->kind() == ToolNestedPanel::Kind::TextStroke)
		nestedPanel->refreshTextStrokeSwatch();
}

void ToolSub::onColorPicked(size_t index)
{
	if (index >= colors.size()) return;
	const bool forStroke = colorPanelForStroke_;
	if (index == customColorIndex) {
		if (!pickCustomColor()) return;
	}
	else if (index == (forStroke ? strokeColorIndex : selectColorIndex)) {
		closeColorPanel();
		return;
	}
	if (forStroke) {
		strokeColorIndex = static_cast<UINT>(index);
		if (nestedPanel && nestedPanel->kind() == ToolNestedPanel::Kind::TextStroke)
			nestedPanel->refreshTextStrokeSwatch();
	}
	else {
		selectColorIndex = static_cast<UINT>(index);
		if (!curToolId.empty())
			colorIndexMem[curToolId] = selectColorIndex;
		updateColorTriggerSwatch();
	}
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
	if (colorPanelForStroke_ && colorAnchorCX_ >= 0.f) {
		screenCenterX = colorAnchorCX_;
		screenBottom = colorAnchorBottom_;
		return;
	}
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
	numberStyleVals.clear();
	emojiTrigger = nullptr;
	numberInput = nullptr;
	int defaultNum = NumberDigit;
	{
		const int form = designInt(ToolKey::Number, DesignKey::Form, -1);
		if (form >= NumberDigit && form <= NumberEmoji) defaultNum = form;
	}
	// 再点同一项回到默认形态
	auto addStyleBtn = [this, defaultNum](const wchar_t* icon, int style, const wchar_t* tipKey) {
		auto btn = contentNode->makeChild<Ling::Button>();
		btn->setText(icon);
		styleToolbarBtn(btn);
		btn->setFontFamily(Icon::Family);
		btn->setFontSize(Icon::Size);
		tip->bind(btn, Lang::get(tipKey));
		btn->onClick.add([this, style, defaultNum](Ling::Button*) {
			numberStyle = (numberStyle == style) ? defaultNum : style;
			showNumberTools(); // 重建：Emoji 模式多出表情入口 / 滑条范围
			win->onToolStyleChanged();
		});
		numberStyleBtns.push_back(btn);
		numberStyleVals.push_back(style);
		registerStyleToggle([this, style] { return numberStyle == style; },
			[this, defaultNum] { numberStyle = defaultNum; });
	};

	// 形态顺序跟随「初始属性」；默认形态已在工具栏，属性栏里不再重复
	for (const auto& c : orderedFormsFor(L"number", defaultNum)) {
		addStyleBtn(c.glyph ? c.glyph : Icon::Number, c.value,
			c.tipKey ? c.tipKey : L"tool.numberDefault");
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
	else buildNumberInput();
	refreshNumberStyleBtns();
}

// 序号文案：数字 → 1/2/3，字母 → A/B/…/Z/AA（与 ShapeNumber::formatLabel 一致）
std::wstring ToolSub::formatNumberLabel(int style, int val)
{
	if (style == NumberLetter) {
		std::wstring s;
		int n = val;
		while (n > 0) {
			n -= 1;
			s.insert(s.begin(), (wchar_t)(L'A' + (n % 26)));
			n /= 26;
		}
		return s.empty() ? L"A" : s;
	}
	return std::to_wstring(val);
}

// 按已有形状推算下一个序号（与 ShapeNumber::getNextVal 同一规则）
int ToolSub::autoNextNumber(int style) const
{
	int maxVal{ 0 };
	if (!win->history) return 1;
	for (auto& shape : win->history->shapes) {
		auto* number = dynamic_cast<ShapeNumber*>(shape.get());
		if (number && !number->isUndo && number->style() == style && number->numberVal() > maxVal)
			maxVal = number->numberVal();
	}
	return maxVal + 1;
}

std::wstring ToolSub::numberInputDisplay() const
{
	if (numberShowsSelected_) return selectedNumberLabel_;
	// 手动输入的文字优先（中文等）；否则显示下一个自然序号
	if (!numberCustomLabel.empty()) return numberCustomLabel;
	const int val = numberStartVal > 0 ? numberStartVal : autoNextNumber(numberStyle);
	return formatNumberLabel(numberStyle, val);
}

void ToolSub::showSelectedNumberValue(const std::wstring& text)
{
	numberShowsSelected_ = true;
	selectedNumberLabel_ = text;
	if (!numberInput) return;
	numberInputSyncing = true;
	numberInput->setText(text);
	numberInputSyncing = false;
	numberInput->setWidth(measureNumberInputW(text));
	sizeExtraW = measureNumberInputW(text) + 8.f;
	refreshSize();
	win->layoutTools();
}

void ToolSub::clearSelectedNumberValue()
{
	if (!numberShowsSelected_) return;
	numberShowsSelected_ = false;
	selectedNumberLabel_.clear();
	syncNumberInput();
	refreshSize();
	win->layoutTools();
}

void ToolSub::blurInputs()
{
	if (numberInput && numberInput->isFocused()) numberInput->blur();
	if (wmTextBox && wmTextBox->isFocused()) wmTextBox->blur();
	// 输入框偶发残留鼠标捕获：鼠标已经回到画布，捕获还挂在属性栏上会导致画布点不动
	HWND cap = GetCapture();
	if (cap && hwnd && (cap == hwnd || IsChild(hwnd, cap))) ReleaseCapture();
}

float ToolSub::measureNumberInputW(const std::wstring& text) const
{
	const std::wstring sample = text.empty() ? L"1" : text;
	auto layout = Ling::D2D::get()->makeTextLayout(sample, 12.f * dpi);
	float tw = 0.f;
	if (layout) {
		DWRITE_TEXT_METRICS m{};
		layout->GetMetrics(&m);
		tw = m.widthIncludingTrailingWhitespace / dpi;
	}
	return std::clamp(tw + 16.f, 34.f, 160.f);
}

void ToolSub::buildNumberInput()
{
	constexpr float marginH{ 4.f };
	numberInput = contentNode->makeChild<Ling::TextBox>();
	numberInput->setMargin(marginH, 0.f, marginH, 0.f);
	numberInput->setFlexGrow(0.f);
	numberInput->setFlexShrink(0.f);
	numberInput->setBg(Ling::Color(ToolbarTheme::hoverBg));
	numberInput->setBorder(0.f, 0);
	numberInput->setBorderRadius(6.f);
	numberInput->setPadding(6.f, 2.f, 6.f, 2.f);
	numberInput->setFontSize(12.f);
	// 字色/光标色跟随工具栏主题（深色主题下用浅色，避免深底深字看不见）
	numberInput->setColor(Ling::Color(ToolbarTheme::iconNormal));
	numberInput->setCaretColor(Ling::Color(ToolbarTheme::iconNormal));
	numberInput->setSelectionBgColor(Ling::Color(0x34C75966u));
	numberInput->setAutoSize(true);
	numberInput->setVerticalCenter(true);
	numberInput->setTextAlignCenter(true);   // 序号在输入框里居中
	numberInputSyncing = true;
	numberInput->setText(numberInputDisplay());
	numberInputSyncing = false;
	numberInput->setWidth(measureNumberInputW(numberInputDisplay()));
	// 点进来就全选：直接输入新的数字或文字即可替换掉当前序号
	numberInput->onFocusChanged.add([this](Ling::TextBox*, bool focused) {
		if (!numberInput) return;
		if (focused) numberInput->selectAll();
		else syncNumberInput();   // 空文本 / 非规范输入 → 失焦时回写成当前序号
	});
	numberInput->onTextChanged.add([this](Ling::TextBox*, const std::wstring&) {
		applyNumberInputEdited();
	});
	numberInput->onEnter.add([this](Ling::TextBox* tb) {
		if (tb) tb->blur();
	});
}

void ToolSub::applyNumberInputEdited()
{
	if (numberInputSyncing || numberApplying_ || !numberInput) return;
	struct ApplyGuard {
		bool& applying;
		~ApplyGuard() { applying = false; }
	} guard{ numberApplying_ };
	numberApplying_ = true;
	std::wstring text = numberInput->getText();
	const auto isSpace = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
	while (!text.empty() && isSpace(text.front())) text.erase(text.begin());
	while (!text.empty() && isSpace(text.back())) text.pop_back();

	// 二次选中序号标注：输入框改的是这个标注本身，不改变"下一次落笔"的序号
	if (numberShowsSelected_) {
		if (auto* num = dynamic_cast<ShapeNumber*>(win->shapeHover)) {
			num->setDisplayLabel(text);
			selectedNumberLabel_ = num->displayLabel();
			numberInput->setWidth(measureNumberInputW(selectedNumberLabel_));
			sizeExtraW = measureNumberInputW(selectedNumberLabel_) + 8.f;
			// 文本框正在派发 onTextChanged：把属性栏重排推迟到本次输入事件之后，
			// 避免在 TextBox 内部再次布局自身导致卡住。
			if (auto* app = Ling::App::get()) {
				auto* sub = this;
				auto* w = win;
				app->dq.TryEnqueue([sub, w]() {
					sub->refreshSize();
					w->layoutTools();
				});
			}
			return;
		}
		numberShowsSelected_ = false;
	}

	bool digits = !text.empty();
	for (auto c : text) if (c < L'0' || c > L'9') { digits = false; break; }

	if (text.empty()) {
		// 清空 = 回到自动序号（失焦时再把显示补回来，避免打字过程被顶掉）
		numberStartVal = 0;
		numberCustomLabel.clear();
	}
	else if (digits) {
		numberStartVal = std::clamp(_wtoi(text.c_str()), 1, 9999);
		numberCustomLabel.clear();
	}
	else if (numberStyle == NumberLetter && text.size() == 1
		&& ((text[0] >= L'A' && text[0] <= L'Z') || (text[0] >= L'a' && text[0] <= L'z'))) {
		numberStartVal = towupper(text[0]) - L'A' + 1;
		numberCustomLabel.clear();
	}
	else {
		// 非数字（例如中文）：作为下一次落笔的字面序号
		numberStartVal = 0;
		numberCustomLabel = text;
	}
	numberInput->setWidth(measureNumberInputW(text));
	sizeExtraW = measureNumberInputW(text) + 8.f;
	refreshSize();
	win->layoutTools();
}

void ToolSub::syncNumberInput()
{
	if (!numberInput) return;
	numberInputSyncing = true;
	numberInput->setText(numberInputDisplay());
	numberInputSyncing = false;
	numberInput->setWidth(measureNumberInputW(numberInputDisplay()));
}

int ToolSub::takeNumberValue(int style, int autoVal, std::wstring& label)
{
	// 开始落新的序号：退出"显示选中标注"状态，输入框回到下一个自然序号
	numberShowsSelected_ = false;
	selectedNumberLabel_.clear();
	if (style == NumberEmoji) return autoVal;
	int val = numberStartVal > 0 ? numberStartVal : autoVal;
	if (!numberCustomLabel.empty()) {
		label = numberCustomLabel;
		numberCustomLabel.clear();
	}
	// 手动指定过起始值：落笔后自增，输入框随之显示下一个
	if (numberStartVal > 0) numberStartVal = val + 1;
	syncNumberInput();
	return val;
}

void ToolSub::updateEmojiTriggerGlyph()
{
	if (!emojiTrigger) return;
	emojiTrigger->setText(serialEmoji.empty() ? SerialEmoji::Default : serialEmoji);
}

void ToolSub::refreshNumberStyleBtns()
{
	for (size_t i = 0; i < numberStyleBtns.size() && i < numberStyleVals.size(); i++)
		applyToggleStyle(numberStyleBtns[i], numberStyle == numberStyleVals[i]);
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
	registerStyleToggle([this] { return isRectFill; }, [this] { isRectFill = false; });

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
	registerStyleToggle([this] { return isRectDash; }, [this] { isRectDash = false; });

	roundBtn = contentNode->makeChild<Ling::Button>();
	roundBtn->setText(Icon::RoundCorner);
	styleToolbarBtn(roundBtn);
	roundBtn->setFontFamily(Icon::Family);
	roundBtn->setFontSize(Icon::Size);
	refreshRoundBtnStyle();
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
	registerStyleToggle([this] { return cornerRadius > 0.f; }, [this] {
		lastCornerRadius = cornerRadius;
		cornerRadius = 0.f;
		closeNestedPanel();
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
		isLine = false;   // 选任一箭头样式 = 退出「线段」模式
		// 记忆工具样式：记住这次选的箭头样式
		if (Setting::get()->getRememberToolStyle())
			Setting::get()->setLastArrowHead(arrowHead);
		refreshArrowHeadBtns();
		win->onToolStyleChanged();
	});
	registerStyleToggle([this, style] { return !isLine && arrowHead == style; }, [this] {
		arrowHead = designInt(ToolKey::Arrow, DesignKey::Head, ArrowDefault);
	});
	arrowHeadBtns.push_back(btn);
	arrowHeadStyles.push_back(style);
}

void ToolSub::makeArrowHeadBtns()
{
	arrowHeadBtns.clear();
	arrowHeadStyles.clear();
	makeArrowHeadBtn(Icon::ArrowBig, ArrowBig, L"tool.arrowBig");
	makeArrowHeadBtn(Icon::FunArrow, ArrowFun, L"tool.arrowFun");
	makeArrowHeadBtn(Icon::ArrowBoth, ArrowBoth, L"tool.arrowBoth");
	makeArrowHeadBtn(Icon::AnnotBoth, ArrowAnnot, L"tool.arrowAnnot");
	refreshArrowHeadBtns();
}

void ToolSub::refreshArrowHeadBtns()
{
	for (size_t i = 0; i < arrowHeadBtns.size() && i < arrowHeadStyles.size(); i++)
		applyToggleStyle(arrowHeadBtns[i], !isLine && arrowHead == arrowHeadStyles[i]);
	if (lineBtn) applyToggleStyle(lineBtn, isLine);
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
	// 滚轮步进固定为 1（逻辑值）。按量程的 1/40 计算会产生 4.6、5.2 等小数，
	// 标签取整后呈跳跃变化。
	const float step = 1.f;
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
	sliderValue->setText(wfmt(L"%d", static_cast<int>(std::round(sliderVal))));
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

D2D1_COLOR_F ToolSub::getTextStrokeColor() const
{
	return Ling::Color(colors[strokeColorIndex]).getD2DColor();
}

UINT32 ToolSub::getTextStrokeColorValue() const
{
	return colors[strokeColorIndex];
}

float ToolSub::getTextStrokeWidthPx() const
{
	return textStrokeWidth * dpi;
}

void ToolSub::setTextStrokeWidthPx(float px)
{
	textStrokeWidth = std::clamp(px / dpi, 1.f, 12.f);
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
	// 收属性栏：先把"展示选中标注属性"期间的工具状态还原
	endShapeStylePreview();
	hasTools = false;
	numberStyleBtns.clear();
	arrowHeadBtns.clear();
	arrowHeadStyles.clear();
	lineKindBtns.clear();
	bubbleKindBtn = nullptr;
	angleKindBtn = nullptr;
	waveKindBtn = nullptr;
	dimKindBtn = nullptr;
	fillBtn = nullptr;
	dashBtn = nullptr;
	softBtn = nullptr;
	boldBtn = nullptr;
	italicBtn = nullptr;
	hiEllipseBtn = nullptr;
	hiRectBtn = nullptr;
	hiBorderBtn = nullptr;
	arrowDashBtn = nullptr;
	arrowRoundBtn = nullptr;
	lineBtn = nullptr;
	markerBtn = nullptr;
	roundBtn = nullptr;
	if (sliderHover) {
		sliderHover = false;
		win->setSizePreviewHold(false);
	}
	closeColorPanel();
	closeNestedPanel();
	colorTrigger = nullptr;
	colorTriggerRing = nullptr;
	colorTriggerSwatch = nullptr;
	strokeBtn = nullptr;
	wmTextBox = nullptr;
	numberInput = nullptr;
	numberShowsSelected_ = false;
	selectedNumberLabel_.clear();
	angleBtn = nullptr;
	patinaWmBtn = nullptr;
	emojiTrigger = nullptr;
	overlayBtn = nullptr;
	overlayBaseLabel = nullptr;
	overlayTopLabel = nullptr;
	overlayFlag = nullptr;
	styleToggles.clear();
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

WinCap* ToolSub::maskHost() const
{
	// 属性栏的宿主可能是 WinCap（截图/长图）或 WinPin（钉图）
	return dynamic_cast<WinCap*>(win);
}

void ToolSub::hideExtraHoverTips()
{
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
		wa, refTop, refBottom, w, h, ToolbarTheme::propBubbleGap,
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
				wa, refTop, refBottom, w, h, ToolbarTheme::propBubbleGap, px, arrowAnchor,
				false, tipDown_, arrowX, x, y);
			if (overlapArea(flipped) < overlapArea(place)) place = flipped;
		}
	}
	// 换向后无条件同步箭头方向，避免方向状态残留导致间距按旧方向计算
	applyTipDirection(place.tipDown);
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
