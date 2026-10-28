#include "pch.h"
#include "../Win/WinCap.h"
#include "../History.h"
#include "../Util.h"
#include "../Lang.h"
#include "../Tip.h"
#include "ToolVideo.h"
#include <climits>
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

ToolVideo::ToolVideo(WinCap* win) : ToolbarWindow(), win(win)
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
	body->setBg(0);
	initChrome(ToolbarTheme::paddingLeft, contentPadRight(),
		ToolbarTheme::shadowPad, ToolbarTheme::shadowPad, Ling::FlexDirection::Row);
	tip->excludeFromCapture();
	buildBar();
	onMouseDown.add([this](POINT pos, bool isRight) { onDragDown(pos, isRight); });
	onMouseMove.add([this](POINT pos) { onDragMove(pos); });
	onMouseUp.add([this](POINT pos, bool isRight) { onDragUp(pos, isRight); });
	show();
	updateUndoEnabled();
}

void ToolVideo::drawChrome(ID2D1DeviceContext* ctx)
{
	if (!brushBg) return;
	const float pad = ToolbarTheme::shadowPad * dpi;
	const float radius = toolbarRadius * dpi;
	const float borderW = ToolbarTheme::borderWidth * dpi;
	const D2D1_RECT_F bar{ pad, pad, w - pad, h - pad };
	ToolbarChrome::paintRoundBar(ctx, bar, radius, brushBg.Get(), borderW);
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
	// 拖拽手柄不给悬停提示：它只是移动工具条

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
		btnPierce = makeIconBtn(Icon::Cursor, L"pierce");   // 与截图栏「显示光标」同一图标
		btnPierce->onClick.add([this](Ling::Button*) { togglePierce(); });
		tip->bind(btnPierce, Lang::get(L"video.mouseThrough"));
		applyToggleStyle(btnPierce, pierceOn);
	}
	else if (id == L"play") {
		// 录制中这颗按钮显示"暂停"，并保持选中态（绿）—— 让用户一眼看出还在录
		btnPlay = makeIconBtn(isRecording ? (isPaused ? Icon::Play : Icon::Pause) : Icon::Play, L"play");
		btnPlay->onClick.add([this](Ling::Button*) { togglePlayPause(); });
		tip->bind(btnPlay, Lang::get(isRecording
			? (isPaused ? L"video.resumeRecord" : L"video.pauseRecord")
			: L"video.startRecord"));
		applyToggleStyle(btnPlay, isRecording);
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
		// 各工具栏的 × / ✓ 文案统一：× = 关闭，✓ = 完成（与截图栏、长截图一致）
		tip->bind(btnCancel, Lang::get(L"tool.close"));
	}
	else if (id == L"done") {
		auto* btnDone = makeIconBtn(Icon::Done, L"done");
		btnDone->setColor(Icon::ColorDone);
		btnDone->setHoverColor(Icon::ColorDone);
		btnDone->onClick.add([this](Ling::Button*) {
			if (isRecording) finishRecord(true);
		});
		// 各工具栏的 × / ✓ 文案统一：✓ = 完成（这里会复制到剪贴板并结束录制）
		tip->bind(btnDone, Lang::get(L"tool.clipboard"));
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
	killTimer(peekTimerId);
	draggingBar = true;
	dragMoved_ = false;
	POINT scr{};
	GetCursorPos(&scr);
	dragMouseScreen = scr;
	dragWinX = x;
	dragWinY = y;
	SetCapture(hwnd);
}

void ToolVideo::onDragMove(POINT pos)
{
	// 贴边隐藏的进出：离开已滑出的工具栏 → 延时缩回；移到缩进后露出的边上 → 滑出
	if (peekEdge_ != PeekEdge::None && !draggingBar) {
		if (pos.x == INT_MAX && pos.y == INT_MAX) {
			if (!peekHidden_) schedulePeekCollapse();
		}
		else {
			killTimer(peekTimerId);   // 又回到工具栏上了：取消待缩回
			if (peekHidden_) peekReveal();
		}
	}
	if (!draggingBar) return;
	POINT scr{};
	GetCursorPos(&scr);
	const int nx = dragWinX + scr.x - dragMouseScreen.x;
	const int ny = dragWinY + scr.y - dragMouseScreen.y;
	if (nx != x || ny != y) dragMoved_ = true;
	setPosition(nx, ny);
}

void ToolVideo::onDragUp(POINT, bool)
{
	if (!draggingBar) return;
	draggingBar = false;
	ReleaseCapture();
	win->markMainToolUserPlaced();
	peekEdge_ = PeekEdge::None;
	peekHidden_ = false;
	// 「贴边隐藏」只认**手动拖动**：松手时工具栏边缘贴住屏幕的上/下沿（8px 内）才缩进。
	// 判定用整块屏幕 rcMonitor —— 工作区会被任务栏挤出一条缝，容易"看着贴边其实差几十像素"。
	// 只要拖离边缘，就清掉贴边状态，本次录制内不再自动隐藏（拖回边缘可重新触发）。
	if (Setting::get()->getRecordToolbarPeek() && dragMoved_) {
		MONITORINFO mi{ sizeof(MONITORINFO) };
		if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
			const int th = (int)(8.f * dpi + 0.5f);
			if (y <= mi.rcMonitor.top + th) peekEdge_ = PeekEdge::Top;
			else if (y + (int)h >= mi.rcMonitor.bottom - th) peekEdge_ = PeekEdge::Bottom;
		}
	}
	killTimer(peekTimerId);
	dragMoved_ = false;
	if (peekEdge_ != PeekEdge::None) peekCollapse();
}

