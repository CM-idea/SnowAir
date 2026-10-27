#include "pch.h"
#include "../Lang.h"
#include "../Setting.h"
#include "WinSetting.h"
#include "WinSettingHotkeys.h"
#include "SettingTheme.h"
#include "SettingWidgets.h"
#include <algorithm>
#include <cctype>

namespace {
    // 不配修饰键、单独按下也能当快捷键的那一类（F1~F12、PrintScreen、ScrollLock、Pause）
    bool canBeUsedAlone(const std::wstring& key)
    {
        if (key.size() >= 2 && key[0] == L'F') {
            auto num = key.substr(1);
            auto isDigit = [](wchar_t c) { return c >= L'0' && c <= L'9'; };
            if (std::all_of(num.begin(), num.end(), isDigit)) return true;
        }
        return key == L"PrintScreen" || key == L"ScrollLock" || key == L"Pause";
    }

    // 全局热键的「保留键」分两类，处理方式不同：
    //   System：Windows 保留（任务切换 / 锁定 / 任务管理器…），注册不会成功 → 直接拒绝并说明原因；
    //   Common：编辑 / 保存类通用组合，注册后会对所有程序的同名快捷键生效 → 先警告、由用户决定。
    enum class ShortcutReserved { None, System, Common };
    ShortcutReserved reservedKindOf(const std::wstring& shortcut)
    {
        static const std::vector<std::wstring> system{
            L"Ctrl+Alt+Delete", L"Ctrl+Shift+Esc", L"Ctrl+Esc",
            L"Alt+Tab", L"Alt+Esc", L"Alt+F4",
            L"Win+L", L"Win+D", L"Win+E", L"Win+R", L"Win+Tab",
        };
        static const std::vector<std::wstring> common{
            L"Ctrl+C", L"Ctrl+V", L"Ctrl+X", L"Ctrl+Z", L"Ctrl+Y", L"Ctrl+A", L"Ctrl+S",
        };
        if (std::find(system.begin(), system.end(), shortcut) != system.end())
            return ShortcutReserved::System;
        if (std::find(common.begin(), common.end(), shortcut) != common.end())
            return ShortcutReserved::Common;
        return ShortcutReserved::None;
    }

    // 组合键规范化比较：拆成"修饰键位 + 主键"，与键名大小写、修饰键书写顺序无关。
    // 用于冲突判定 —— "Shift+Ctrl+A" 与 "Ctrl+Shift+A" 是同一把键，不能当成两条绑上去。
    bool sameChord(const std::wstring& a, const std::wstring& b)
    {
        auto split = [](const std::wstring& chord, unsigned& mods, UINT& key) {
            mods = 0;
            key = 0;
            std::wstring lower = chord;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
            for (auto& part : Ling::Util::splitStr(lower, L'+')) {
                if (part == L"ctrl") mods |= MOD_CONTROL;
                else if (part == L"alt") mods |= MOD_ALT;
                else if (part == L"shift") mods |= MOD_SHIFT;
                else if (part == L"win" || part == L"lwin" || part == L"rwin") mods |= MOD_WIN;
                else key = Ling::Util::strToKey(part);
            }
            return key != 0;
        };
        unsigned ma = 0, mb = 0;
        UINT ka = 0, kb = 0;
        if (!split(a, ma, ka) || !split(b, mb, kb)) return false;
        return ma == mb && ka == kb;
    }

