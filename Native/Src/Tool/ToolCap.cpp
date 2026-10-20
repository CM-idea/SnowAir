#include "pch.h"
#include "../Win/WinCap.h"
#include "../Win/WinPin.h"
#include "../History.h"
#include "../Lang.h"
#include "../Setting.h"
#include "../Tip.h"
#include "ToolPicker.h"
#include "ToolSub.h"
#include "ToolCap.h"
#include "ToolbarChrome.h"
#include "ToolbarTheme.h"
#include "ToolbarStore.h"
#include "../Win/AnnotHost.h"
#include "../Win/PinMenu.h"
#include "../Win/SettingWidgets.h"
#include "../CommandKeys.h"

ToolCap::ToolCap(WinCap* capWin) : Ling::WinBase(), winCap(capWin)
{
	dpi = capWin->dpi;
	applyStoredLayout(0);   // 套用「截图工具栏外观」里保存的槽位顺序（没存过则为内置默认）
	refreshSize();
	onKeyDown.add([this](UINT key) {
		if (winPin) winPin->forwardKey(key);
		else if (winCap) winCap->forwardKey(key);
	});
	onDpiChanged.add([this]() { dpiChanged = true; });
	onSizeChanged.add([this]() {
		if (!dpiChanged) return;
		dpiChanged = false;
		refreshSize();
		if (winPin) winPin->layoutTools();
		else if (winCap) winCap->layoutTool(this);
	});
}

LRESULT ToolCap::onHitTest(const POINT screenPos)
{
	// 工具栏盖住选区边/角时，把命中穿透给 WinCap，悬停才能立刻变成 ↔
	if (winCap && winCap->hwnd && winCap->cutMask && winCap->cutMask->hasRect()) {
		POINT client = screenPos;
		ScreenToClient(winCap->hwnd, &client);
		const auto hit = winCap->cutMask->hitTest(client);
		if (hit != MaskHit::None && hit != MaskHit::Inside)
			return HTTRANSPARENT;
	}
	return HTCLIENT;
}

void ToolCap::hideHoverTip()
{
	if (tip) tip->hide();
	if (picker && picker->isOpen()) picker->hidePicker();
}

ToolCap::ToolCap(WinPin* pin) : Ling::WinBase(), winPin(pin)
{
	dpi = pin->dpi;
	applyStoredLayout(3);   // 套用「贴图工具栏外观」里保存的槽位顺序（没存过则为内置默认）
	refreshSize();
	onKeyDown.add([this](UINT key) { this->winPin->forwardKey(key); });
	onDpiChanged.add([this]() { dpiChanged = true; });
	onSizeChanged.add([this]() {
		if (!dpiChanged) return;
		dpiChanged = false;
		refreshSize();
		this->winPin->layoutTools();
	});
	// 钉图：窗在 setEditing(true) 里才建；此处不 createNativeWindow，避免未定位就出现在 (0,0)
}

float ToolCap::splitterSlotW() const
{
	return spliterW + (ToolbarTheme::splitterGap - hoverInset) * 2.f;
}

void ToolCap::refreshSize()
{
	// 内容宽：左 pad + 拖拽 + 拖拽右距 + 工具格 + 右 pad（右距已扣掉末按钮 margin/字形边）
	float logicW{ ToolbarTheme::paddingLeft + ToolbarTheme::dragIconSize + dragMarginRight() };
	const auto vis = visibleSlots();
	for (size_t i = 0; i < btnIds.size(); i++) {
		if (!vis[i]) continue;
		logicW += (btnIds[i] == L"|") ? splitterSlotW() : btnSize;
	}
	logicW += contentPadRight();
	setSize(logicW + ToolbarTheme::shadowPad * 2.f, btnSize + ToolbarTheme::shadowPad * 2.f);
}

float ToolCap::iconSideInset() const
{
	return (btnSize - hoverInset * 2.f - Icon::Size) * 0.5f;
}

float ToolCap::dragMarginRight() const
{
	// 拖拽本体 → 首个工具本体 = dragGap；扣掉首按钮左 margin 与字形侧边
	return ToolbarTheme::dragGap - hoverInset - iconSideInset();
}

float ToolCap::contentPadRight() const
{
	// 完成本体 → 栏右缘 = paddingRight
	return ToolbarTheme::paddingRight - hoverInset - iconSideInset();
}

float ToolCap::contentLeadWidth() const
{
	return ToolbarTheme::paddingLeft + ToolbarTheme::dragIconSize + dragMarginRight();
}

ToolCap::~ToolCap()
{
	// WinBase 析构不销毁 HWND：不 close 就析构（例如直接重新 make_unique 覆盖）会留下"幽灵窗"
	if (hwnd) { close(); hwnd = nullptr; }
}

bool ToolCap::isAnnotationTool(const std::wstring& id)
{
	return id == L"rect" || id == L"arrow" || id == L"pen" || id == L"number"
		|| id == L"text" || id == L"mosaic" || id == L"patina"
		|| id == L"watermark" || id == L"highlight" || id == L"eraser";
}

bool ToolCap::isCapFeature(const std::wstring& id)
{
	return id == L"long" || id == L"extra" || id == L"ocr" || id == L"translate";
}

bool ToolCap::isPinHidden(const std::wstring& id)
{
	return isCapFeature(id) || id == L"pin" || id == L"show-cursor";
}

bool ToolCap::isPinOnly(const std::wstring& id)
{
	return id == L"border" || id == L"opacity";
}

std::vector<bool> ToolCap::visibleSlots() const
{
	std::vector<bool> keep(btnIds.size(), true);
	for (size_t i = 0; i < btnIds.size(); i++) {
		if (btnIds[i] == L"|") continue;
		if (winPin) {
			if (isPinHidden(btnIds[i])) keep[i] = false;
		}
		else if (isPinOnly(btnIds[i])) {
			keep[i] = false;
		}
	}
	// 折叠相邻 |，去掉首尾多余分隔
	std::vector<bool> out = keep;
	for (size_t i = 0; i < btnIds.size(); i++) {
		if (btnIds[i] == L"|") out[i] = false;
	}
	int prev = -1;
	for (size_t i = 0; i < btnIds.size(); i++) {
		if (btnIds[i] == L"|" || !keep[i]) continue;
		if (prev >= 0) {
			for (int j = (int)i - 1; j > prev; --j) {
				if (btnIds[j] == L"|") { out[j] = true; break; }
			}
		}
		prev = (int)i;
	}
	return out;
}

