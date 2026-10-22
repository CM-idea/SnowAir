#include "pch.h"
#include "../Lang.h"
#include "../Setting.h"
#include "WinSetting.h"
#include "WinSettingHotkeys.h"
#include "SettingTheme.h"
#include "SettingWidgets.h"
#include "HotkeyHook.h"
#include "../Tool/IconCodes.h"
#include <algorithm>
#include <cctype>

namespace {
    bool isShortcutModifierKey(const std::wstring& key)
    {
        return key == L"Ctrl" || key == L"Alt" || key == L"Shift" || key == L"Win" || key == L"LWin" || key == L"RWin";
    }

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

    std::wstring joinShortcutKeys(const std::vector<std::wstring>& keys)
    {
        std::wstring result;
        for (const auto& key : keys) {
            if (!result.empty()) result += L"+";
            result += (key == L"LWin" || key == L"RWin") ? L"Win" : key;
        }
        return result;
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

    // 提示文案填充：%1 = 键位、%2 = 占用者（没有 %2 的文案传空即可）
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

    bool isValidShortcutKeys(const std::vector<std::wstring>& keys)
    {
        bool hasModifier{ false };
        std::wstring normalKey;
        for (const auto& key : keys) {
            if (isShortcutModifierKey(key)) {
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
        return id == L"quick_translate" || id == L"translate_selection";
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

    // 转圈圈帧：新图标字体「加载1~加载8」共 8 帧顺时针轮换，营造加载感。
    const wchar_t* kSpinnerFrames[] = {
        Icon::Loading1, Icon::Loading2, Icon::Loading3, Icon::Loading4,
        Icon::Loading5, Icon::Loading6, Icon::Loading7, Icon::Loading8,
    };
    constexpr int kSpinnerFramesCount = 8;
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
            this->endCapture();          // 正在录制键位时先收尾（清掉转圈、恢复全局热键）
            Setting::get()->restoreShortcutKeyDefaults();
            for (auto* b : this->btns) applyShortcutLabel(b, b->id);
            this->syncRestoreState();
        });
        restoreBtn->setMarginLeft(SettingTheme::sp3);
    }

    // 3 分组 12 项（AI 对话组已随 AI 功能一并移除）
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
        // 1 翻译组 2 项
        {Lang::get(L"setting.hotkeyGrpTranslate"),{
            { L"quick_translate",       Lang::get(L"shortcut.quick_translate") },
            { L"translate_selection",   Lang::get(L"shortcut.translate_selection") },
        }},
        // 2 其他组 4 项
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

    auto weakThis = getWeakThis();
    onKeyDownToken = win->onKeyDown.add([this, weakThis](UINT key) {
        if (!weakThis.lock()) return;
        if (this->curKey.empty()) return;
        this->onKeyDown(key);
        });
    // 松开修饰键：录制还没结束，把按钮里的预览退回（或退到剩余仍按住的修饰键）
    onKeyUpToken = win->onKeyUp.add([this, weakThis](UINT) {
        if (!weakThis.lock()) return;
        if (this->curKey.empty()) return;
        this->refreshCapturePreview();
        });
    onMouseDownToken = win->onMouseDown.add([this, weakThis](POINT pos, bool) {
        if (!weakThis.lock()) return;
        if (this->curKey.empty()) return;
        for (auto btn : this->btns)
            if (btn->isPosIn(pos)) return;
        this->endCapture();
        });
    onTimerToken = win->onTimer.add([this, weakThis](UINT id) {
        if (!weakThis.lock()) return;
        this->onSpinnerTick(id);
        });
    // 等待按键期间窗口失焦（前台切换到其他进程）→ 直接取消这次录制，不留悬挂状态。
    onBlurToken = win->onBlur.add([this, weakThis]() {
        if (!weakThis.lock()) return;
        if (this->curKey.empty()) return;
        this->endCapture();
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
        auto r = SettingUi::optionRow(p, it.second, L"", true);
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
    if (!curKey.empty()) {
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

void WinSettingHotkeys::onBtnClick(Ling::Button* btn)
{
    if (curKey == btn->id) { endCapture(); return; }
    if (!curKey.empty()) endCapture();
    beginCapture(btn);
}

void WinSettingHotkeys::beginCapture(Ling::Button* btn)
{
    curKey = btn->id;
    // 清空按钮内置文本，改用自定义"转圈 + 文案"两个子节点。
    // 原因：Button 内置 Text 是 flex 首个子节点、无法用 makeChild 插到转圈左侧，
    // 故先置空内置文本，再用 (spinner + label) 作为一组居中摆放，转圈自然落于文案左侧。
    btn->setText(L"");
    styleHotkeyBtn(btn, true);   // 录制中：#0FDC78 底 + 白字（无描边、无悬停变化）
    // Button 默认 flexDirection 为 column（纵向），子节点会上下堆叠 → 转圈压在文案上方。
    // 改为 row 后 spinner 居左、label 居右，水平排列（Button 构造已设 justify/align 居中）。
    btn->setFlexDirection(Ling::FlexDirection::Row);

    curSpinner = btn->makeChild<Ling::Label>();
    curSpinner->setText(kSpinnerFrames[0]);
    curSpinner->setFontFamily(Icon::Family);
    curSpinner->setFontSize(16.f);
    curSpinner->setColor(kHotkeyCaptureFg);   // 绿底上白转圈
    curSpinner->setMarginRight(SettingTheme::sp2);

    curLabel = btn->makeChild<Ling::Label>();
    curLabel->setText(Lang::get(L"shortcut.pressKey"));
    curLabel->setFontSize(SettingTheme::fontBase);
    curLabel->setColor(kHotkeyCaptureFg);   // 绿底上白字
    refreshCapturePreview();                // 点按钮时若已按住修饰键，立刻显示出来

    // 录制期间挂起全局热键：否则按下的组合键会先被全局热键（RegisterHotKey / 低级钩子）
    // 消费，传不到设置窗口，导致无法录制。
    Setting::get()->suspendShortcutKeys();

    // 再挂一个临时低级键盘钩子接管键盘输入：目标组合可能已被其他进程注册为全局热键，
    // 按键会在系统热键分发阶段被消费，设置窗口收不到 WM_KEYDOWN。录制期间由这里
    // 先行消费所有按键（含系统快捷键），再转回本页。
    auto weakThis = getWeakThis();
    HotkeyHook::beginCapture([this, weakThis](UINT vk, bool up) {
        if (!weakThis.lock() || curKey.empty()) return;
        if (up) { refreshCapturePreview(); return; }   // 松开修饰键：预览退回
        onKeyDown(vk);
    });

    spinIdx = 0;
    win->setTimer(kSpinnerMs, kSpinnerTimerId);
}

void WinSettingHotkeys::endCapture()
{
    if (curKey.empty()) return;
    win->killTimer(kSpinnerTimerId);
    for (auto& btn : btns) {
        if (btn->id == curKey) {
            if (curSpinner) { btn->removeChild(curSpinner); curSpinner = nullptr; }
            if (curLabel)   { btn->removeChild(curLabel);   curLabel = nullptr; }
            styleHotkeyBtn(btn, false);          // 结束录制：恢复常态配色
            applyShortcutLabel(btn, curKey);
            break;
        }
    }
    curKey.clear();
    // 先撤录制钩子，再恢复全局热键：顺序反了会让重装后的钩子仍处于消费所有按键的状态
    HotkeyHook::endCapture();
    // 录制结束（保存或取消）：按挂起前的组合键恢复全局热键
    Setting::get()->resumeShortcutKeys();
}

void WinSettingHotkeys::onSpinnerTick(UINT id)
{
    if (id != kSpinnerTimerId) return;
    if (!curSpinner) return;
    spinIdx = (spinIdx + 1) % kSpinnerFramesCount;
    curSpinner->setText(kSpinnerFrames[spinIdx]);
}

void WinSettingHotkeys::refreshCapturePreview()
{
    if (!curLabel) return;
    const std::wstring prefix = liveModifierPrefix();
    curLabel->setText(prefix.empty() ? Lang::get(L"shortcut.pressKey") : prefix);
}

void WinSettingHotkeys::onKeyDown(UINT key)
{
    // Esc = 取消这次录制（页面提示里的承诺）：不落盘、保持原键位
    if (key == VK_ESCAPE) { endCapture(); return; }
    // 主键按下即完成录制：不再等抬起。否则用户在组合还没按全时先松任意一个修饰键
    //（或改主意换修饰键）就会被当成"松手保存"，三键组合经常录不进去。
    auto keyStr = keyToStr(key);
    if (keyStr.empty()) return;
    // 只按修饰键：立即把已按住的修饰键显示出来，继续等主键（纯修饰键不落盘）
    if (isShortcutModifierKey(keyStr)) { refreshCapturePreview(); return; }

    const bool ctrl  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt   = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool lwin  = (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0;
    const bool rwin  = (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;

    // 裸 Backspace / Delete = 清空这一行（带修饰键时仍按普通组合录制）
    if ((key == VK_BACK || key == VK_DELETE) && !ctrl && !alt && !shift && !lwin && !rwin) {
        Setting::get()->setShortcutKey(curKey, {});
        endCapture();
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

    // 最多三键（两个修饰键 + 一个主键）；不合法就丢弃，保持原键位
    if (keys.size() > 3 || !isValidShortcutKeys(keys)) { endCapture(); return; }

    const std::wstring id = curKey;
    const std::wstring chord = joinShortcutKeys(keys);
    // ⚠ 先收尾再弹窗：按钮在确认前必须回到原值（取消 = 什么都不变）
    endCapture();

    // Windows 保留的组合：注册无法成功，直接说明原因（不再静默丢弃）
    if (reservedKindOf(chord) == ShortcutReserved::System) {
        SettingUi::ConfirmDialog::ask(win,
            fillMessage(L"shortcut.reservedSystem", chord, L""),
            [](bool) {}, Lang::get(L"setting.confirm"), L"", true);
        return;
    }
    // 通用编辑 / 保存键：可以设置，但会对所有程序的同名快捷键生效 → 先警告，由用户决定
    if (reservedKindOf(chord) == ShortcutReserved::Common) {
        auto weakThis = getWeakThis();
        SettingUi::ConfirmDialog::ask(win,
            fillMessage(L"shortcut.reservedCommon", chord, L""),
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
// 调用前录制已经收尾（endCapture），所以这里只负责数据与显示。
void WinSettingHotkeys::applyShortcut(const std::wstring& id, const std::vector<std::wstring>& keys)
{
    const std::wstring chord = joinShortcutKeys(keys);
    const std::wstring otherId = conflictOwner(chord, id);
    if (otherId.empty()) {
        Setting::get()->setShortcutKey(id, keys);
        for (auto* b : this->btns) applyShortcutLabel(b, b->id);
        return;
    }
    auto weakThis = getWeakThis();
    const std::wstring otherLabel = labelOf(otherId);
    SettingUi::ConfirmDialog::ask(win,
        fillMessage(L"cmdkey.conflict", chord, otherLabel),
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

std::wstring WinSettingHotkeys::keyToStr(UINT vkCode)
{
    switch (vkCode) {
    case VK_CONTROL: return L"Ctrl";
    case VK_MENU:    return L"Alt";
    case VK_SHIFT:   return L"Shift";
    case VK_LWIN:    return L"LWin";
    case VK_RWIN:    return L"RWin";
    case VK_CAPITAL: return L"CapsLock";
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