    bool isValidShortcutKeys(const std::vector<std::wstring>& keys)
    {
        bool hasModifier{ false };
        std::wstring normalKey;
        for (const auto& key : keys) {
            if (KeyCapture::isModifierKey(key)) {
                hasModifier = true;
                continue;
            }
            if (!normalKey.empty()) return false;
            normalKey = key;
        }
        if (normalKey.empty()) return false;
        if (!hasModifier && !canBeUsedAlone(normalKey)) return false;
        std::wstring lowerKey = normalKey;
        std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(), ::towlower);
        // 保留键不在这里拦：由 onKeyDown 按 System / Common 分别提示（见 reservedKindOf）
        return Ling::Util::strToKey(lowerKey) != 0;
    }

    // 暂缓启用的全局热键：设置页保留这一行但**置灰不可点**，
    // 免得出现"能录入、能保存、按下去却不生效"的迷惑行为。
    bool isPendingHotkey(const std::wstring& id)
    {
        return id == L"quick_translate" || id == L"translate_selection"
            || id == L"open_clipboard";
    }

    // 每一项的「?」说明（目前只有快速贴屏需要；其余行不给问号）
    std::wstring hotkeyHelpTipOf(const std::wstring& id)
    {
        return id == L"quick_pin" ? Lang::get(L"shortcut.quickPinTip") : std::wstring{};
    }

    // 快捷键按钮文字与颜色：未绑定时显示「点击设置」占位（muted 色），已绑定显示组合键。
    void applyShortcutLabel(Ling::Button* btn, const std::wstring& id)
    {
        auto key = Setting::get()->getShortcutKey(id);
        btn->setText(key.empty() ? Lang::get(L"shortcut.setToBind") : key);
        btn->setColor(key.empty() ? SettingTheme::textMuted : SettingTheme::textPrimary);
    }

    // 快捷键按钮配色：
    //   常态 = 白底 + 1px 描边 + 深色文字（文字色最终由 applyShortcutLabel 决定）
    //   录制中（点了按钮在等按键）= #0FDC78 底 + 白字/白转圈，去掉描边，且悬停不产生任何变化
    constexpr uint32_t kHotkeyCaptureBg{ 0x0FDC78FF };
    constexpr uint32_t kHotkeyCaptureFg{ 0xFFFFFFFF };
    void styleHotkeyBtn(Ling::Button* btn, bool capturing)
    {
        if (capturing) {
            btn->setBg(kHotkeyCaptureBg);
            btn->setHoverBg(kHotkeyCaptureBg);   // 悬停取同值 → 无停悬效果
            btn->setBorder(0.f, 0);
            btn->setColor(kHotkeyCaptureFg);
            btn->setHoverColor(kHotkeyCaptureFg);
        } else {
            btn->setBg(SettingTheme::inputBg);
            btn->setHoverBg(SettingTheme::accent);
            btn->setBorder(1.f, SettingTheme::input);
            btn->setColor(SettingTheme::textPrimary);
            btn->setHoverColor(SettingTheme::textPrimary);
        }
    }
}

