#include "pch.h"
#include "../Win/WinCap.h"
#include "../History.h"
#include "../Util.h"
#include "../Lang.h"
#include "../Tip.h"
#include "ToolVideo.h"
#include "ToolSub.h"
#include "IconCodes.h"
#include "ToolbarChrome.h"
#include "ToolbarTheme.h"

ToolVideo::ToolVideo(WinCap* win) : Ling::WinBase(), win(win)
{
	dpi = win->dpi;
	refreshSize();
	onKeyDown.add([this](UINT key) { this->win->onKeyDown(key); });
	onTimer.add([this](UINT id) { this->onTimerCB(id); });
	onDpiChanged.add([this]() { dpiChanged = true; });
	onSizeChanged.add([this]() {
		if (!dpiChanged) return;
		dpiChanged = false;
		refreshSize();
		this->win->layoutTool(this);
	});
}

void ToolVideo::refreshSize()
{
	setSize(contentWidth() + ToolbarTheme::shadowPad * 2.f,
		btnSize + ToolbarTheme::shadowPad * 2.f);
	if (hwnd) layout();
}

ToolVideo::~ToolVideo()
{
}

float ToolVideo::iconSideInset() const
{
	return (btnSize - hoverInset * 2.f - Icon::Size) * 0.5f;
}

float ToolVideo::dragMarginRight() const
{
	// 与 ToolCap 一致：拖拽本体 → 开始按钮本体 = dragGap
	return ToolbarTheme::dragGap - hoverInset - iconSideInset();
}

float ToolVideo::contentPadRight() const
{
	return ToolbarTheme::paddingRight - hoverInset - iconSideInset();
}

float ToolVideo::contentWidth() const
{
	const float sep = spliterW + (ToolbarTheme::splitterGap - hoverInset) * 2.f;
	// 拖拽 + 穿透 + | + 播放 + 时间 + 音频 + 麦 + 7 标注 + | + 撤销 + | + GIF + 保存 + 取消 + 完成
	constexpr float iconBtns = 16.f; // pierce + play audio mic +7 annot + undo gif save cancel done
	const float drag = ToolbarTheme::paddingLeft + ToolbarTheme::dragIconSize + dragMarginRight();
	return drag + btnSize * iconBtns + timerW + sep * 3.f + contentPadRight();
}

float ToolVideo::getBtnCenterX() const
{
	// 物理像素：相对工具条左缘到当前标注按钮中心
	const float sep = (spliterW + (ToolbarTheme::splitterGap - hoverInset) * 2.f) * dpi;
	float x = (ToolbarTheme::shadowPad + ToolbarTheme::paddingLeft
		+ ToolbarTheme::dragIconSize + dragMarginRight()) * dpi;
	x += btnSize * dpi; // pierce
	x += sep;
	x += btnSize * dpi; // play
	x += timerW * dpi;
	x += btnSize * dpi * 2.f; // audio + mic
	static const wchar_t* kAnnot[] = {
		L"rect", L"arrow", L"pen", L"text", L"number", L"mosaic", L"eraser"
	};
	for (const auto* id : kAnnot) {
		if (curAnnotId_ == id) {
			x += btnSize * dpi * 0.5f;
			return x;
		}
		x += btnSize * dpi;
	}
	// 未选标注：箭头对准播放钮
	return (ToolbarTheme::shadowPad + ToolbarTheme::paddingLeft
		+ ToolbarTheme::dragIconSize + dragMarginRight()) * dpi
		+ btnSize * dpi // pierce
		+ sep
		+ btnSize * dpi * 0.5f;
}