void ToolCap::applySlotVisibility()
{
	const auto vis = visibleSlots();
	size_t bi = 0, si = 0;
	for (size_t i = 0; i < btnIds.size(); i++) {
		if (btnIds[i] == L"|") {
			if (si < splitterNodes.size()) {
				if (vis[i]) splitterNodes[si]->show();
				else splitterNodes[si]->hide();
			}
			++si;
		}
		else {
			if (bi < btns.size()) {
				if (vis[i]) btns[bi]->show();
				else btns[bi]->hide();
			}
			++bi;
		}
	}
}

namespace {
	// 运行时槽位 id ↔ 设置页（ToolbarStore）槽位 id 的互转。两边历史上命名不统一
	// （number/serial-number、pin/fixed、long/scroll-screenshot、close/cancel、
	// clipboard/confirm、laser/laser-pointer、border/show-border），集中映射；
	// 分隔符 | 在设置页里叫 split-*。
	std::wstring storeIdOfRuntime(const std::wstring& rid)
	{
		if (rid == L"number") return L"serial-number";
		if (rid == L"pin") return L"fixed";
		if (rid == L"long") return L"scroll-screenshot";
		if (rid == L"close") return L"cancel";
		if (rid == L"clipboard") return L"confirm";
		if (rid == L"laser") return L"laser-pointer";
		if (rid == L"border") return L"show-border";
		return rid;
	}
	std::wstring runtimeIdOfStore(const std::wstring& sid)
	{
		if (sid.rfind(L"split", 0) == 0) return L"|";
		if (sid == L"serial-number") return L"number";
		if (sid == L"fixed") return L"pin";
		if (sid == L"scroll-screenshot") return L"long";
		if (sid == L"cancel") return L"close";
		if (sid == L"confirm") return L"clipboard";
		if (sid == L"laser-pointer") return L"laser";
		if (sid == L"show-border") return L"border";
		return sid;
	}
}

// 按设置页保存的布局重排运行时槽位表。没存过就保持内置默认（ToolbarStore 的默认
// 与运行时默认未必逐字相同，未自定义的用户不该被动改变）。
void ToolCap::applyStoredLayout(int kind)
{
	if (!ToolbarStore::hasSavedLayout(kind)) return;
	auto st = ToolbarStore::loadLayout(kind);

	struct Slot { std::wstring id, code, tip; };
	std::vector<Slot> pool;   // 设置页里存在的槽位：按设置页顺序重排
	std::vector<Slot> kept;   // 设置页里没有的槽位（如截图栏里贴图专属的 border/opacity）：保持原位、放最前
	for (size_t i = 0; i < btnIds.size(); i++) {
		if (btnIds[i] == L"|") continue;   // 分隔符位置改由设置页的 split-* 决定
		Slot s{ btnIds[i], i < btnCodes.size() ? btnCodes[i] : L"", i < btnTips.size() ? btnTips[i] : L"" };
		const auto sid = storeIdOfRuntime(s.id);
		bool known = false;
		for (const auto& a : st.active) if (a == sid) { known = true; break; }
		if (!known) for (const auto& h : st.inactive) if (h == sid) { known = true; break; }
		(known ? pool : kept).push_back(std::move(s));
	}

	std::vector<Slot> ordered;
	for (const auto& sid : st.active) {
		if (sid.rfind(L"split", 0) == 0) {
			ordered.push_back({ L"|", L"", L"" });
			continue;
		}
		const auto rid = runtimeIdOfStore(sid);
		for (auto& s : pool) {
			if (s.id == rid) { ordered.push_back(s); break; }
		}
	}
	// 收一遍多余分隔符（visibleSlots 运行时还会再折一次）；空布局则原样保留，别把工具栏弄空
	std::vector<Slot> merged;
	for (auto& s : ordered) {
		if (s.id == L"|" && (merged.empty() || merged.back().id == L"|")) continue;
		merged.push_back(std::move(s));
	}
	while (!merged.empty() && merged.back().id == L"|") merged.pop_back();
	if (merged.empty()) return;

	btnIds.clear(); btnCodes.clear(); btnTips.clear();
	for (auto& s : kept) { btnIds.push_back(s.id); btnCodes.push_back(s.code); btnTips.push_back(s.tip); }
	for (auto& s : merged) { btnIds.push_back(s.id); btnCodes.push_back(s.code); btnTips.push_back(s.tip); }
}

// 演示模式（全屏画布）工具条：槽位与图标严格照默认全屏布局
// （ToolbarLayoutStore kind=2 默认布局）：
//   穿透 白板 ┊ 矩形 / 箭头 / 画笔 / 文字 / 序号 / 马赛克 / 橡皮擦 / 渐隐画笔 ┊ 撤销 ┊ 重置画布 / 关闭
// 与普通截图栏的差别：没有椭圆、没有贴图/OCR/翻译/长截图/保存/完成，多了渐隐画笔、重置画布、穿透、白板。
// 注意：要在 createNativeWindow（onCreated 建按钮）之前调用。
void ToolCap::applyDemoLayout()
{
	demoLayout = true;
	// 演示栏里 laser 与 pen 是同一层级的两个槽位（pickedSub 映射见 onCreated）
	btnIds = {
		L"mouse-through", L"whiteboard", L"|",
		L"rect", L"arrow", L"pen", L"text", L"number", L"mosaic", L"eraser", L"laser", L"|",
		L"undo", L"|",
		L"extra", L"reset-canvas", L"close"
	};
	btnCodes = {
		Icon::Cursor, Icon::Board, L"",
		Icon::Rect, Icon::Arrow, Icon::Pen, Icon::Text, Icon::Number, Icon::Mosaic, Icon::Eraser, Icon::FadePen, L"|",
		Icon::Undo, L"|",
		Icon::Video, Icon::Redo, Icon::Cancel
	};
	btnTips = {
		L"tool.mouseThrough", L"tool.whiteboard", L"",
		L"tool.rect", L"tool.arrow", L"tool.pen", L"tool.text", L"tool.number", L"tool.mosaic", L"tool.eraser", L"tool.fadePen", L"",
		L"tool.undo", L"",
		L"cap.video", L"tool.resetCanvas", L"tool.close"
	};
	// 槽位换了必须重算窗口宽度：构造函数里那次 refreshSize 用的是默认（截图栏）槽位表，
	// 不重算的话演示栏会带着截图栏的宽度留一大截空白（"工具栏宽度没自适应"）。
	applyStoredLayout(2);   // 设置页若自定义过「全屏画布」布局，覆盖上面的内置默认
	// 老的自定义布局里没有白板槽位：补在穿透右边，保证升级后也能看到入口
	{
		bool hasBoard = false;
		for (const auto& id : btnIds) if (id == L"whiteboard") { hasBoard = true; break; }
		if (!hasBoard) {
			auto it = std::find(btnIds.begin(), btnIds.end(), L"mouse-through");
			const size_t pos = (it == btnIds.end()) ? 0 : (size_t)(it - btnIds.begin()) + 1;
			btnIds.insert(btnIds.begin() + pos, L"whiteboard");
			btnCodes.insert(btnCodes.begin() + pos, Icon::Board);
			btnTips.insert(btnTips.begin() + pos, L"tool.whiteboard");
		}
	}
	// 同样兼容旧布局：没有屏幕录制槽位就补在「重置画布」前面
	{
		bool hasRec = false;
		for (const auto& id : btnIds) if (id == L"extra") { hasRec = true; break; }
		if (!hasRec) {
			auto it = std::find(btnIds.begin(), btnIds.end(), L"reset-canvas");
			const size_t pos = (it == btnIds.end()) ? btnIds.size() : (size_t)(it - btnIds.begin());
			btnIds.insert(btnIds.begin() + pos, L"extra");
			btnCodes.insert(btnCodes.begin() + pos, Icon::Video);
			btnTips.insert(btnTips.begin() + pos, L"cap.video");
		}
	}
	refreshSize();
}

