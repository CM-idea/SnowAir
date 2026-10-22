#include "pch.h"
#include "../Lang.h"
#include "../Setting.h"
#include "../CommandKeys.h"
#include "WinSetting.h"
#include "WinSettingSystemKeys.h"
#include "SettingTheme.h"
#include "SettingWidgets.h"
#include "HotkeyHook.h"
#include "../Tool/IconCodes.h"
#include <algorithm>

namespace {
	// 键位框的显示文案：表内存的是稳定标识（MouseRight 等），仅在此处转换为界面语言。
	// 键盘键不做加工 —— 默认值与录制结果同为词首大写（Esc / Enter / Ctrl+C…），存什么显示什么。
	std::wstring chordDisplay(const std::wstring& chord)
	{
		if (chord.empty()) return Lang::get(L"shortcut.setToBind");
		// 鼠标键与键盘键共用同一 chord 命名空间：存 MouseRight 这类标识，显示时翻成界面语言
		if (_wcsicmp(chord.c_str(), L"MouseRight") == 0) return Lang::get(L"cmdkey.mouseRight");
		return chord;
	}

	bool isModifierKey(const std::wstring& key)
	{
		return key == L"Ctrl" || key == L"Alt" || key == L"Shift"
			|| key == L"Win" || key == L"LWin" || key == L"RWin";
	}

	// 录制中的实时预览：按当前按住的修饰键拼前缀（顺序与落盘口径一致，末尾带 "+"）。
	// 只反映修饰键，主键一按下就整条落盘并结束录制，所以预览到这里为止。
	std::wstring liveModifierPrefix()
	{
		std::wstring out;
		auto add = [&out](const wchar_t* name) {
			if (!out.empty()) out += L"+";
			out += name;
		};
		if (GetAsyncKeyState(VK_CONTROL) & 0x8000) add(L"Ctrl");
		if (GetAsyncKeyState(VK_MENU) & 0x8000) add(L"Alt");
		if (GetAsyncKeyState(VK_SHIFT) & 0x8000) add(L"Shift");
		if ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) add(L"Win");
		if (!out.empty()) out += L"+";
		return out;
	}

	std::wstring joinKeys(const std::vector<std::wstring>& keys)
	{
		std::wstring out;
		for (const auto& k : keys) {
			if (!out.empty()) out += L"+";
			out += (k == L"LWin" || k == L"RWin") ? L"Win" : k;
		}
		return out;
	}

	// 录到的这组键能否作为「系统按键」：允许不带修饰键的单键（默认值里就有 1..0 / W / Q / F /
	// ` / Space / Esc / Enter / Tab / Backspace）—— 这些命令只在会话窗口有焦点时派发，
	// 不占用其他程序的按键。要求恰好一个主键，且主键能被 strToKey 识别（否则落盘后无法命中）。
	bool isValidKeys(const std::vector<std::wstring>& keys)
	{
		if (keys.empty()) return false;
		unsigned mains = 0;
		for (const auto& k : keys) if (!isModifierKey(k)) mains++;
		if (mains != 1) return false;
		return CommandKeys::validChord(joinKeys(keys));
	}

	// 提示文案填充：%1 = 键位、%2 = 占用者的行标签（两句都取自 QT6 原句）
	std::wstring fillMessage(const wchar_t* key, const std::wstring& chord, const std::wstring& otherLabel)
	{
		std::wstring s = Lang::get(key);
		auto replaceAll = [](std::wstring& str, const std::wstring& from, const std::wstring& to) {
			size_t pos = 0;
			while ((pos = str.find(from, pos)) != std::wstring::npos) {
				str.replace(pos, from.size(), to);
				pos += to.size();
			}
			};
		replaceAll(s, L"%1", chord);
		replaceAll(s, L"%2", otherLabel);
		return s;
	}

	// 配色与「全局热键」页同款：
	//   常态 = 白底 + 1px 描边 + 深字（悬停变 accent）
	//   录制中 = #0FDC78 底 + 白字/白转圈，去掉描边，悬停也取同值
	// 只读 / 未实装行不走这里 —— 它们的键位框不是按钮（见 readOnlyKeyBox）
	constexpr uint32_t kCaptureBg{ 0x0FDC78FF };
	constexpr uint32_t kCaptureFg{ 0xFFFFFFFF };
	void styleKeyBtn(Ling::Button* btn, bool capturing)
	{
		if (capturing) {
			btn->setBg(kCaptureBg);
			btn->setHoverBg(kCaptureBg);
			btn->setBorder(0.f, 0);
			btn->setColor(kCaptureFg);
			btn->setHoverColor(kCaptureFg);
		} else {
			btn->setBg(SettingTheme::inputBg);
			btn->setHoverBg(SettingTheme::accent);
			btn->setBorder(1.f, SettingTheme::input);
			btn->setColor(SettingTheme::textPrimary);
			btn->setHoverColor(SettingTheme::textPrimary);
		}
	}

	// 转圈圈帧：「加载1~加载8」顺时针轮换（与「全局热键」页同一套）
	const wchar_t* kSpinnerFrames[] = {
		Icon::Loading1, Icon::Loading2, Icon::Loading3, Icon::Loading4,
		Icon::Loading5, Icon::Loading6, Icon::Loading7, Icon::Loading8,
	};
	constexpr int kSpinnerFramesCount = 8;

	// 键位框宽度：双栏下窄一点，不然"开启/关闭系统音量"这类长标签会被挤成两行。
	// 只读框与可编辑框必须等宽，否则同一栏里两行对不齐
	constexpr float kKeyBoxW{ 116.f };
}

