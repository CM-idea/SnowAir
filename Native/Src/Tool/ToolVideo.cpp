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
#include "ToolbarStore.h"
#include "../Setting.h"

namespace {
	// 录屏栏运行时 id ↔ 设置页（ToolbarStore kind=1）id：设置页里穿透/录制/音频/麦克风/
	// 动图叫 mouse-through / video-play / video-audio / video-mic / video-gif，运行时是简写。
	std::wstring videoRuntimeId(const std::wstring& sid)
	{
		if (sid.rfind(L"split", 0) == 0) return L"|";
		if (sid == L"mouse-through") return L"pierce";
		if (sid == L"video-play" || sid == L"video-pause") return L"play";
		if (sid == L"video-audio") return L"audio";
		if (sid == L"video-mic") return L"mic";
		if (sid == L"video-gif") return L"gif";
		if (sid == L"confirm") return L"done";
		if (sid == L"serial-number") return L"number";
		return sid;
	}
	// 内置默认顺序（与旧版手写 buildBar 一字不差）
	const std::vector<std::wstring>& defaultVideoSlots()
	{
		static const std::vector<std::wstring> k{
			L"pierce", L"|", L"play", L"record-time", L"audio", L"mic",
			L"rect", L"arrow", L"pen", L"text", L"number", L"mosaic", L"eraser", L"|",
			L"undo", L"|", L"gif", L"save", L"cancel", L"done"
		};
		return k;
	}
}

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
	// 按当前槽位表累加：图标格 + 时长格 + 分隔符（默认布局下与旧版硬编码的 16 图标 + 时长 + 3 分隔一致）
	float icons = 0.f, timer = 0.f, splits = 0.f;
	for (const auto& id : resolveSlots()) {
		if (id == L"|") splits += 1.f;
		else if (id == L"record-time") timer += timerW;
		else icons += btnSize;
	}
	const float drag = ToolbarTheme::paddingLeft + ToolbarTheme::dragIconSize + dragMarginRight();
	return drag + icons + timer + sep * splits + contentPadRight();
}

float ToolVideo::getBtnCenterX() const
{
	// 物理像素：相对工具条左缘到当前标注按钮中心；未选标注时对准播放钮
	const float sep = (spliterW + (ToolbarTheme::splitterGap - hoverInset) * 2.f) * dpi;
	float x = (ToolbarTheme::shadowPad + ToolbarTheme::paddingLeft
		+ ToolbarTheme::dragIconSize + dragMarginRight()) * dpi;
	float playCenter = -1.f;
	for (const auto& id : resolveSlots()) {
		if (id == L"|") { x += sep; continue; }
		const float slotW = (id == L"record-time") ? timerW * dpi : btnSize * dpi;
		if (id == L"play") playCenter = x + slotW * 0.5f;
		if (id == curAnnotId_) return x + slotW * 0.5f;
		x += slotW;
	}
	return playCenter >= 0.f ? playCenter : x;
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

	for (const auto& id : resolveSlots()) {
		if (id == L"|") makeSpliter();
		else makeSlot(id);
	}
	updateAudioButtons();
}

// 运行时槽位顺序：用户存过「录屏工具栏外观」就按它排，否则用内置默认。
std::vector<std::wstring> ToolVideo::resolveSlots() const
{
	const auto& def = defaultVideoSlots();
	if (!ToolbarStore::hasSavedLayout(1)) return def;
	auto st = ToolbarStore::loadLayout(1);
	std::vector<std::wstring> out;
	for (const auto& sid : st.active) {
		std::wstring rid = videoRuntimeId(sid);
		if (rid == L"|") {
			// 相邻分隔符并成一个（运行时不做折叠，得在这儿收）
			if (!out.empty() && out.back() != L"|") out.push_back(L"|");
			continue;
		}
		// 只收运行时认识的槽位（设置页里可能还留着别的类型的历史项）
		bool known = false;
		for (const auto& d : def) if (d == rid) { known = true; break; }
		if (!known) continue;
		bool dup = false;
		for (const auto& o : out) if (o == rid) { dup = true; break; }
		if (dup) continue;
		out.push_back(rid);
	}
	while (!out.empty() && out.back() == L"|") out.pop_back();
	return out.empty() ? def : out;
}