WinSettingHotkeys::WinSettingHotkeys(Ling::WinBase* parent) :Ling::Node(parent)
{
    Ling::Node* titleRow = nullptr;
    SettingUi::sectionHeader(this, Lang::get(L"setting.hotkeys"),
        Lang::get(L"setting.hotkeysDesc"), Lang::get(L"setting.hotkeysTip"), &titleRow);
    // 标题行右端：恢复默认（清掉用户设过的键位 → 逐条回落内置默认表，并整体重注册）
    if (titleRow) {
        auto* spacer = titleRow->makeChild<Ling::Node>();
        spacer->setFlexGrow(1.f);
        spacer->setFlexShrink(1.f);
        restoreBtn = SettingUi::btnOutline(titleRow, Lang::get(L"setting.restoreDefault"), [this]() {
            if (Setting::get()->shortcutKeyAtDefaults()) return;   // 本来就是出厂键位
            this->capture_.end();          // 正在录制键位时先收尾（清掉转圈、恢复全局热键）
            Setting::get()->restoreShortcutKeyDefaults();
            for (auto* b : this->btns) applyShortcutLabel(b, b->id);
            this->syncRestoreState();
        });
        restoreBtn->setMarginLeft(SettingTheme::sp3);
    }

    // 4 分组 14 项（AI 对话组已随 AI 功能一并移除）
    // 注：滚动已由外层 WinSetting 的内容 ScrollerBox 统一管理，本页不再自建滚动容器。
    struct Group { std::wstring title; std::vector<std::pair<std::wstring, std::wstring>> items; };
    const Group groups[] = {
        // 0 截图组 6 项
        {Lang::get(L"setting.hotkeyGrpCapture"),{
            { L"capture",               Lang::get(L"shortcut.capture") },
            { L"record",                Lang::get(L"shortcut.record") },
            { L"fullscreen_canvas",     Lang::get(L"shortcut.fullscreen_canvas") },
            { L"capture_fullscreen",    Lang::get(L"shortcut.capture_fullscreen") },
            { L"capture_repeat",        Lang::get(L"shortcut.capture_repeat") },
            { L"capture_focused_window",Lang::get(L"shortcut.capture_focused_window") },
        }},
        // 1 剪切板组 2 项（紧跟在截图下面）：快速贴屏已实现（Beta），「打开剪切板」暂缓
        {Lang::get(L"setting.hotkeyGrpClipboard"),{
            { L"quick_pin",             Lang::get(L"shortcut.quick_pin") + L" Beta" },
            { L"open_clipboard",        Lang::get(L"shortcut.open_clipboard") },
        }},
        // 2 翻译组 2 项
        {Lang::get(L"setting.hotkeyGrpTranslate"),{
            { L"quick_translate",       Lang::get(L"shortcut.quick_translate") },
            { L"translate_selection",   Lang::get(L"shortcut.translate_selection") },
        }},
        // 3 其他组 4 项
        {Lang::get(L"setting.hotkeyGrpOther"),{
            { L"show_main",             Lang::get(L"shortcut.show_main") },
            { L"open_screenshot_dir",   Lang::get(L"shortcut.open_screenshot_dir") },
            { L"open_history",          Lang::get(L"shortcut.open_history") },
            { L"toggle_pins",           Lang::get(L"shortcut.toggle_pins") },
        }},
    };

    // 「停用截图历史」开启时，隐藏「打开截图历史」这一行的键位设置
    for (const auto& g : groups) {
        std::vector<std::pair<std::wstring, std::wstring>> items;
        for (const auto& it : g.items) {
            if (it.first == L"open_history" && Setting::get()->getDisableCaptureHistory()) continue;
            items.push_back(it);
        }
        buildGroup(g.title, items, this);
    }

    // 内容区只有外层 content 提供底部留白(20)：清掉末尾 optionRow 的 marginBottom，
    // 使底部间距与左右一致。
    SettingUi::trimTrailingGap(this);

    // 录制交互统一交由 KeyCapture；本页只负责校验录制结果、写入配置与冲突提示。
    capture_.init(win, kSpinnerTimerId,
        [this](const std::wstring& id) {                 // 收尾后把该框恢复成已落盘的值
            for (auto* b : btns) {
                if (b->id == id) { applyShortcutLabel(b, id); break; }
            }
        },
        [this](const std::wstring& id, const std::vector<std::wstring>& keys) {
            commitChord(id, keys);
        },
        [this](const std::wstring& id) {
            Setting::get()->setShortcutKey(id, {});
        },
        [this](const std::vector<std::wstring>& keys) {
            return isValidShortcutKeys(keys);
        });

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
    // 等待按键期间窗口失焦（前台切换到其他进程）→ 直接取消这次录制，不留悬挂状态。
    onBlurToken = win->onBlur.add([this, weakThis]() {
        if (!weakThis.lock()) return;
        capture_.handleBlur();
        });
    // 键位改动发生在各按钮自己的录制流程里、不触发本页重建：「恢复默认」的置灰借
    // 鼠标移动这个现成的高频回调同步（值没变时 setColor 不产生多余重绘）
    onMouseMoveToken = win->onMouseMove.add([this, weakThis](POINT) {
        if (!weakThis.lock()) return;
        this->syncRestoreState();
        });
    syncRestoreState();
}

void WinSettingHotkeys::syncRestoreState()
{
    if (!restoreBtn) return;
    const bool grey = Setting::get()->shortcutKeyAtDefaults();
    restoreBtn->setColor(grey ? SettingTheme::textMuted : SettingTheme::textPrimary);
    restoreBtn->setHoverColor(grey ? SettingTheme::textMuted : SettingTheme::textPrimary);
}

void WinSettingHotkeys::buildGroup(const std::wstring& grpTitle,
    const std::vector<std::pair<std::wstring, std::wstring>>& items, Ling::Node* p)
{
    // 分组标题：独立加粗文本（无卡片背景）
    SettingUi::groupLabel(p, grpTitle);

    // 每个选项为独立的浅灰圆角卡片行，行间由 optionGap 自然留出间距
    for (size_t i = 0; i < items.size(); i++) {
        const auto& it = items[i];
        // 行标题后的「?」说明：只有快速贴屏给了文案，其余行为空 → 不显示问号
        auto r = SettingUi::optionRow(p, it.second, L"", true,
            SettingTheme::ctrlH, hotkeyHelpTipOf(it.first));
        // 样式与其余设置页统一：白底 + 描边（去掉旧的浅灰底覆盖）；卡片高 50
        r.row->setHeight(SettingTheme::hotkeyRowH);
        // 行高改过之后，右侧内边距要跟着重算：与按钮的上下留白 (50-32)/2 取齐
        r.row->setPadding(SettingTheme::sp4, 0,
                          (SettingTheme::hotkeyRowH - SettingTheme::ctrlH) * 0.5f, 0);

        // 暂缓实现的项：整行置灰，值框显示「功能开发中」且不响应点击、也不参与录制。
        // 值框用只读展示框：它不是可点的东西，Button 悬停会给手型是误导
        if (isPendingHotkey(it.first)) {
            SettingUi::staticBox(r.ctrlBox, Lang::get(L"cap.comingSoon"), 160.f);
            if (r.label) r.label->setColor(SettingTheme::textMuted);
            continue;
        }

        auto* btn = r.ctrlBox->makeChild<Ling::Button>();
        btn->setId(it.first);
        btn->setHeight(SettingTheme::ctrlH);
        btn->setWidth(160.f);
        btn->setBorderRadius(SettingTheme::radiusCtl);   // 与所在行的外框(10)同心
        // 顺序：先定底色与悬停色、再定文字色。styleHotkeyBtn 会把文字色写为 textPrimary，
        // 若放在 applyShortcutLabel 之后，未绑定行的灰色占位会被冲掉
        styleHotkeyBtn(btn, false);          // 常态配色（白底 + 描边 + 悬停底色）
        applyShortcutLabel(btn, it.first);   // 文字色最后定：未绑定 = 灰
        btn->onClick.add([this](Ling::Button* b) { this->onBtnClick(b); });
        btns.push_back(btn);
        labelById.push_back({ it.first, it.second });
    }
}