WinSettingSystemKeys::WinSettingSystemKeys(Ling::WinBase* parent) : Ling::Node(parent)
{
	Ling::Node* titleRow = nullptr;
	SettingUi::sectionHeader(this, Lang::get(L"setting.systemKeys"),
		Lang::get(L"setting.systemKeysDesc"),
		Lang::get(L"setting.systemKeysTip"), &titleRow);
	// 标题行右端：恢复默认（删掉全部改绑 → 逐条回落命令表的默认键位）
	if (titleRow) {
		auto* spacer = titleRow->makeChild<Ling::Node>();
		spacer->setFlexGrow(1.f);
		spacer->setFlexShrink(1.f);
		restoreBtn = SettingUi::btnOutline(titleRow, Lang::get(L"setting.restoreDefault"), [this]() {
			if (!CommandKeys::anyCustomBinding()) return;   // 本来就是默认键位
			this->endCapture();          // 正在录制键位时先收尾（清掉转圈、恢复热键）
			CommandKeys::resetBindings();
			for (auto& row : this->rows) this->refreshRow(row.id);
			this->syncRestoreState();
		});
		restoreBtn->setMarginLeft(SettingTheme::sp3);
	}

	// 分组与组内顺序完全由命令表决定（不做第二份清单）
	for (const auto& gkey : CommandKeys::groupOrder()) {
		SettingUi::groupLabel(this, Lang::get(gkey));
		// 本页一律双栏：两两一行，每格 = 与「全局热键」页同款的行（标签 + 键位框）
		auto* grid = SettingUi::grid2(this);
		for (const auto& id : CommandKeys::idsInGroup(gkey)) {
			const std::wstring chord = CommandKeys::binding(id);
			const bool ro = CommandKeys::readOnly(id);
			const bool impl = CommandKeys::implemented(id);
			// 只读提示行 / 未实装行：键位按钮不可点、不可录
			const bool editable = impl && !ro;

			auto r = SettingUi::optionRow(grid, Lang::get(CommandKeys::labelKey(id)), L"", true);
			r.row->setHeight(SettingTheme::hotkeyRowH);
			// 与热键页同一套行内边距：按右侧控件高度取上下留白
			r.row->setPadding(SettingTheme::sp4, 0,
				(SettingTheme::hotkeyRowH - SettingTheme::ctrlH) * 0.5f, 0);

			// 只读 / 未实装行：键位框换成只读展示框（不给手型、不可点），行名称保持正常色
			if (!editable) {
				SettingUi::staticBox(r.ctrlBox, chordDisplay(chord), kKeyBoxW);
				continue;
			}

			// 键位框：与「全局热键」页的快捷键按钮同款（白底 + 描边 + 圆角 + 定宽定高）
			auto* key = r.ctrlBox->makeChild<Ling::Button>();
			key->setId(id);
			key->setHeight(SettingTheme::ctrlH);
			key->setWidth(kKeyBoxW);
			key->setBorderRadius(SettingTheme::radiusCtl);
			styleKeyBtn(key, false);
			key->setText(chordDisplay(chord));
			// 未绑定的行：占位文案走 muted（与「点击设置」同色）
			if (chord.empty()) key->setColor(SettingTheme::textMuted);
			key->onClick.add([this](Ling::Button* b) { this->onBtnClick(b); });
			rows.push_back({ id, key });
		}
		SettingUi::grid2Cells(grid);
	}
	SettingUi::trimTrailingGap(this);

	// —— 录制交互：与「全局热键」页逐条对齐（点框录制 / 点别处或失焦取消）——
	auto weakThis = getWeakThis();
	onKeyDownToken = win->onKeyDown.add([this, weakThis](UINT key) {
		if (!weakThis.lock()) return;
		if (this->curId.empty()) return;
		this->onKeyDown(key);
		});
	// 松开修饰键：录制还没结束，把按钮里的预览退回（或退到剩余仍按住的修饰键）
	onKeyUpToken = win->onKeyUp.add([this, weakThis](UINT) {
		if (!weakThis.lock()) return;
		if (this->curId.empty()) return;
		this->refreshCapturePreview();
		});
	onMouseDownToken = win->onMouseDown.add([this, weakThis](POINT pos, bool) {
		if (!weakThis.lock()) return;
		if (this->curId.empty()) return;
		if (this->curBtn && this->curBtn->isPosIn(pos)) return;
		this->endCapture();
		});
	onTimerToken = win->onTimer.add([this, weakThis](UINT id) {
		if (!weakThis.lock()) return;
		this->onSpinnerTick(id);
		});
	onBlurToken = win->onBlur.add([this, weakThis]() {
		if (!weakThis.lock()) return;
		if (this->curId.empty()) return;
		this->endCapture();
		});
	// 改绑发生在键位框自己的录制流程里，不在本函数建的那些行上触发重建：
	// 「恢复默认」的置灰借鼠标移动这个现成的高频回调同步（值没变时 setColor 不产生重绘）
	onMouseMoveToken = win->onMouseMove.add([this, weakThis](POINT) {
		if (!weakThis.lock()) return;
		this->syncRestoreState();
		});
	syncRestoreState();
}