void ToolCap::setDemoBoardState(bool on)
{
	for (auto* b : btns) {
		if (!b || b->id != L"whiteboard") continue;
		if (on) applySelectedStyle(b);
		else applyNormalStyle(b);
		break;
	}
}

void ToolCap::setDemoRecordState(bool on)
{
	for (auto* b : btns) {
		if (!b || b->id != L"extra") continue;
		if (on) applySelectedStyle(b);
		else applyNormalStyle(b);
		break;
	}
}

void ToolCap::setDemoThroughState(bool on)
{
	for (auto* b : btns) {
		if (!b || b->id != L"mouse-through") continue;
		if (on) applySelectedStyle(b);
		else applyNormalStyle(b);
		break;
	}
}

void ToolCap::applyPinLayout()
{
	if (!winPin) return;
	applySlotVisibility();
	for (auto* b : btns) {
		if (!b || b->id != L"border") continue;
		if (winPin->isBorderVisible()) applySelectedStyle(b);
		else applyNormalStyle(b);
		break;
	}
	refreshSize();
	layout();
	updateUndoEnabled();
}

void ToolCap::updateUndoEnabled()
{
	Ling::Button* undoBtn = nullptr;
	for (auto* b : btns) {
		if (b && b->id == L"undo") { undoBtn = b; break; }
	}
	if (!undoBtn) return;
	const bool ok = annotHost() && annotHost()->history && annotHost()->history->canUndo();
	// 仅弱化图标；不改光标（保持普通箭头），点击在 onClick 里拦截
	if (ok) {
		undoBtn->setColor(Icon::ColorNormal);
		undoBtn->setHoverColor(Icon::ColorNormal);
		undoBtn->setHoverBg(ToolbarTheme::hoverBg);
	}
	else {
		undoBtn->setColor(Icon::ColorDisabled);
		undoBtn->setHoverColor(Icon::ColorDisabled);
		undoBtn->setHoverBg(0);
	}
}

bool ToolCap::hasSubTools(const std::wstring& slotId)
{
	// extra（屏幕录制）不再有子工具气泡 —— 直接是普通按钮（只剩自己一项的气泡没有意义）
	return slotId == L"mosaic" || slotId == L"ocr";
}

const std::vector<ToolPickItem>* ToolCap::getSubTools(const std::wstring& slotId)
{
	static const std::vector<ToolPickItem> mosaicSubs = {
		{ L"mosaic", Icon::Mosaic, L"tool.mosaic" },
		{ L"patina", Icon::Patina, L"tool.patina" },
		{ L"watermark", Icon::Watermark, L"tool.watermark" },
		{ L"highlight", Icon::Highlight, L"tool.highlight" },
	};
	// 文字识别气泡 = 文字识别 + 识别二维码；
	// 原来的「清除识别结果」是多余项，已移除。
	static const std::vector<ToolPickItem> ocrSubs = {
		{ L"ocr", Icon::Ocr, L"cap.ocr" },
		{ L"qrcode", Icon::Qrcode, L"cap.qrcode" },
	};
	if (slotId == L"mosaic") return &mosaicSubs;
	if (slotId == L"ocr") return &ocrSubs;
	return nullptr;
}

std::wstring ToolCap::resolveToolId(const std::wstring& slotId) const
{
	auto it = pickedSub.find(slotId);
	if (it != pickedSub.end()) return it->second;
	return slotId;
}

void ToolCap::showComingSoon()
{
	MessageBoxW(hwnd, Lang::get(L"cap.comingSoon").c_str(), Lang::get(L"about.sysTip").c_str(),
		MB_OK | MB_ICONINFORMATION);
}

void ToolCap::updateSlotButton(const std::wstring& slot)
{
	const auto eff = resolveToolId(slot);
	for (auto* b : btns) {
		if (b->id != slot) continue;
		const auto* subs = getSubTools(slot);
		if (!subs) break;
		for (const auto& item : *subs) {
			if (item.id != eff) continue;
			b->setText(item.icon);
			if (item.tipKey) bindTip(b, Lang::get(item.tipKey), item.id);
			break;
		}
		break;
	}
}

void ToolCap::pickSub(const std::wstring& slot, const std::wstring& subId)
{
	pickedSub[slot] = subId;
	updateSlotButton(slot);
	if (winPin && curId == slot) {
		const auto eff = resolveToolId(slot);
		if (!isAnnotationTool(eff)) {
			cancelSelect();
			showComingSoon();
			return;
		}
		showToolSubForEffective(eff);
		if (auto* host = annotHost()) {
			host->syncViewportFilters();
			host->layoutTools();
		}
	}
}

void ToolCap::showToolSubForEffective(const std::wstring& eff)
{
	auto* host = annotHost();
	if (!host || !host->toolSub) return;
	if (eff == L"rect") host->toolSub->showRectTools();
	else if (eff == L"arrow") host->toolSub->showArrowTools();
	else if (eff == L"pen") host->toolSub->showPenTools();
	else if (eff == L"number") host->toolSub->showNumberTools();
	else if (eff == L"text") host->toolSub->showTextTools();
	else if (eff == L"mosaic") host->toolSub->showMosaicTools();
	else if (eff == L"patina") host->toolSub->showPatinaTools();
	else if (eff == L"watermark") host->toolSub->showWatermarkTools();
	else if (eff == L"highlight") host->toolSub->showHighlightTools();
	else if (eff == L"eraser") host->toolSub->showEraserTools();
	else if (eff == L"opacity") host->toolSub->showOpacityTools();
	else host->toolSub->hideTools();
}

