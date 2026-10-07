#include "pch.h"
#include "../Lang.h"
#include "../Setting.h"
#include "WinSetting.h"
#include "WinSettingHotkeys.h"
#include "SettingTheme.h"
#include "SettingWidgets.h"
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

    bool isReservedShortcut(const std::wstring& shortcut)
    {
        static const std::vector<std::wstring> reserved{
            L"Ctrl+C", L"Ctrl+V", L"Ctrl+X", L"Ctrl+Z", L"Ctrl+Y", L"Ctrl+A", L"Ctrl+S",
            L"Ctrl+Alt+Delete", L"Ctrl+Shift+Esc", L"Ctrl+Esc",
            L"Alt+Tab", L"Alt+Esc", L"Alt+F4",
            L"Win+L", L"Win+D", L"Win+E", L"Win+R", L"Win+Tab",
        };
        return std::find(reserved.begin(), reserved.end(), shortcut) != reserved.end();
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
        if (Ling::Util::strToKey(lowerKey) == 0) return false;
        return !isReservedShortcut(joinShortcutKeys(keys));
    }

    int modifierRank(const std::wstring& key)
    {
        if (key == L"Ctrl")  return 0;
        if (key == L"Alt")   return 1;
        if (key == L"Shift") return 2;
        if (key == L"Win")   return 3;
        if (key == L"LWin")  return 4;
        if (key == L"RWin")  return 5;
        return 100;
    }

    void normalizeShortcutKeys(std::vector<std::wstring>& keys)
    {
        std::stable_sort(keys.begin(), keys.end(),
            [](const std::wstring& a, const std::wstring& b) {
                return modifierRank(a) < modifierRank(b);
            });
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
    SettingUi::sectionHeader(this, Lang::get(L"setting.hotkeys"),
        Lang::get(L"setting.hotkeysDesc"), Lang::get(L"setting.hotkeysTip"));

    // 3 分组 12 项（AI 对话组已随 AI 功能一并移除）
    // 注：滚动已由外层 WinSetting 的内容 ScrollerBox 统一管理，本页不再自建滚动容器。
    struct Group { std::wstring title; std::vector<std::pair<std::wstring, std::wstring>> items; };
    const Group groups[] = {
        // 0 截图组 6 项
        {Lang::get(L"setting.hotkeyGrpCapture"),{
            { L"capture",               Lang::get(L"shortcut.capture") },
            { L"capture_fullscreen",    Lang::get(L"shortcut.capture_fullscreen") },
            { L"capture_focused_window",Lang::get(L"shortcut.capture_focused_window") },
            { L"fullscreen_canvas",     Lang::get(L"shortcut.fullscreen_canvas") },
            { L"record",                Lang::get(L"shortcut.record") },
            { L"show_cursor",           Lang::get(L"shortcut.show_cursor") },
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

    for (const auto& g : groups)
        buildGroup(g.title, g.items, this);

    // 内容区只有外层 content 提供底部留白(20)：清掉末尾 optionRow 的 marginBottom，
    // 使底部间距与左右一致。
    SettingUi::trimTrailingGap(this);

    auto weakThis = getWeakThis();
    onKeyDownToken = win->onKeyDown.add([this, weakThis](UINT key) {
        if (!weakThis.lock()) return;
        if (this->curKey.empty()) return;
        this->onKeyDown(key);
        });
    onKeyUpToken = win->onKeyUp.add([this, weakThis](UINT key) {
        if (!weakThis.lock()) return;
        if (this->curKey.empty()) return;
        this->onKeyUp(key);
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
    // 等待按键期间窗口失焦（切到别的程序）→ 直接取消这次录制，不留悬挂状态。
    onBlurToken = win->onBlur.add([this, weakThis]() {
        if (!weakThis.lock()) return;
        if (this->curKey.empty()) return;
        this->endCapture();
        });
}

void WinSettingHotkeys::buildGroup(const std::wstring& grpTitle,
    const std::vector<std::pair<std::wstring, std::wstring>>& items, Ling::Node* p)
{
    // 分组标题：独立加粗文本（无卡片背景），参考图一
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

        auto* btn = r.ctrlBox->makeChild<Ling::Button>();
        btn->setId(it.first);
        applyShortcutLabel(btn, it.first);
        btn->setHeight(SettingTheme::ctrlH);
        btn->setWidth(160.f);
        btn->setBorderRadius(SettingTheme::radiusCtl);   // 与所在行的外框(10)同心
        styleHotkeyBtn(btn, false);   // 常态配色（白底 + 描边 + 深字）
        btn->onClick.add([this](Ling::Button* b) { this->onBtnClick(b); });
        btns.push_back(btn);
    }
}

WinSettingHotkeys::~WinSettingHotkeys()
{
    // 页面在录制中途被销毁（切走/关窗）时，热键仍是挂起状态，这里兜底恢复
    if (!curKey.empty()) Setting::get()->resumeShortcutKeys();
    win->killTimer(kSpinnerTimerId);
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
    tempKeys.clear();
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

    // 录制期间挂起全局热键：否则按下的组合键会先被全局热键（RegisterHotKey / 低级钩子）吞掉，
    // 根本传不到设置窗口，导致录不进任何热键。
    Setting::get()->suspendShortcutKeys();

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
    tempKeys.clear();
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

void WinSettingHotkeys::onKeyDown(UINT key)
{
    auto keyStr = keyToStr(key);
    if (keyStr.empty()) return;
    if (!isShortcutModifierKey(keyStr)) {
        auto ensure = [&](int vk, const std::wstring& name) {
            if ((GetAsyncKeyState(vk) & 0x8000) &&
                std::find(tempKeys.begin(), tempKeys.end(), name) == tempKeys.end()) {
                tempKeys.push_back(name);
            }
            };
        ensure(VK_CONTROL, L"Ctrl");
        ensure(VK_MENU,    L"Alt");
        ensure(VK_SHIFT,   L"Shift");
        ensure(VK_LWIN,    L"LWin");
        ensure(VK_RWIN,    L"RWin");
    }
    bool isContains = std::find(tempKeys.begin(), tempKeys.end(), keyStr) != tempKeys.end();
    if (isContains) return;
    tempKeys.push_back(keyStr);
}

void WinSettingHotkeys::onKeyUp(UINT key)
{
    if (curKey.empty()) return;
    normalizeShortcutKeys(tempKeys);
    if (isValidShortcutKeys(tempKeys)) {
        Setting::get()->setShortcutKey(curKey, tempKeys);
    }
    endCapture();
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
    case VK_MULTIPLY: return L"*"; case VK_ADD: return L"+";
    case VK_SUBTRACT: return L"-"; case VK_DIVIDE: return L"/";
    case VK_DECIMAL:  return L".";
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