// 缩进边缘：藏到屏幕之外，只露出 peekStripPx 高的一条，鼠标移到这条上再滑出
void ToolVideo::peekCollapse()
{
	if (peekEdge_ == PeekEdge::None || !hwnd) return;
	MONITORINFO mi{ sizeof(MONITORINFO) };
	if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return;
	// ★ 用 rcMonitor（整块屏幕）而不是 rcWork（工作区）：底部要藏到屏幕下沿之外，
	//   只留一条压在任务栏上沿；用 rcWork 会在"工作区底"和"屏幕底"之间留出任务栏那段空档，
	//   光标正好落在空档里 → 露头/缩回反复弹。
	const RECT area = mi.rcMonitor;
	const int strip = (int)(peekStripPx * dpi + 0.5f);
	int nx = x;
	const int maxX = area.right - (int)w;
	if (nx < area.left) nx = area.left;
	if (nx > maxX) nx = maxX;
	const int ny = (peekEdge_ == PeekEdge::Top)
		? area.top - ((int)h - strip)
		: area.bottom - strip;
	peekHidden_ = true;
	if (x != nx || y != ny) setPosition(nx, ny);
}

// 滑出：贴着屏幕边缘（rcMonitor），光标从露出的一条进来后仍落在工具栏内，不会来回弹
void ToolVideo::peekReveal()
{
	if (peekEdge_ == PeekEdge::None || !hwnd) return;
	killTimer(peekTimerId);
	MONITORINFO mi{ sizeof(MONITORINFO) };
	if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return;
	const RECT area = mi.rcMonitor;
	int nx = x;
	const int maxX = area.right - (int)w;
	if (nx < area.left) nx = area.left;
	if (nx > maxX) nx = maxX;
	const int ny = (peekEdge_ == PeekEdge::Top)
		? area.top
		: area.bottom - (int)h;
	peekHidden_ = false;
	if (x != nx || y != ny) setPosition(nx, ny);
}

void ToolVideo::schedulePeekCollapse()
{
	if (peekEdge_ == PeekEdge::None || peekHidden_ || draggingBar) return;
	// 设置里中途关掉「贴边隐藏」：不再自动缩回（下次手动拖动时才清掉 peekEdge_）
	if (!Setting::get()->getRecordToolbarPeek()) return;
	setTimer(peekHideDelayMs, peekTimerId);
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
	trimmedAfterStart_ = false;
	if (btnPlay) {
		btnPlay->setText(Icon::Pause);
		applyToggleStyle(btnPlay, true);   // 选中态 = 正在录制
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
		// 未开录：动图显示 10 分钟上限；视频不限时，从 00.00 起（不再显示上限数字）。
		timerLabel->setText(selectIndex == 1 ? L"10.00" : L"00.00");
		return;
	}
	timerLabel->setText(wfmt(L"%02d.%02d", (int)(totalSeconds / 60), (int)(totalSeconds % 60)));
}

void ToolVideo::onTimerCB(UINT id)
{
	if (id == peekTimerId) {
		killTimer(peekTimerId);
		if (peekEdge_ != PeekEdge::None && !peekHidden_ && !draggingBar) peekCollapse();
		return;
	}
	if (id != tickTimerId) return;
	if (isPaused) return;
	totalSeconds += 1;
	updateTimerText();
	// 开录 1 秒后修剪一次工作集：此时采集/编码缓冲已分配、开场整屏截图已释放，
	// 把堆里已经空闲的页还给系统（否则那几十 MB 会一直挂在任务管理器里）。
	if (!trimmedAfterStart_ && totalSeconds >= 1) {
		trimmedAfterStart_ = true;
		SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
	}
	// 视频录制不限时；动图（GIF/WebP/APNG）仍有 10 分钟上限，避免文件无限膨胀
	if (selectIndex == 1 && totalSeconds >= 10 * 60) saveFile();
}

void ToolVideo::saveFile()
{
	if (!isRecording) return;
	hide();
	killTimer(tickTimerId);
	// 先把屏幕上的选框/遮罩收掉：stopRecord() 要等编码收尾，那几百毫秒不该让用户干等
	win->hideRecordOverlay();
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
	if (!tarPath.empty() && CopyFile(srcPath.data(), tarPath.data(), false))
		addRecordHistory(tarPath, totalSeconds);
	DeleteFile(srcPath.data());
	win->close();
}

void ToolVideo::addRecordHistory(const std::wstring& path, int durationSec)
{
	if (path.empty()) return;
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
	Setting::get()->addHistoryMedia(path, vw, vh, L"record", durationSec, cover);
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
	// 同 saveFile：先收屏幕上的选框/遮罩，再等编码收尾
	win->hideRecordOverlay();
	auto srcPath = win->stopRecord();
	if (toClipboard) {
		if (!srcPath.empty()) {
			// 剪贴板里要贴的是落在「录屏文件路径」里的成片，不是临时目录里的半成品
			auto tarPath = Util::commitRecordedFile(srcPath);
			Util::addFileToClipboard(tarPath.empty() ? srcPath : tarPath);
			// 登记进历史，否则历史页只能按目录扫描把它当「其他」里的用户文件
			if (!tarPath.empty()) addRecordHistory(tarPath, totalSeconds);
		}
	}
	else {
		if (!srcPath.empty()) DeleteFile(srcPath.data());
	}
	win->close();
}