AnnotHost* ToolCap::annotHost() const
{
	if (winPin) return winPin;
	if (winCap) return winCap;
	return nullptr;
}

void ToolCap::syncOcrTranslateButtons()
{
	// 只有截图栏的「识别 / 翻译」是浮层开关，贴图栏走各自逻辑
	if (!winCap) return;
	const bool hasPanel = winCap->hasOcrPanel();
	const bool isTranslate = winCap->isTranslatePanel();
	for (auto* b : btns) {
		if (b->id == L"ocr") {
			if (hasPanel && !isTranslate) applySelectedStyle(b); else applyNormalStyle(b);
		}
		else if (b->id == L"translate") {
			if (hasPanel && isTranslate) applySelectedStyle(b); else applyNormalStyle(b);
		}
	}
}

void ToolCap::selectAnnotTool(const std::wstring& id)
{
	// 标注与「识别 / 翻译」浮层可以并存：切标注工具时**不**收掉识别面板，
	// 用户不必先取消识别再标注（浮层里只有文字块吃鼠标，其余区域穿透，仍可正常画）。
	// 渐隐画笔 / 画笔 是同级一级槽位，底层都是 pen：先定好 isPenFade 再建属性栏
	if (auto* host = annotHost(); host && host->toolSub) host->toolSub->setPenSlotMode(id);
	// 包浆/水印/高亮是 mosaic 槽的子工具，工具栏按钮 id 仍是 mosaic
	std::wstring slot = id;
	if (id == L"patina" || id == L"watermark" || id == L"highlight") {
		slot = L"mosaic";
		pickedSub[L"mosaic"] = id;
		updateSlotButton(slot);
	}
	Ling::Button* target = nullptr;
	for (auto b : btns) {
		if (b->id == slot) { target = b; break; }
	}
	if (!target) return;
	for (auto b : btns) {
		if (b->id == curId) applyNormalStyle(b);
		if (b->id == slot) applySelectedStyle(b);
	}
	curId = slot;
	showToolSubForEffective(resolveToolId(slot));
	if (auto* host = annotHost()) {
		host->presentSelectedAnnotStyle();
		host->syncViewportFilters();
		host->layoutTools();
	}
}

void ToolCap::onHoverMove(POINT pos)
{
	if (!picker) return;
	if (draggingBar) {
		cancelHidePicker();
		picker->hidePicker();
		hoverBtn = nullptr;
		hoverSuppress = nullptr;
		return;
	}

	auto openFor = [this](Ling::Button* btn) {
		// 演示模式没有二级子工具（把马赛克/水印拆成独立槽，不弹气泡）
		if (demoLayout) return;
		const auto* subs = getSubTools(btn->id);
		if (!subs) return;
		cancelHidePicker();
		hoverBtn = btn;
		picker->showFor(this, btn, *subs, resolveToolId(btn->id),
			[this, slot = btn->id](const std::wstring& subId) {
				pickSub(slot, subId);
				if (winPin) {
					for (auto* b : btns) {
						if (b->id == slot) { onClickPin(b); break; }
					}
				}
				else if (winCap && slot == L"mosaic" && isAnnotationTool(subId)) {
					winCap->startAnnotate(subId);
				}
				else if (winCap && slot == L"ocr") {
					if (subId == L"qrcode") winCap->startQrcode();
					else winCap->startOcr();
				}
			});
	};

	// 离开主栏 HWND：在气泡（或主栏↔气泡通道）上则保持，否则延迟关以便穿过空隙
	if (pos.x == INT_MAX) {
		POINT screen{};
		GetCursorPos(&screen);
		if (picker->isOpen() && picker->hitKeepOpen(screen)) {
			cancelHidePicker();
			return;
		}
		hoverSuppress = nullptr; // 离开主栏后再次停悬算二次
		if (picker->isOpen()) scheduleHidePicker();
		return;
	}

	Ling::Button* found = nullptr;
	for (auto* b : btns) {
		if (b->isPosIn(pos)) { found = b; break; }
	}

	// 点按关闭后：停在同按钮上不再弹，直到移开
	if (hoverSuppress) {
		if (found == hoverSuppress) return;
		hoverSuppress = nullptr;
	}

	POINT screen{};
	GetCursorPos(&screen);
	if (picker->isOpen() && picker->hitTestScreen(screen) && !found) {
		cancelHidePicker();
		return;
	}

	if (found && getSubTools(found->id)) {
		if (found == hoverBtn && picker->isOpen()) {
			cancelHidePicker();
			return;
		}
		openFor(found);
		return;
	}

	if (found) {
		// 明确停到无子工具图标 → 立刻关
		hoverBtn = found;
		cancelHidePicker();
		if (picker->isOpen()) picker->hidePicker();
		return;
	}

	// 主栏内但不在图标上（顶边/缝隙）：保持打开，才能移到气泡
	if (picker->isOpen()) cancelHidePicker();
}

void ToolCap::scheduleHidePicker()
{
	setTimer(120, pickerHideTimerId);
}

void ToolCap::cancelHidePicker()
{
	killTimer(pickerHideTimerId);
}

void ToolCap::onPickerHideTimer()
{
	killTimer(pickerHideTimerId);
	if (!picker || !picker->isOpen()) return;
	POINT pt{};
	GetCursorPos(&pt);
	if (picker->hitKeepOpen(pt)) return;
	picker->hidePicker();
	hoverBtn = nullptr;
	// suppress 保留：若仍停在原按钮上，由 onHoverMove 继续挡；已离开则下次停悬可弹
}

void ToolCap::attachToPin(WinPin* pin, const std::wstring& initialTool)
{
	winPin = pin;
	winCap = nullptr;
	dpi = pin->dpi;
	applyPinLayout();
	// 预选工具由 WinPin 在进入编辑态时再 select（贴图默认藏栏）
	(void)initialTool;
	pin->layoutTools();
}

float ToolCap::getBtnCenterX() const
{
	// 按设计格（btnSize / splitterSlotW）算中心，不能累加 btn->w：后者不含 margin，越往后越偏
	float result{ (ToolbarTheme::shadowPad + contentLeadWidth()) * dpi };
	const std::wstring& anchor = anchorId.empty() ? curId : anchorId;
	const auto vis = visibleSlots();
	for (size_t i = 0; i < btnIds.size(); i++)
	{
		if (!vis[i]) continue;
		if (btnIds[i] == L"|") {
			result += splitterSlotW() * dpi;
			continue;
		}
		if (anchor == btnIds[i]) {
			result += btnSize * dpi * 0.5f;
			return result;
		}
		result += btnSize * dpi;
	}
	return result;
}

