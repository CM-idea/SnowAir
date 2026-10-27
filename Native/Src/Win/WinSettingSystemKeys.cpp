#include "pch.h"
#include "../Lang.h"
#include "../Setting.h"
#include "../CommandKeys.h"
#include "WinSetting.h"
#include "WinSettingSystemKeys.h"
#include "SettingTheme.h"
#include "SettingWidgets.h"

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

	// 录到的这组键能否作为「系统按键」：允许不带修饰键的单键（默认值里就有 1..0 / W / Q / F /
	// ` / Space / Esc / Enter / Tab / Backspace）—— 这些命令只在会话窗口有焦点时派发，
	// 不占用其他程序的按键。要求恰好一个主键，且主键能被 strToKey 识别（否则落盘后无法命中）。
	bool isValidKeys(const std::vector<std::wstring>& keys)
	{
		if (keys.empty()) return false;
		unsigned mains = 0;
		for (const auto& k : keys) if (!KeyCapture::isModifierKey(k)) mains++;
		if (mains != 1) return false;
		return CommandKeys::validChord(KeyCapture::joinKeys(keys));
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
			this->capture_.end();          // 正在录制键位时先收尾（清掉转圈、恢复热键）
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

	// —— 录制交互：与「全局热键」页同款（点框录制 / 点别处或失焦取消）——
	// 录制过程统一交由 KeyCapture；本页只负责判定录制结果与写入配置。
	capture_.init(win, kSpinnerTimerId,
		[this](const std::wstring& id) { refreshRow(id); },       // 收尾后把框恢复成已落盘的值
		[this](const std::wstring& id, const std::vector<std::wstring>& keys) { applyChord(id, keys); },
		[this](const std::wstring& id) { CommandKeys::setBinding(id, L""); },
		[this](const std::vector<std::wstring>& keys) { return isValidKeys(keys); });

	auto weakThis = getWeakThis();
	onKeyDownToken = win->onKeyDown.add([this, weakThis](UINT key) {
		if (!weakThis.lock()) return;
		capture_.handleKey(key);
		});
	// 松开修饰键：录制还没结束，把按钮里的预览退回（或退到剩余仍按住的修饰键）
	onKeyUpToken = win->onKeyUp.add([this, weakThis](UINT) {
		if (!weakThis.lock()) return;
		capture_.handleKeyUp();
		});
	onMouseDownToken = win->onMouseDown.add([this, weakThis](POINT pos, bool) {
		if (!weakThis.lock()) return;
		capture_.handleMouseDown(pos);
		});
	onTimerToken = win->onTimer.add([this, weakThis](UINT id) {
		if (!weakThis.lock()) return;
		capture_.handleTimer(id);
		});
	onBlurToken = win->onBlur.add([this, weakThis]() {
		if (!weakThis.lock()) return;
		capture_.handleBlur();
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
	capture_.shutdown();
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
	capture_.begin(btn, btn->id, styleKeyBtn);
}

// 录制提交：只读行占用的固定键直接拒绝；与同一界面其他命令撞键则先问是否覆盖。
// 调用前 KeyCapture 已完成收尾（输入框恢复原值）；取消时不做任何修改。
void WinSettingSystemKeys::applyChord(const std::wstring& id, const std::vector<std::wstring>& keys)
{
	const std::wstring chord = KeyCapture::joinKeys(keys);
	std::wstring otherLabel, otherId;
	// 只读行占用的固定键不让其他命令改绑（也即不提供"覆盖"），直接拒绝并说明被哪条占用
	std::wstring ownerLabel;
	if (CommandKeys::reservedChord(chord, &ownerLabel)) {
		SettingUi::ConfirmDialog::ask(win,
			KeyCapture::fillMessage(L"cmdkey.reserved", chord, Lang::get(ownerLabel)),
			[](bool) {});
		return;
	}
	if (CommandKeys::boundToOther(chord, id, &otherLabel, &otherId)) {
		// 与同一界面上其他命令撞键：先问「是否覆盖」，确认才解绑旧主再落值
		auto weakThis = getWeakThis();
		SettingUi::ConfirmDialog::ask(win,
			KeyCapture::fillMessage(L"cmdkey.conflict", chord, Lang::get(otherLabel)),
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