void WinSettingSystemKeys::syncRestoreState()
{
	if (!restoreBtn) return;
	const bool grey = !CommandKeys::anyCustomBinding();
	restoreBtn->setColor(grey ? SettingTheme::textMuted : SettingTheme::textPrimary);
	restoreBtn->setHoverColor(grey ? SettingTheme::textMuted : SettingTheme::textPrimary);
}

WinSettingSystemKeys::~WinSettingSystemKeys()
{
	// 页面在录制中途被销毁（切走/关窗）时，热键仍是挂起状态，这里兜底恢复
	if (!curId.empty()) {
		HotkeyHook::endCapture();
		Setting::get()->resumeShortcutKeys();
	}
	win->killTimer(kSpinnerTimerId);
	win->onMouseMove.remove(onMouseMoveToken);
	win->onMouseDown.remove(onMouseDownToken);
	win->onKeyDown.remove(onKeyDownToken);
	win->onKeyUp.remove(onKeyUpToken);
	win->onTimer.remove(onTimerToken);
	win->onBlur.remove(onBlurToken);
}

void WinSettingSystemKeys::refreshRow(const std::wstring& id)
{
	const std::wstring chord = CommandKeys::binding(id);
	const std::wstring text = chordDisplay(chord);
	for (auto& row : rows) {
		if (row.id != id) continue;
		row.btn->setText(text);
		const uint32_t color = chord.empty() ? SettingTheme::textMuted : SettingTheme::textPrimary;
		row.btn->setColor(color);
		row.btn->setHoverColor(color);
		return;
	}
}

void WinSettingSystemKeys::onBtnClick(Ling::Button* btn)
{
	if (curBtn == btn) { endCapture(); return; }
	if (!curId.empty()) endCapture();
	beginCapture(btn);
}

void WinSettingSystemKeys::beginCapture(Ling::Button* btn)
{
	curBtn = btn;
	curId = btn->id;
	// 清空按钮内置文本，改用自定义「转圈 + 文案」两个子节点（转圈落于文案左侧）
	btn->setText(L"");
	styleKeyBtn(btn, true);
	// Button 默认 flexDirection 为 column：改为 row 后 spinner 居左、label 居右
	btn->setFlexDirection(Ling::FlexDirection::Row);

	curSpinner = btn->makeChild<Ling::Label>();
	curSpinner->setText(kSpinnerFrames[0]);
	curSpinner->setFontFamily(Icon::Family);
	curSpinner->setFontSize(16.f);
	curSpinner->setColor(kCaptureFg);   // 绿底上白转圈
	curSpinner->setMarginRight(SettingTheme::sp2);

	curLabel = btn->makeChild<Ling::Label>();
	curLabel->setText(Lang::get(L"shortcut.pressKey"));
	curLabel->setFontSize(SettingTheme::fontBase);
	curLabel->setColor(kCaptureFg);   // 绿底上白字
	refreshCapturePreview();          // 点按钮时若已按住修饰键，立刻显示出来

	// 录制期间挂起全局热键：否则按下的组合键会先被全局热键（RegisterHotKey / 低级钩子）
	// 消费，传不到设置窗口，导致无法录制。
	Setting::get()->suspendShortcutKeys();

	// 再挂一个临时低级键盘钩子接管键盘输入：已被其他进程注册的全局热键（如 F3）
	// 会在系统热键分发阶段被消费；录制期间由这里先行消费所有按键再转回本页
	auto weakThis = getWeakThis();
	HotkeyHook::beginCapture([this, weakThis](UINT vk, bool up) {
		if (!weakThis.lock() || curId.empty()) return;
		if (up) { refreshCapturePreview(); return; }   // 松开修饰键：预览退回
		onKeyDown(vk);
	});

	spinIdx = 0;
	win->setTimer(kSpinnerMs, kSpinnerTimerId);
}