void ToolCap::onCreated()
{
	// 首次建窗时把当前主题色板刷进 ToolbarTheme（含 Icon::ColorNormal/Disabled），
	// 之后所有 ToolbarTheme::background / hoverBg / 图标色调用点即拿到用户所选主题。
	ToolbarTheme::refresh();
	pickedSub[L"mosaic"] = L"mosaic";
	pickedSub[L"extra"] = L"video";
	pickedSub[L"ocr"] = L"ocr";
	// 「渐隐画笔」是独立一级槽位，但底层工具仍是画笔（laser = pen + isPenFade）：
	// 槽位 id → 工具 id 显式映射，curId 可以是 laser（排他选中 / 取消选中 / 属性栏锚点按它算），
	// 而标注引擎通过 getCurToolId() 仍然拿到 pen。
	pickedSub[L"laser"] = L"pen";
	tip = std::make_unique<Tip>(this);
	picker = std::make_unique<ToolPicker>();
	picker->onHoverChange = [this](bool inside) {
		if (inside) cancelHidePicker();
		else scheduleHidePicker();
	};
	onTimer.add([this](UINT id) {
		if (id == pickerHideTimerId) onPickerHideTimer();
		else if (id == demoEdgeTimerId) onDemoEdgeTimer();
	});
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
	initDragHandle();
	const float sepMarginV = (btnSize - spliterH) * 0.5f;
	const float sepMarginH = ToolbarTheme::splitterGap - hoverInset;
	for (size_t i = 0; i < btnIds.size(); i++)
	{
		auto& id = btnIds[i];
		if (id == L"|") {
			auto spliter = contentNode->makeChild<Ling::Node>();
			spliter->setSize(spliterW, spliterH);
			spliter->setMargin(sepMarginH, sepMarginV, sepMarginH, sepMarginV);
			spliter->setBg(ToolbarTheme::splitter);
			spliter->setFlexGrow(0.f);
			spliter->setFlexShrink(0.f);
			splitterNodes.push_back(spliter);
		}
		else {
			auto btn = contentNode->makeChild<Ling::Button>();
			btn->setId(id);
			btn->setText(btnCodes[i]);
			styleToolbarBtn(btn);
			btn->setFontFamily(Icon::Family);
			btn->setFontSize(Icon::Size);
			applyActionColors(btn);
			btn->onClick.add([this](Ling::Button* btn) { onClick(btn); });
			if (winPin && id == L"close") {
				tip->bind(btn, Lang::get(L"pin.cancelEdit"));
			}
			else if (winPin && id == L"clipboard") {
				tip->bind(btn, Lang::get(L"pin.confirmEdit"));
			}
			else if (!btnTips[i].empty()) {
				bindTip(btn, Lang::get(btnTips[i]));
			}
			btns.push_back(btn);
		}
	}
	if (winPin) applyPinLayout();
	else applySlotVisibility();
	onMouseDown.add([this](POINT pos, bool isRight) { onDragDown(pos, isRight); });
	onMouseMove.add([this](POINT pos) {
		onDragMove(pos);
		onHoverMove(pos);
	});
	onMouseUp.add([this](POINT pos, bool isRight) { onDragUp(pos, isRight); });
	// 截图栏立即显示；钉图栏要等 WinPin 先 layoutTools 再 show
	if (!winPin) show();
	updateUndoEnabled();
}

void ToolCap::initDragHandle()
{
	dragHandle = contentNode->makeChild<Ling::Button>();
	dragHandle->setId(L"drag");
	dragHandle->setText(Icon::DragHandle);
	dragHandle->setFontFamily(Icon::Family);
	dragHandle->setFontSize(Icon::Size);
	// 半透明手柄、无悬停灰底
	dragHandle->setColor(ToolbarTheme::dragHandleColor);
	dragHandle->setHoverColor(ToolbarTheme::dragHandleColor);
	dragHandle->setHoverBg(0);
	const float marginV = (btnSize - ToolbarTheme::dragIconSize) * 0.5f;
	dragHandle->setSize(ToolbarTheme::dragIconSize, ToolbarTheme::dragIconSize);
	dragHandle->setMargin(0.f, marginV, dragMarginRight(), marginV);
	dragHandle->setFlexGrow(0.f);
	dragHandle->setFlexShrink(0.f);
	dragHandle->setAlignItems(Ling::Align::Center);
	dragHandle->setJustifyContent(Ling::Justify::Center);
	// 拖拽手柄不给悬停提示：它只是移动工具条
}

void ToolCap::layout()
{
	Ling::WinBase::layout();
	paintChrome();
}

void ToolCap::onDragDown(POINT pos, bool isRight)
{
	if (isRight) {
		// 工具栏右键：分层取消，但不重框选/退出
		if (winCap) winCap->handleLayerCancel(true);
		return;
	}
	if (!dragHandle || !dragHandle->isPosIn(pos)) return;
	// 新一次拖动：先撤掉自动隐藏态，免得拖动途中被探测定时器拉回
	demoAutoHide = false;
	demoCollapsed = false;
	killTimer(demoEdgeTimerId);
	draggingBar = true;
	dragGrabOffset = pos;
	if (hwnd) SetCapture(hwnd);
	tip->hide();
	if (picker) {
		cancelHidePicker();
		picker->hidePicker();
	}
	hoverBtn = nullptr;
	hoverSuppress = nullptr;
}

void ToolCap::onDragMove(POINT pos)
{
	// 贴边自动隐藏：复用录屏工具栏那套 —— 缩进后只露一条边，鼠标移上去就滑出，
	// 离开工具栏（及属性栏）150ms 后再缩回。
	if (demoAutoHide && !draggingBar) {
		if (pos.x == INT_MAX && pos.y == INT_MAX) {
			if (!demoCollapsed) scheduleDemoCollapse();
		}
		else {
			killTimer(demoEdgeTimerId);
			if (demoCollapsed) demoExpandFromEdge();
		}
	}
	if (!draggingBar) return;
	POINT pt{};
	GetCursorPos(&pt);
	setPosition(pt.x - dragGrabOffset.x, pt.y - dragGrabOffset.y);
	if (picker && picker->isOpen()) picker->reposition();
	if (auto* host = annotHost(); host && host->toolSub && host->toolSub->hasContent()) {
		RECT winRect{ x, y, x + static_cast<int>(w), y + static_cast<int>(h) };
		host->toolSub->updatePosition(ToolbarChrome::workAreaNear(winRect));
	}
}