void ToolVideo::onCreated()
{
	ToolbarTheme::refresh();   // 建窗时套用当前工具栏主题
	tip = std::make_unique<Tip>(this);
	tip->excludeFromCapture();
	body->setBg(0);
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(
		Ling::Color(ToolbarTheme::background).getD2DColor(), brushBg.GetAddressOf());
	chromeCanvas = body->makeChild<Ling::Canvas>();
	chromeCanvas->setPositionType(Ling::Position::Absolute);
	chromeCanvas->setSizePercent(100.f, 100.f);
	contentNode = body->makeChild<Ling::Node>();
	contentNode->setPositionType(Ling::Position::Absolute);
	contentNode->setPosition(Ling::Edge::Left, ToolbarTheme::shadowPad);
	contentNode->setPosition(Ling::Edge::Top, ToolbarTheme::shadowPad);
	contentNode->setPosition(Ling::Edge::Right, ToolbarTheme::shadowPad);
	contentNode->setPosition(Ling::Edge::Bottom, ToolbarTheme::shadowPad);
	contentNode->setAlignItems(Ling::Align::Center);
	contentNode->setFlexDirection(Ling::FlexDirection::Row);
	contentNode->setPaddingLeft(ToolbarTheme::paddingLeft);
	contentNode->setPaddingRight(contentPadRight());
	buildBar();
	onMouseDown.add([this](POINT pos, bool isRight) { onDragDown(pos, isRight); });
	onMouseMove.add([this](POINT pos) { onDragMove(pos); });
	onMouseUp.add([this](POINT pos, bool isRight) { onDragUp(pos, isRight); });
	show();
	updateUndoEnabled();
}

void ToolVideo::layout()
{
	Ling::WinBase::layout();
	paintChrome();
}

void ToolVideo::paintChrome()
{
	if (!chromeCanvas || !brushBg) return;
	auto ctx = chromeCanvas->startPaint();
	if (!ctx) return;
	ctx->Clear(0);
	const float pad = ToolbarTheme::shadowPad * dpi;
	const float radius = toolbarRadius * dpi;
	const float borderW = ToolbarTheme::borderWidth * dpi;
	const D2D1_RECT_F bar{ pad, pad, w - pad, h - pad };
	ToolbarChrome::paintRoundBar(ctx, bar, radius, brushBg.Get(), borderW);
	chromeCanvas->finishPaint();
}

void ToolVideo::onMinMaxInfo(MINMAXINFO* mmi)
{
	mmi->ptMinTrackSize.x = 1;
	mmi->ptMinTrackSize.y = 1;
}

Ling::Node* ToolVideo::makeSpliter()
{
	auto spliter = contentNode->makeChild<Ling::Node>();
	spliter->setSize(spliterW, spliterH);
	const float sepMarginV = (btnSize - spliterH) * 0.5f;
	const float sepMarginH = ToolbarTheme::splitterGap - hoverInset;
	spliter->setMargin(sepMarginH, sepMarginV, sepMarginH, sepMarginV);
	spliter->setBg(ToolbarTheme::splitter);
	spliter->setFlexGrow(0.f);
	spliter->setFlexShrink(0.f);
	return spliter;
}

Ling::Button* ToolVideo::makeIconBtn(const std::wstring& code, const std::wstring& id)
{
	auto btn = contentNode->makeChild<Ling::Button>();
	if (!id.empty()) btn->setId(id);
	btn->setText(code);
	const float inner{ btnSize - hoverInset * 2.f };
	btn->setSize(inner, inner);
	btn->setMargin(hoverInset);
	btn->setFlexGrow(0.f);
	btn->setFlexShrink(0.f);
	btn->setBorderRadius(hoverRadius);
	btn->setHoverBg(ToolbarTheme::hoverBg);
	btn->setAlignItems(Ling::Align::Center);
	btn->setJustifyContent(Ling::Justify::Center);
	btn->setFontFamily(Icon::Family);
	btn->setFontSize(Icon::Size);
	btn->setColor(Icon::ColorNormal);
	btn->setHoverColor(Icon::ColorNormal);
	return btn;
}