void WinSettingSystemKeys::endCapture()
{
	if (curId.empty()) return;
	win->killTimer(kSpinnerTimerId);
	if (curBtn) {
		if (curSpinner) { curBtn->removeChild(curSpinner); curSpinner = nullptr; }
		if (curLabel) { curBtn->removeChild(curLabel);   curLabel = nullptr; }
		styleKeyBtn(curBtn, false);   // 结束录制：恢复常态配色
		refreshRow(curId);                  // 显示"已落盘"的值（冲突未确认时 = 原值）
	}
	curBtn = nullptr;
	curId.clear();
	// 先撤录制钩子，再恢复全局热键（顺序反了会让重装后的钩子仍处于消费所有按键的状态）
	HotkeyHook::endCapture();
	// 录制结束：按挂起前的键位恢复全局热键
	Setting::get()->resumeShortcutKeys();
}

void WinSettingSystemKeys::onSpinnerTick(UINT id)
{
	if (id != kSpinnerTimerId) return;
	if (!curSpinner) return;
	spinIdx = (spinIdx + 1) % kSpinnerFramesCount;
	curSpinner->setText(kSpinnerFrames[spinIdx]);
}

void WinSettingSystemKeys::refreshCapturePreview()
{
	if (!curLabel) return;
	const std::wstring prefix = liveModifierPrefix();
	curLabel->setText(prefix.empty() ? Lang::get(L"shortcut.pressKey") : prefix);
}