void ToolCap::onDragUp(POINT, bool)
{
	if (!draggingBar) return;
	draggingBar = false;
	if (GetCapture() == hwnd) ReleaseCapture();
	if (auto* host = annotHost()) host->markMainToolUserPlaced();
	applyDemoEdgeBehavior();
}

// 演示模式：拖动结束后按「工具栏贴边」设置处理：
//   0 吸附边缘 —— 停手即贴到最近的上/下屏幕边（工作区，不压任务栏）；
//   1 自动隐藏 —— 复用录屏工具栏的「贴边隐藏」：贴到屏幕上下沿后缩进，只露 4px 条，
//                  鼠标移上去滑出、离开 150ms 再缩回；
//   2 无 —— 不吸附、不自动隐藏，停在拖放位置。
// 非演示模式（截图 / 贴图工具条）不走这套，保持落哪算哪。
void ToolCap::applyDemoEdgeBehavior()
{
	if (!demoLayout || !winCap) return;
	const int behavior = Setting::get()->getDemoEdgeBehavior();
	if (behavior == 2) {
		// 无：位置保持拖放结果，仅让属性栏按当前落点重排（箭头方向跟着变）
		demoAutoHide = false;
		demoCollapsed = false;
		killTimer(demoEdgeTimerId);
		if (auto* host = annotHost()) host->layoutTools();
		return;
	}

	if (behavior != 1) {
		// 吸附边缘（工作区，避免压住任务栏）
		const RECT wa = ToolbarChrome::workAreaNear(RECT{ x, y, x + (int)w, y + (int)h });
		const int barW = (int)w, barH = (int)h;
		const int distTop = y - wa.top;
		const int distBottom = wa.bottom - (y + barH);
		demoEdge = (distTop <= distBottom) ? 0 : 1;
		demoExpandX = std::clamp(x, (int)wa.left, (int)wa.right - barW);
		demoExpandY = (demoEdge == 0) ? wa.top : wa.bottom - barH;
		demoAutoHide = false;
		demoCollapsed = false;
		killTimer(demoEdgeTimerId);
		setPosition(demoExpandX, demoExpandY);
		// 吸附后必须立刻重排属性栏：否则它还停在上一次的位置，
		// 贴到底部时箭头也不会翻到上方。
		if (auto* host = annotHost()) host->layoutTools();
		return;
	}

	// 自动隐藏：与录屏工具栏一致，贴边判定和缩进都用整块屏幕 rcMonitor
	MONITORINFO mi{ sizeof(MONITORINFO) };
	if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return;
	const RECT area = mi.rcMonitor;
	const int barW = (int)w, barH = (int)h;
	const int distTop = y - area.top;
	const int distBottom = area.bottom - (y + barH);
	demoEdge = (distTop <= distBottom) ? 0 : 1;
	demoExpandX = std::clamp(x, (int)area.left, (int)area.right - barW);
	demoExpandY = (demoEdge == 0) ? area.top : area.bottom - barH;
	demoAutoHide = true;
	demoCollapsed = false;
	demoCollapseToEdge();
}

// 缩进边缘：藏到屏幕之外，只露出 demoPeekStripPx 高的一条（与录屏工具栏一致）
void ToolCap::demoCollapseToEdge()
{
	if (!hwnd || !demoAutoHide) return;
	killTimer(demoEdgeTimerId);
	demoCollapsed = true;
	MONITORINFO mi{ sizeof(MONITORINFO) };
	RECT rc{ demoExpandX, demoExpandY, demoExpandX + (int)w, demoExpandY + (int)h };
	RECT area = rc;
	if (GetMonitorInfoW(MonitorFromRect(&rc, MONITOR_DEFAULTTONEAREST), &mi)) area = mi.rcMonitor;
	const int strip = (int)(demoPeekStripPx * dpi + 0.5f);
	const int nx = std::clamp(demoExpandX, (int)area.left, (int)area.right - (int)w);
	const int ny = (demoEdge == 0) ? area.top - ((int)h - strip) : area.bottom - strip;
	if (x != nx || y != ny) setPosition(nx, ny);
	if (auto* host = annotHost(); host && host->toolSub) host->toolSub->hideTools();
}

void ToolCap::demoExpandFromEdge()
{
	if (!demoAutoHide) return;
	killTimer(demoEdgeTimerId);
	demoCollapsed = false;
	setPosition(demoExpandX, demoExpandY);
	// 收起时一并藏了属性栏，弹回来时按当前工具恢复
	if (auto* host = annotHost()) host->layoutTools();
}

void ToolCap::scheduleDemoCollapse()
{
	if (!demoAutoHide || demoCollapsed || draggingBar) return;
	setTimer(demoPeekHideDelayMs, demoEdgeTimerId);
}

// 延时到点：鼠标仍不在工具栏/属性栏上才缩回（避免从工具栏移到属性栏时被收起）
void ToolCap::onDemoEdgeTimer()
{
	killTimer(demoEdgeTimerId);
	if (!demoAutoHide || demoCollapsed || draggingBar) return;
	POINT pt{};
	if (!GetCursorPos(&pt)) return;
	RECT bar{ x, y, x + (int)w, y + (int)h };
	if (PtInRect(&bar, pt)) return;
	if (auto* host = annotHost(); host && host->toolSub && host->toolSub->hwnd) {
		RECT sub{};
		if (GetWindowRect(host->toolSub->hwnd, &sub) && PtInRect(&sub, pt)) {
			// 人还在属性栏上：延后再看一次（属性栏离开主工具栏后不会再有鼠标移出事件）
			setTimer(demoPeekHideDelayMs, demoEdgeTimerId);
			return;
		}
	}
	demoCollapseToEdge();
}

void ToolCap::paintChrome()
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

// 停悬气泡：按「快捷键提示」(外观设置 > 其他外观) 决定是否把该按钮当前的键位拼进文字，
// 如 "矩形" → "矩形（1）"。贴图布局不属于命令表（贴图/镜像组已取消），故不提示。
void ToolCap::bindTip(Ling::Button* btn, const std::wstring& text, const std::wstring& lookup)
{
	if (!tip || !btn) return;
	std::wstring full = text;
	if (!winPin && Setting::get() && Setting::get()->getHotkeyTip()) {
		const unsigned surface = demoLayout ? CommandKeys::SurfaceFullscreen : CommandKeys::SurfaceCapture;
		const std::wstring eff = lookup.empty() ? resolveToolId(btn->id) : lookup;
		const std::wstring chord = CommandKeys::hintChord(btn->id, eff, surface);
		if (!chord.empty()) full += L"（" + chord + L"）";
	}
	tip->bind(btn, full);
}