void ToolVideo::applyToggleStyle(Ling::Button* btn, bool selected)
{
	if (!btn) return;
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

void ToolVideo::buildBar()
{
	annotBtns.clear();
	btnPlay = btnAudio = btnMic = btnGif = btnUndo = btnPierce = nullptr;
	timerLabel = nullptr;
	dragHandle = nullptr;
	tip->hide();
	contentNode->removeAllChildren();
	refreshSize();

	const float marginV = (btnSize - ToolbarTheme::dragIconSize) * 0.5f;
	dragHandle = contentNode->makeChild<Ling::Button>();
	dragHandle->setId(L"drag");
	dragHandle->setText(Icon::DragHandle);
	dragHandle->setFontFamily(Icon::Family);
	dragHandle->setFontSize(Icon::Size);
	dragHandle->setColor(ToolbarTheme::dragHandleColor);
	dragHandle->setHoverColor(ToolbarTheme::dragHandleColor);
	dragHandle->setHoverBg(0);
	dragHandle->setSize(ToolbarTheme::dragIconSize, ToolbarTheme::dragIconSize);
	// 左 pad 在 contentNode；右距扣掉穿透按钮的 hoverInset，避免看起来偏宽
	dragHandle->setMargin(0.f, marginV, dragMarginRight(), marginV);
	dragHandle->setFlexGrow(0.f);
	dragHandle->setFlexShrink(0.f);
	dragHandle->setAlignItems(Ling::Align::Center);
	dragHandle->setJustifyContent(Ling::Justify::Center);
	tip->bind(dragHandle, L"拖动");

	btnPierce = makeIconBtn(Icon::Pierce, L"pierce");
	btnPierce->onClick.add([this](Ling::Button*) { togglePierce(); });
	tip->bind(btnPierce, Lang::get(L"video.mouseThrough"));
	applyToggleStyle(btnPierce, pierceOn);

	makeSpliter();

	btnPlay = makeIconBtn(Icon::Play, L"play");
	btnPlay->onClick.add([this](Ling::Button*) { togglePlayPause(); });
	tip->bind(btnPlay, Lang::get(L"video.startRecord"));

	timerLabel = contentNode->makeChild<Ling::Label>();
	timerLabel->setWidth(timerW);
	timerLabel->setHeightPercent(100.f);
	timerLabel->setAlignItems(Ling::Align::Center);
	timerLabel->setJustifyContent(Ling::Justify::Center);
	timerLabel->setColor(Icon::ColorNormal);
	updateTimerText();

	btnAudio = makeIconBtn(Icon::Audio, L"audio");
	btnAudio->onClick.add([this](Ling::Button*) {
		if (selectIndex != 0) return;
		selectSpeaker = !selectSpeaker;
		updateAudioButtons();
	});
	tip->bind(btnAudio, Lang::get(L"video.recordSystem"));

	btnMic = makeIconBtn(Icon::MicOn, L"mic");
	btnMic->onClick.add([this](Ling::Button*) {
		if (selectIndex != 0) return;
		selectMic = !selectMic;
		updateAudioButtons();
	});
	tip->bind(btnMic, Lang::get(L"video.recordMic"));
	updateAudioButtons();

	struct AnnotItem { const wchar_t* id; const wchar_t* icon; const wchar_t* tip; };
	const AnnotItem annots[] = {
		{ L"rect", Icon::Rect, L"tool.rect" },
		{ L"arrow", Icon::Arrow, L"tool.arrow" },
		{ L"pen", Icon::Pen, L"tool.pen" },
		{ L"text", Icon::Text, L"tool.text" },
		{ L"number", Icon::Number, L"tool.number" },
		{ L"mosaic", Icon::Mosaic, L"tool.mosaic" },
		{ L"eraser", Icon::Eraser, L"tool.eraser" },
	};
	for (const auto& a : annots) {
		auto* btn = makeIconBtn(a.icon, a.id);
		tip->bind(btn, Lang::get(a.tip));
		btn->onClick.add([this, id = std::wstring(a.id)](Ling::Button*) { onAnnotClick(id); });
		annotBtns.push_back(btn);
	}

	makeSpliter();

	btnUndo = makeIconBtn(Icon::Undo, L"undo");
	btnUndo->onClick.add([this](Ling::Button*) {
		if (win->history && win->history->canUndo())
			win->history->undo();
		updateUndoEnabled();
	});
	tip->bind(btnUndo, Lang::get(L"tool.undo"));

	makeSpliter();

	btnGif = makeIconBtn(Icon::Gif, L"gif");
	btnGif->onClick.add([this](Ling::Button*) { onGifClick(); });
	tip->bind(btnGif, Lang::get(L"video.outputGif"));
	applyToggleStyle(btnGif, selectIndex == 1);

	auto btnSave = makeIconBtn(Icon::Save, L"save");
	btnSave->onClick.add([this](Ling::Button*) {
		if (isRecording) saveFile();
	});
	tip->bind(btnSave, Lang::get(L"video.stopFile"));

	auto btnCancel = makeIconBtn(Icon::Cancel, L"cancel");
	btnCancel->setColor(Icon::ColorCancel);
	btnCancel->setHoverColor(Icon::ColorCancel);
	btnCancel->onClick.add([this](Ling::Button*) {
		if (isRecording) finishRecord(false);
		else win->close();
	});
	tip->bind(btnCancel, Lang::get(isRecording ? L"video.stopExit" : L"video.exit"));

	auto btnDone = makeIconBtn(Icon::Done, L"done");
	btnDone->setColor(Icon::ColorDone);
	btnDone->setHoverColor(Icon::ColorDone);
	btnDone->onClick.add([this](Ling::Button*) {
		if (isRecording) finishRecord(true);
	});
	tip->bind(btnDone, Lang::get(L"video.stopClipboard"));
}

void ToolVideo::onDragDown(POINT pos, bool isRight)
{
	if (isRight) return;
	if (!dragHandle || !dragHandle->isPosIn(pos)) return;
	draggingBar = true;
	POINT scr{};
	GetCursorPos(&scr);
	dragMouseScreen = scr;
	dragWinX = x;
	dragWinY = y;
	SetCapture(hwnd);
}

void ToolVideo::onDragMove(POINT)
{
	if (!draggingBar) return;
	POINT scr{};
	GetCursorPos(&scr);
	setPosition(dragWinX + scr.x - dragMouseScreen.x, dragWinY + scr.y - dragMouseScreen.y);
}

void ToolVideo::onDragUp(POINT, bool)
{
	if (!draggingBar) return;
	draggingBar = false;
	ReleaseCapture();
	win->markMainToolUserPlaced();
}

void ToolVideo::onGifClick()
{
	if (isRecording) return; // 开录后锁定格式
	selectIndex = (selectIndex == 1) ? 0 : 1;
	applyToggleStyle(btnGif, selectIndex == 1);
	// GIF 无音轨：音频钮灰显；取消 GIF 后恢复进入前的开关状态（selectSpeaker/Mic 未改）
	updateAudioButtons();
	updateTimerText();
}

void ToolVideo::updateAudioButtons()
{
	auto applyDisabled = [](Ling::Button* btn) {
		if (!btn) return;
		btn->setBg(0);
		btn->setHoverBg(0);
		btn->setColor(Icon::ColorDisabled);
		btn->setHoverColor(Icon::ColorDisabled);
	};
	if (selectIndex == 1) {
		applyDisabled(btnAudio);
		applyDisabled(btnMic);
		return;
	}
	applyToggleStyle(btnAudio, selectSpeaker);
	applyToggleStyle(btnMic, selectMic);
}

void ToolVideo::togglePlayPause()
{
	if (!isRecording) {
		startRecord();
		return;
	}
	setPaused(!isPaused);
}

void ToolVideo::togglePierce()
{
	win->setMouseTransparent(!win->mouseTransparent());
	if (win->mouseTransparent()) cancelAnnotSelect();
}

void ToolVideo::syncPierce(bool on)
{
	pierceOn = on;
	applyToggleStyle(btnPierce, pierceOn);
}

void ToolVideo::startRecord()
{
	isRecording = true;
	isPaused = false;
	totalSeconds = 0;
	if (btnPlay) {
		btnPlay->setText(Icon::Pause);
		tip->bind(btnPlay, Lang::get(L"video.pauseRecord"));
	}
	setTimer(1000, tickTimerId);
	updateTimerText();
	cancelAnnotSelect();
	if (selectIndex == 0) win->startMp4(selectSpeaker, selectMic);
	else win->startGif();
}

void ToolVideo::setPaused(bool on)
{
	if (!isRecording || isPaused == on) return;
	isPaused = on;
	win->setRecordPaused(on);
	if (btnPlay) {
		btnPlay->setText(on ? Icon::Play : Icon::Pause);
		tip->bind(btnPlay, Lang::get(on ? L"video.resumeRecord" : L"video.pauseRecord"));
	}
	if (on) killTimer(tickTimerId);
	else setTimer(1000, tickTimerId);
}

void ToolVideo::applyAnnotSelect(const std::wstring& id)
{
	curAnnotId_ = id;
	for (auto* b : annotBtns) {
		if (!b) continue;
		applyToggleStyle(b, b->id == id);
	}
}

void ToolVideo::cancelAnnotSelect()
{
	curAnnotId_.clear();
	for (auto* b : annotBtns) applyToggleStyle(b, false);
	win->clearVideoAnnotTool();
}

void ToolVideo::onAnnotClick(const std::wstring& id)
{
	if (!curAnnotId_.empty() && curAnnotId_ == id) {
		if (win->hasSelectedAnnot()) {
			win->startVideoAnnotate(id);
			win->presentSelectedAnnotStyle();
			return;
		}
		cancelAnnotSelect();
		return;
	}
	applyAnnotSelect(id);
	win->startVideoAnnotate(id);
	win->presentSelectedAnnotStyle();
}

void ToolVideo::updateUndoEnabled()
{
	if (!btnUndo) return;
	const bool ok = win->history && win->history->canUndo();
	if (ok) {
		btnUndo->setColor(Icon::ColorNormal);
		btnUndo->setHoverColor(Icon::ColorNormal);
		btnUndo->setHoverBg(ToolbarTheme::hoverBg);
	}
	else {
		btnUndo->setColor(Icon::ColorDisabled);
		btnUndo->setHoverColor(Icon::ColorDisabled);
		btnUndo->setHoverBg(0);
	}
}

void ToolVideo::updateTimerText()
{
	if (!timerLabel) return;
	if (!isRecording) {
		// 未开录：只显示最大时长（如 120.00 / 6.00），缩短栏宽
		const int maxMinutes = (selectIndex == 1) ? 6 : 120;
		timerLabel->setText(std::format(L"{}.00", maxMinutes));
		return;
	}
	timerLabel->setText(std::format(L"{:02d}.{:02d}", totalSeconds / 60, totalSeconds % 60));
}

void ToolVideo::onTimerCB(UINT id)
{
	if (id != tickTimerId) return;
	if (isPaused) return;
	totalSeconds += 1;
	updateTimerText();
	const int maxSeconds = ((selectIndex == 1) ? 6 : 120) * 60;
	if (totalSeconds >= maxSeconds) saveFile();
}

void ToolVideo::saveFile()
{
	if (!isRecording) return;
	hide();
	killTimer(tickTimerId);
	auto srcPath = win->stopRecord();
	if (srcPath.empty()) {
		win->close();
		return;
	}
	auto tarPath = Util::getSaveFilePath(nullptr, selectIndex == 1 ? L"gif" : L"mp4");
	if (!tarPath.empty()) CopyFile(srcPath.data(), tarPath.data(), false);
	DeleteFile(srcPath.data());
	win->close();
}

bool ToolVideo::onSaveKey(bool toClipboard)
{
	if (!isRecording) return false;
	if (toClipboard) finishRecord(true);
	else saveFile();
	return true;
}

void ToolVideo::finishRecord(bool toClipboard)
{
	hide();
	killTimer(tickTimerId);
	auto srcPath = win->stopRecord();
	if (toClipboard) {
		if (!srcPath.empty()) Util::addFileToClipboard(srcPath);
	}
	else {
		if (!srcPath.empty()) DeleteFile(srcPath.data());
	}
	win->close();
}