WinSettingHotkeys::~WinSettingHotkeys()
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

void WinSettingHotkeys::onBtnClick(Ling::Button* btn)
{
    capture_.begin(btn, btn->id, styleHotkeyBtn);
}

// 录制提交：先处理 Windows 保留键 / 通用编辑键，再进入同页撞键判定。
// 调用前 KeyCapture 已完成收尾（按钮恢复原值）；取消时不做任何修改。
void WinSettingHotkeys::commitChord(const std::wstring& id, const std::vector<std::wstring>& keys)
{
    const std::wstring chord = KeyCapture::joinKeys(keys);
    // Windows 保留的组合：注册无法成功，直接说明原因（不再静默丢弃）
    if (reservedKindOf(chord) == ShortcutReserved::System) {
        SettingUi::ConfirmDialog::ask(win,
            KeyCapture::fillMessage(L"shortcut.reservedSystem", chord, L""),
            [](bool) {}, Lang::get(L"setting.confirm"), L"", true);
        return;
    }
    // 通用编辑 / 保存键：可以设置，但会对所有程序的同名快捷键生效 → 先警告，由用户决定
    if (reservedKindOf(chord) == ShortcutReserved::Common) {
        auto weakThis = getWeakThis();
        SettingUi::ConfirmDialog::ask(win,
            KeyCapture::fillMessage(L"shortcut.reservedCommon", chord, L""),
            [this, weakThis, id, keys](bool yes) {
                if (!weakThis.lock() || !yes) return;
                this->applyShortcut(id, keys);
            },
            Lang::get(L"setting.confirm"), Lang::get(L"setting.cancel"));
        return;
    }
    applyShortcut(id, keys);
}

// 落值：先判同页撞键（确认后解绑旧主），再写入并刷新按钮文字。
// 录制已完成收尾，此处只负责数据与显示。
void WinSettingHotkeys::applyShortcut(const std::wstring& id, const std::vector<std::wstring>& keys)
{
    const std::wstring chord = KeyCapture::joinKeys(keys);
    const std::wstring otherId = conflictOwner(chord, id);
    if (otherId.empty()) {
        Setting::get()->setShortcutKey(id, keys);
        for (auto* b : this->btns) applyShortcutLabel(b, b->id);
        return;
    }
    auto weakThis = getWeakThis();
    const std::wstring otherLabel = labelOf(otherId);
    SettingUi::ConfirmDialog::ask(win,
        KeyCapture::fillMessage(L"cmdkey.conflict", chord, otherLabel),
        [this, weakThis, id, otherId, keys](bool yes) {
            if (!weakThis.lock() || !yes) return;
            Setting::get()->setShortcutKey(otherId, {});   // 先解绑旧主
            Setting::get()->setShortcutKey(id, keys);
            for (auto* b : this->btns) applyShortcutLabel(b, b->id);
        },
        Lang::get(L"setting.confirm"), Lang::get(L"setting.cancel"));
}

std::wstring WinSettingHotkeys::conflictOwner(const std::wstring& chord, const std::wstring& selfId) const
{
    if (chord.empty()) return {};
    for (auto* b : btns) {
        if (b->id == selfId) continue;
        if (sameChord(Setting::get()->getShortcutKey(b->id), chord)) return b->id;
    }
    return {};
}

std::wstring WinSettingHotkeys::labelOf(const std::wstring& id) const
{
    for (const auto& it : labelById) {
        if (it.first == id) return it.second;
    }
    return id;
}