void ToolCap::styleToolbarBtn(Ling::Button* btn)
{
	const float inner{ btnSize - hoverInset * 2.f };
	btn->setSize(inner, inner);
	btn->setMargin(hoverInset);
	btn->setFlexGrow(0.f);
	btn->setFlexShrink(0.f);
	btn->setBorderRadius(hoverRadius);
	btn->setHoverBg(ToolbarTheme::hoverBg);
	btn->setAlignItems(Ling::Align::Center);
	btn->setJustifyContent(Ling::Justify::Center);
}

void ToolCap::applyActionColors(Ling::Button* btn)
{
	if (btn->id == L"close") {
		btn->setColor(Icon::ColorCancel);
		btn->setHoverColor(Icon::ColorCancel);
	}
	else if (btn->id == L"clipboard") {
		btn->setColor(Icon::ColorDone);
		btn->setHoverColor(Icon::ColorDone);
	}
	else {
		btn->setColor(ToolbarTheme::iconNormal);
		btn->setHoverColor(ToolbarTheme::iconNormal);   // 图标色（停悬/常态同一色）
	}
}

void ToolCap::applyNormalStyle(Ling::Button* btn)
{
	btn->setBg(0);
	btn->setHoverBg(ToolbarTheme::hoverBg);
	applyActionColors(btn);
}

void ToolCap::applySelectedStyle(Ling::Button* btn)
{
	btn->setBg(0);
	btn->setHoverBg(ToolbarTheme::hoverBg);
	btn->setColor(ToolbarTheme::iconActive);      // 选中（主题色槽 4：accent）
	btn->setHoverColor(ToolbarTheme::iconActive);
}

void ToolCap::cancelSelect()
{
	if (curId.empty()) return;
	for (auto b : btns)
	{
		if (b->id == curId) {
			applyNormalStyle(b);
		}
	}
	curId.clear();
	if (auto* host = annotHost()) {
		// 白板属性栏不跟着绘图工具一起收：右键层级是先取消工具，再取消编辑背景，
		// 最后才收属性栏（见 WinCap::handleLayerCancel）。
		if (host->toolSub && !host->toolSub->boardToolsBuilt()) host->toolSub->hideTools();
		// 再点水印/包浆取消选中时立刻撤掉视口滤镜（不要等到切到别的工具）
		host->syncViewportFilters();
		host->layoutTools();
		// 白板属性栏只是被工具属性栏临时替换：取消工具后还回来
		if (auto* cap = dynamic_cast<WinCap*>(host)) cap->restoreDemoBoardBar();
	}
}

void ToolCap::restoreToolSelection(const std::wstring& id)
{
	if (id.empty()) return;
	// 只改选中态与画笔模式，不打开属性栏（白板属性栏刚收起，不应该冒出别的东西）
	if (auto* host = annotHost(); host && host->toolSub) host->toolSub->setPenSlotMode(id);
	Ling::Button* target = nullptr;
	for (auto b : btns) {
		if (b->id == id) { target = b; break; }
	}
	if (!target) return;
	for (auto b : btns) {
		if (b->id == curId) applyNormalStyle(b);
		if (b->id == id) applySelectedStyle(b);
	}
	curId = id;
	if (auto* host = annotHost()) host->presentSelectedAnnotStyle();
}

void ToolCap::bindOwner(HWND ownerHwnd)
{
	if (!hwnd || !ownerHwnd) return;
	// WS_POPUP 的 GWLP_HWNDPARENT = owner；owned 窗口始终在 owner 之上
	SetWindowLongPtr(hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(ownerHwnd));
}

void ToolCap::selectTool(const std::wstring& id)
{
	for (auto b : btns) {
		if (b->id == id) {
			onClickPin(b);
			return;
		}
	}
}

// 「工具循环」(系统按键 Tab)：在可见的绘图工具槽位之间循环切换一个。
// 顺序即工具栏从左到右（btnIds 顺序）；隐藏的槽位不参与。
// 包浆/水印/高亮属 mosaic 槽的子工具，不单独计数（判据 isAnnotationTool）。
void ToolCap::cycleTool()
{
	const auto vis = visibleSlots();
	std::vector<Ling::Button*> tools;
	size_t bi = 0;   // btns 与 btnIds 里**非分隔符**的项一一对应
	for (size_t i = 0; i < btnIds.size(); i++) {
		if (btnIds[i] == L"|") continue;
		Ling::Button* b = (bi < btns.size()) ? btns[bi] : nullptr;
		++bi;
		if (!b || !isAnnotationTool(btnIds[i])) continue;
		if (i < vis.size() && !vis[i]) continue;
		tools.push_back(b);
	}
	if (tools.empty()) return;
	// 当前选中的若不是绘图工具（还没选/停在撤销之类上）→ 从第一个开始
	size_t next = 0;
	for (size_t k = 0; k < tools.size(); k++) {
		if (tools[k]->id == curId) { next = (k + 1) % tools.size(); break; }
	}
	onClickCap(tools[next]);
}

void ToolCap::onClick(Ling::Button* btn)
{
	tip->hide();
	cancelHidePicker();
	if (picker) picker->hidePicker();
	hoverBtn = nullptr;
	hoverSuppress = btn; // 点按关气泡；同按钮需移开再停悬才再弹
	// 渐隐画笔 / 画笔：一级槽位，底层同为 pen —— 钉图路径不经过 selectAnnotTool，这里统一先定模式
	if ((btn->id == L"laser" || btn->id == L"pen")) {
		if (auto* host = annotHost(); host && host->toolSub) host->toolSub->setPenSlotMode(btn->id);
	}
	// 再点已选中工具 = 取消选中；但已有选中标注时保留工具与选中，只刷新属性栏
	if (!curId.empty() && btn->id == curId) {
		auto* host = annotHost();
		const auto eff = resolveToolId(btn->id);
		// 已有选中标注：这里只切换属性栏显隐 —— 不动工具、不动选中。
		// 关掉属性栏只是"我不改属性"，选中标注照样能拖/缩放/旋转/删除；
		// 再点一下又打开（顺带把滑条同步到该标注当前尺寸）。
		if (host && host->hasSelectedAnnot() && isAnnotationTool(eff)) {
			auto* sub = host->toolSub.get();
			if (sub && sub->hasContent() && sub->chromeVisible()) {
				sub->hideTools();
				host->layoutTools();
			}
			else {
				showToolSubForEffective(eff);
				host->presentSelectedAnnotStyle();
				host->layoutTools();
			}
			return;
		}
		cancelSelect();
		return;
	}
	if (winPin) onClickPin(btn);
	else onClickCap(btn);
}