void WinSettingSystemKeys::onKeyDown(UINT key)
{
	// Esc = 退出这次录制：不落盘、保持原键位（Esc 本身也不参与录制）
	if (key == VK_ESCAPE) {
		endCapture();
		return;
	}
	// 主键按下即完成录制：不再等抬起。否则组合还没按全时先松任意一个修饰键
	//（或改主意换修饰键）就会被当成"松手保存"，三键组合经常录不进去。
	auto keyStr = keyToStr(key);
	if (keyStr.empty()) return;
	// 只按修饰键：立即把已按住的修饰键显示出来，继续等主键（纯修饰键不落盘）
	if (isModifierKey(keyStr)) {
		refreshCapturePreview();
		return;
	}

	const bool ctrl  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
	const bool alt   = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
	const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
	const bool lwin  = (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0;
	const bool rwin  = (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;

	// 裸 Backspace / Delete = 清空这一行（带修饰键时仍按普通组合录制）
	if ((key == VK_BACK || key == VK_DELETE) && !ctrl && !alt && !shift && !lwin && !rwin) {
		const std::wstring id = curId;
		CommandKeys::setBinding(id, L"");
		endCapture();            // 内部 refreshRow 会显示清空后的「点击设置」占位
		return;
	}

	// 组合取"主键按下这一刻"按住的修饰键：与按下/松开的先后顺序无关
	std::vector<std::wstring> keys;
	if (ctrl) keys.push_back(L"Ctrl");
	if (alt) keys.push_back(L"Alt");
	if (shift) keys.push_back(L"Shift");
	if (lwin) keys.push_back(L"LWin");
	else if (rwin) keys.push_back(L"RWin");
	keys.push_back(keyStr);

	const std::wstring chord = joinKeys(keys);
	// 最多三键（两个修饰键 + 一个主键）；不合法就丢弃，保持原键位
	const bool ok = keys.size() <= 3 && isValidKeys(keys);
	// ⚠ 先收尾再落值：按钮在判定完成前必须回到原值（冲突被取消 = 什么都不变）
	const std::wstring id = curId;
	endCapture();
	if (!ok) return;

	std::wstring otherLabel, otherId;
	// 只读行占用的固定键不让其他命令改绑（也即不提供"覆盖"），直接拒绝并说明被哪条占用
	std::wstring ownerLabel;
	if (CommandKeys::reservedChord(chord, &ownerLabel)) {
		SettingUi::ConfirmDialog::ask(win,
			fillMessage(L"cmdkey.reserved", chord, Lang::get(ownerLabel)),
			[](bool) {});
		return;
	}
	if (CommandKeys::boundToOther(chord, id, &otherLabel, &otherId)) {
		// 与**同一界面**上其他命令撞键：先问「是否覆盖」，确认才解绑旧主再落值
		auto weakThis = getWeakThis();
		SettingUi::ConfirmDialog::ask(win,
			fillMessage(L"cmdkey.conflict", chord, Lang::get(otherLabel)),
			[this, weakThis, id, chord, otherId](bool yes) {
				if (!weakThis.lock() || !yes) return;
				CommandKeys::setBinding(otherId, L"");   // 先解绑旧主
				CommandKeys::setBinding(id, chord);
				refreshRow(otherId);
				refreshRow(id);
			},
			Lang::get(L"setting.confirm"), Lang::get(L"setting.cancel"));
		return;
	}
	CommandKeys::setBinding(id, chord);
	refreshRow(id);
}

std::wstring WinSettingSystemKeys::keyToStr(UINT vkCode)
{
	switch (vkCode) {
	case VK_CONTROL: return L"Ctrl";
	case VK_MENU:    return L"Alt";
	case VK_SHIFT:   return L"Shift";
	case VK_LWIN:    return L"LWin";
	case VK_RWIN:    return L"RWin";
	case VK_F1: return L"F1"; case VK_F2: return L"F2"; case VK_F3: return L"F3";
	case VK_F4: return L"F4"; case VK_F5: return L"F5"; case VK_F6: return L"F6";
	case VK_F7: return L"F7"; case VK_F8: return L"F8"; case VK_F9: return L"F9";
	case VK_F10: return L"F10"; case VK_F11: return L"F11"; case VK_F12: return L"F12";
	case VK_UP: return L"Up"; case VK_DOWN: return L"Down";
	case VK_LEFT: return L"Left"; case VK_RIGHT: return L"Right";
	case VK_RETURN:  return L"Enter";
	case VK_ESCAPE:  return L"Esc";
	case VK_TAB:     return L"Tab";
	case VK_SPACE:   return L"Space";
	case VK_BACK:    return L"Backspace";
	case VK_DELETE:  return L"Delete";
	case VK_INSERT:  return L"Insert";
	case VK_HOME:    return L"Home";
	case VK_END:     return L"End";
	case VK_PRIOR:   return L"PageUp";
	case VK_NEXT:    return L"PageDown";
	case VK_SNAPSHOT:return L"PrintScreen";
	case VK_SCROLL:  return L"ScrollLock";
	case VK_PAUSE:   return L"Pause";
	case VK_NUMLOCK: return L"NumLock";
	// 小键盘符号用 NumXxx 专名：与主键盘的 - = / . 分开，录制按的是哪个键就存哪个
	case VK_MULTIPLY: return L"Num*"; case VK_ADD: return L"NumAdd";
	case VK_SUBTRACT: return L"Num-"; case VK_DIVIDE: return L"Num/";
	case VK_DECIMAL:  return L"Num.";
	default:
		if (vkCode >= 'A' && vkCode <= 'Z') return std::wstring(1, static_cast<wchar_t>(vkCode));
		if (vkCode >= '0' && vkCode <= '9') return std::wstring(1, static_cast<wchar_t>(vkCode));
		if (vkCode >= VK_NUMPAD0 && vkCode <= VK_NUMPAD9)
			return L"Num" + std::to_wstring(vkCode - VK_NUMPAD0);
		if (vkCode == VK_OEM_3 || vkCode == VK_OEM_1 || vkCode == VK_OEM_4 ||
			vkCode == VK_OEM_6 || vkCode == VK_OEM_7 || vkCode == VK_OEM_5 ||
			vkCode == VK_OEM_2 || vkCode == VK_OEM_COMMA || vkCode == VK_OEM_PERIOD ||
			vkCode == VK_OEM_MINUS || vkCode == VK_OEM_PLUS) {
			int result = MapVirtualKeyW(vkCode, MAPVK_VK_TO_CHAR);
			if (result != 0 && !(result & 0x80000000)) {
				wchar_t ch = static_cast<wchar_t>(result & 0xFFFF);
				return std::wstring(1, ch);
			}
		}
		return L"";
	}
}