// 建一个槽位控件。锁定项（录制/时长/取消/完成）设置页不允许删，所以这里必有；
// 其余槽位被用户移到隐藏区时就不会被调用，对应的成员指针保持 nullptr，
// 相关函数都已做空判断。
void ToolVideo::makeSlot(const std::wstring& id)
{
	if (id == L"pierce") {
		btnPierce = makeIconBtn(Icon::Pierce, L"pierce");
		btnPierce->onClick.add([this](Ling::Button*) { togglePierce(); });
		tip->bind(btnPierce, Lang::get(L"video.mouseThrough"));
		applyToggleStyle(btnPierce, pierceOn);
	}
	else if (id == L"play") {
		btnPlay = makeIconBtn(Icon::Play, L"play");
		btnPlay->onClick.add([this](Ling::Button*) { togglePlayPause(); });
		tip->bind(btnPlay, Lang::get(isRecording ? L"video.pauseRecord" : L"video.startRecord"));
	}
	else if (id == L"record-time") {
		timerLabel = contentNode->makeChild<Ling::Label>();
		timerLabel->setWidth(timerW);
		timerLabel->setHeightPercent(100.f);
		timerLabel->setAlignItems(Ling::Align::Center);
		timerLabel->setJustifyContent(Ling::Justify::Center);
		timerLabel->setColor(Icon::ColorNormal);
		updateTimerText();
	}
	else if (id == L"audio") {
		btnAudio = makeIconBtn(Icon::Audio, L"audio");
		btnAudio->onClick.add([this](Ling::Button*) { cmdToggleSystemAudio(); });
		tip->bind(btnAudio, Lang::get(L"video.recordSystem"));
	}
	else if (id == L"mic") {
		btnMic = makeIconBtn(Icon::MicOn, L"mic");
		btnMic->onClick.add([this](Ling::Button*) { cmdToggleMic(); });
		tip->bind(btnMic, Lang::get(L"video.recordMic"));
	}
	else if (id == L"undo") {
		btnUndo = makeIconBtn(Icon::Undo, L"undo");
		btnUndo->onClick.add([this](Ling::Button*) {
			if (win->history && win->history->canUndo())
				win->history->undo();
			updateUndoEnabled();
		});
		tip->bind(btnUndo, Lang::get(L"tool.undo"));
	}
	else if (id == L"gif") {
		btnGif = makeIconBtn(Icon::Gif, L"gif");
		btnGif->onClick.add([this](Ling::Button*) { onGifClick(); });
		tip->bind(btnGif, Lang::get(L"video.outputGif"));
		applyToggleStyle(btnGif, selectIndex == 1);
	}
	else if (id == L"save") {
		auto* btnSave = makeIconBtn(Icon::Save, L"save");
		btnSave->onClick.add([this](Ling::Button*) {
			if (isRecording) saveFile();
		});
		tip->bind(btnSave, Lang::get(L"video.stopFile"));
	}
	else if (id == L"cancel") {
		auto* btnCancel = makeIconBtn(Icon::Cancel, L"cancel");
		btnCancel->setColor(Icon::ColorCancel);
		btnCancel->setHoverColor(Icon::ColorCancel);
		btnCancel->onClick.add([this](Ling::Button*) {
			if (isRecording) finishRecord(false);
			else win->close();
		});
		tip->bind(btnCancel, Lang::get(isRecording ? L"video.stopExit" : L"video.exit"));
	}
	else if (id == L"done") {
		auto* btnDone = makeIconBtn(Icon::Done, L"done");
		btnDone->setColor(Icon::ColorDone);
		btnDone->setHoverColor(Icon::ColorDone);
		btnDone->onClick.add([this](Ling::Button*) {
			if (isRecording) finishRecord(true);
		});
		tip->bind(btnDone, Lang::get(L"video.stopClipboard"));
	}
	else {
		// 标注类：矩形 / 箭头 / 画笔 / 文字 / 序号 / 马赛克 / 橡皮擦
		static const struct { const wchar_t* id; const wchar_t* icon; const wchar_t* tip; } kAnnots[] = {
			{ L"rect", Icon::Rect, L"tool.rect" },
			{ L"arrow", Icon::Arrow, L"tool.arrow" },
			{ L"pen", Icon::Pen, L"tool.pen" },
			{ L"text", Icon::Text, L"tool.text" },
			{ L"number", Icon::Number, L"tool.number" },
			{ L"mosaic", Icon::Mosaic, L"tool.mosaic" },
			{ L"eraser", Icon::Eraser, L"tool.eraser" },
		};
		for (const auto& a : kAnnots) {
			if (id != a.id) continue;
			auto* btn = makeIconBtn(a.icon, a.id);
			tip->bind(btn, Lang::get(a.tip));
			btn->onClick.add([this, id = std::wstring(a.id)](Ling::Button*) { onAnnotClick(id); });
			annotBtns.push_back(btn);
			break;
		}
	}
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

void ToolVideo::cmdToggleSystemAudio()
{
	if (selectIndex != 0) return;   // 只有 mp4 分轨，GIF 没有音轨
	selectSpeaker = !selectSpeaker;
	updateAudioButtons();
}

void ToolVideo::cmdToggleMic()
{
	if (selectIndex != 0) return;
	selectMic = !selectMic;
	updateAudioButtons();
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

// 「系统按键」用：绘图工具命令键（与点击按钮同一实现，但不做"再点一次取消"的切换）
void ToolVideo::cmdAnnotTool(const std::wstring& id)
{
	if (id.empty()) return;
	applyAnnotSelect(id);
	win->startVideoAnnotate(id);
	win->presentSelectedAnnotStyle();
}

// 「系统按键」用：工具循环（Tab）—— 在可见的绘图工具按钮之间循环切换一个
void ToolVideo::cycleAnnotTool()
{
	if (annotBtns.empty()) return;
	size_t next = 0;
	for (size_t i = 0; i < annotBtns.size(); i++) {
		if (annotBtns[i] && annotBtns[i]->id == curAnnotId_) { next = (i + 1) % annotBtns.size(); break; }
	}
	if (annotBtns[next]) cmdAnnotTool(annotBtns[next]->id);
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
	// 「功能设置 → 输出 → 视频」：文件名默认给「视频文件名格式」的末段，目录用「录屏文件路径」。
	// 扩展名按实际临时文件取（录屏 mp4 / 动图 gif 或 apng→png）
	auto* st = Setting::get();
	std::wstring ext = std::filesystem::path(srcPath).extension().wstring();
	if (!ext.empty() && ext[0] == L'.') ext.erase(0, 1);
	if (ext.empty()) ext = selectIndex == 1 ? L"gif" : L"mp4";
	std::wstring defName = Util::previewFileName(
		st->getVideoRecordFormat(), Setting::defaultVideoRecordFormat(), L"");
	if (auto slash = defName.find_last_of(L'/'); slash != std::wstring::npos) defName = defName.substr(slash + 1);
	auto tarPath = Util::getSaveFilePath(nullptr, ext, defName, st->getRecordDir());
	if (!tarPath.empty() && CopyFile(srcPath.data(), tarPath.data(), false)) {
		// 录屏入历史：视频本身不复制（历史只记路径），封面用收工这一刻的画面 ——
		// 此时工具条已隐藏，画面就是用户看到的样子；选区取不到就只记条目、不带封面。
		int vw = 0, vh = 0;
		std::vector<BYTE> cover;
		if (win && win->cutMask) {
			// maskRect 是客户坐标；WinCap 铺满虚拟屏，加 win->x/y 即屏幕坐标
			const auto& sel = win->cutMask->maskRect;   // D2D1_RECT_F，客户坐标
			const int cx = win->x + (int)sel.left;
			const int cy = win->y + (int)sel.top;
			const int cw2 = (int)(sel.right - sel.left);
			const int ch2 = (int)(sel.bottom - sel.top);
			if (cw2 > 0 && ch2 > 0) {
				cover = Util::captureScreen(cx, cy, cw2, ch2);
				if (cover.size() == (size_t)cw2 * (size_t)ch2 * 4) { vw = cw2; vh = ch2; }
				else cover.clear();
			}
		}
		st->addHistoryMedia(tarPath, vw, vh, L"record", cover);
	}
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