void ToolCap::onClickCap(Ling::Button* btn)
{
	const auto eff = resolveToolId(btn->id);
	// 「渐隐画笔 / 画笔」是一级同级槽位：都走画笔工具，靠 isPenFade 区分（见 startPenSlot）
	if (winCap && (btn->id == L"laser" || btn->id == L"pen")) {
		// 演示画布：再点一次当前画笔 = 取消选中，回到"未选工具"——
		// 只有这个状态才能点选已有标注做二次编辑（见 WinCap::annotSkipHitSelect）
		if (demoLayout && curId == btn->id) { cancelSelect(); return; }
		winCap->startPenSlot(btn->id == L"laser");
		return;
	}
	// 演示模式：重置画布、穿透、白板、取消都走 WinCap 的演示动作
	if (demoLayout && winCap) {
		if (btn->id == L"reset-canvas") { winCap->resetDemoCanvas(); return; }
		if (btn->id == L"mouse-through") { winCap->toggleDemoThrough(); return; }
		if (btn->id == L"whiteboard") { winCap->toggleDemoBoard(); return; }
		if (btn->id == L"extra") { winCap->toggleDemoRecorder(); return; }
		if (btn->id == L"close") { winCap->exitDemoCanvas(); return; }
	}
	// 「显示光标」：截图/导出时把会话开始那一刻的系统光标一起拍进去（开关类，不参与工具排他）
	if (btn->id == L"show-cursor") {
		if (winCap) winCap->toggleShowCursor();
		const bool on = winCap && winCap->showCursorEnabled();
		if (on) applySelectedStyle(btn); else applyNormalStyle(btn);
		bindTip(btn, Lang::get(on ? L"tool.hideCursor" : L"tool.showCursor"));
		return;
	}
	if (hasSubTools(btn->id)) {
		if (btn->id == L"mosaic") {
			if (isAnnotationTool(eff)) winCap->startAnnotate(eff);
			else showComingSoon();
		}
		else if (btn->id == L"ocr") {
			if (eff == L"qrcode") {
				winCap->startQrcode();
			}
			else {
				winCap->startOcr();
				// 识别面板开着时按钮为选中态；再点一次收面板并恢复常态
				cancelSelect();
				syncOcrTranslateButtons();
			}
		}
	}
	// 屏幕录制：气泡已取消，直接是普通按钮
	else if (btn->id == L"extra") {
		winCap->startVideo();
	}
	else if (isAnnotationTool(btn->id)) {
		winCap->startAnnotate(btn->id);
	}
	else if (btn->id == L"pin") {
		winCap->startPin();
	}
	else if (btn->id == L"long") {
		winCap->startLong();
	}
	else if (btn->id == L"translate") {
		winCap->startTranslate();
		// 翻译面板开着时按钮为选中态；再点一次收面板并恢复常态
		cancelSelect();
		syncOcrTranslateButtons();
	}
	else if (btn->id == L"undo") {
		if (winCap->isAnnotating() && winCap->history && winCap->history->canUndo())
			winCap->history->undo();
	}
	else if (btn->id == L"save") {
		winCap->saveToFile();
	}
	else if (btn->id == L"clipboard") {
		winCap->copyToClipboard();
	}
	else if (btn->id == L"close") {
		requestCaptureClose();
	}
}

// 「取消」关掉截图窗口。已经画了标注时先确认（受「功能设置 > “取消”弹窗」开关控制），
// 避免误触直接丢失刚画的内容；没画标注就直接关，不打扰。
void ToolCap::requestCaptureClose()
{
	if (!winCap) return;
	const bool hasAnnot = winCap->history && !winCap->history->shapes.empty();
	auto* s = Setting::get();
	if (hasAnnot && (!s || s->getCancelConfirmDialog())) {
		// 确认框是异步的：回调时工具条可能已随窗口销毁，借 body 的存活哨兵判断
		auto weak = body ? body->getWeakThis() : std::weak_ptr<bool>{};
		SettingUi::ConfirmDialog::ask(winCap, Lang::get(L"setting.cancelCaptureConfirm"),
			[this, weak](bool ok) {
				if (!ok || !weak.lock()) return;
				if (winCap) winCap->close();
			});
		return;
	}
	winCap->close();
}

void ToolCap::onClickPin(Ling::Button* btn)
{
	if (isPinHidden(btn->id)) return;
	if (btn->id == L"close") {
		winPin->finishEditing(false);
		return;
	}
	else if (btn->id == L"undo") {
		if (winPin->history && winPin->history->canUndo())
			winPin->history->undo();
		return;
	}
	else if (btn->id == L"redo") {
		winPin->history->redo();
		return;
	}
	else if (btn->id == L"save") {
		winPin->saveToFile();
		return;
	}
	else if (btn->id == L"clipboard") {
		winPin->finishEditing(true);
		return;
	}
	else if (btn->id == L"border") {
		winPin->toggleBorderVisible();
		if (winPin->isBorderVisible()) applySelectedStyle(btn);
		else applyNormalStyle(btn);
		return;
	}
	else if (btn->id == L"opacity") {
		// 透明度：与其它工具一致，走标准属性栏（ToolSub + PropSlider），不再弹右键菜单那套自绘滑条
		for (auto b : btns) {
			if (b->id == curId) applyNormalStyle(b);
			if (b->id == btn->id) applySelectedStyle(b);
		}
		curId = btn->id;
		showToolSubForEffective(L"opacity");
		winPin->layoutTools();
		return;
	}
	const auto eff = resolveToolId(btn->id);
	if (hasSubTools(btn->id) && !isAnnotationTool(eff)) {
		showComingSoon();
		return;
	}
	for (auto b : btns)
	{
		if (b->id == curId)
		{
			applyNormalStyle(b);
		}
		if (b->id == btn->id)
		{
			applySelectedStyle(b);
		}
	}
	curId = btn->id;
	showToolSubForEffective(eff);
	winPin->presentSelectedAnnotStyle();
	winPin->syncViewportFilters();
	winPin->layoutTools();
}

void ToolCap::onMinMaxInfo(MINMAXINFO* mmi)
{
	mmi->ptMinTrackSize.x = 1;
	mmi->ptMinTrackSize.y = 1;
}
