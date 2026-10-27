#include "pch.h"
#include "../Lang.h"
#include "../Setting.h"
#include "../Tool/IconCodes.h"
#include "HotkeyHook.h"
#include "KeyCapture.h"
#include "SettingTheme.h"

namespace
{
    constexpr UINT kSpinnerMs{ 100 };
    constexpr uint32_t kCaptureBg{ 0x0FDC78FF };
    constexpr uint32_t kCaptureFg{ 0xFFFFFFFF };

    // 加载指示帧：按「加载 1 ~ 加载 8」顺时针轮换
    const wchar_t* kSpinnerFrames[] = {
        Icon::Loading1, Icon::Loading2, Icon::Loading3, Icon::Loading4,
        Icon::Loading5, Icon::Loading6, Icon::Loading7, Icon::Loading8,
    };
    constexpr int kSpinnerFramesCount = 8;

    // 录制中的实时预览：按当前按住的修饰键拼前缀（顺序与落盘口径一致，末尾带 "+"）。
    // 只反映修饰键，主键一按下就整条落盘并结束录制，所以预览到这里为止。
    std::wstring modifierPrefix(UINT mods)
    {
        std::wstring out;
        auto add = [&out](const wchar_t* name) {
            if (!out.empty()) out += L"+";
            out += name;
        };
        if (mods & MOD_CONTROL) add(L"Ctrl");
        if (mods & MOD_ALT) add(L"Alt");
        if (mods & MOD_SHIFT) add(L"Shift");
        if (mods & MOD_WIN) add(L"Win");
        if (!out.empty()) out += L"+";
        return out;
    }
}

KeyCapture::~KeyCapture()
{
    shutdown();
}

void KeyCapture::init(Ling::WinBase* win, UINT timerId,
    RefreshFn refresh, CommitFn commit, ClearFn clear, ValidFn valid)
{
    win_ = win;
    timerId_ = timerId;
    refresh_ = std::move(refresh);
    commit_ = std::move(commit);
    clear_ = std::move(clear);
    valid_ = std::move(valid);
}

void KeyCapture::begin(Ling::Button* btn, const std::wstring& id, StyleFn style)
{
    if (!win_ || !btn || id.empty()) return;
    if (active()) {
        if (btn_ == btn) { end(); return; }   // 再点一次同一个框 = 取消
        end();
    }

    btn_ = btn;
    id_ = id;
    style_ = std::move(style);

    // 清空按钮内置文本，改用自定义的两个子节点（加载指示 + 文案）。Button 的内置 Text
    // 固定为 flex 首个子节点，无法插入到加载指示左侧，因此先置空，再以 (spinner, label) 居中排列。
    btn_->setText(L"");
    if (style_) style_(btn_, true);
    // Button 默认 column：改成 row 后转圈居左、文案居右
    btn_->setFlexDirection(Ling::FlexDirection::Row);

    spinner_ = btn_->makeChild<Ling::Label>();
    spinner_->setText(kSpinnerFrames[0]);
    spinner_->setFontFamily(Icon::Family);
    spinner_->setFontSize(16.f);
    spinner_->setColor(kCaptureFg);
    spinner_->setMarginRight(SettingTheme::sp2);

    label_ = btn_->makeChild<Ling::Label>();
    label_->setText(Lang::get(L"shortcut.pressKey"));
    label_->setFontSize(SettingTheme::fontBase);
    label_->setColor(kCaptureFg);
    preview(0);

    // 录制期间挂起全局热键：否则按下的组合键会先被全局热键（RegisterHotKey / 低级钩子）
    // 消费，传不到设置窗口，导致无法录制。
    Setting::get()->suspendShortcutKeys();

    // 再挂一个临时低级键盘钩子接管键盘输入：目标组合可能已被其他进程注册为全局热键，
    // 按键会在系统热键分发阶段被消费，设置窗口收不到 WM_KEYDOWN。录制期间由这里
    // 先行消费所有按键（含系统快捷键），再转回页面。
    HotkeyHook::beginCapture([this](UINT vk, bool up, UINT mods) {
        if (id_.empty()) return;
        if (up) { preview(mods); return; }   // 松开修饰键：预览退回
        onKeyDown(vk, mods);
    });

    spinIdx_ = 0;
    win_->setTimer(kSpinnerMs, timerId_);
}

void KeyCapture::end()
{
    if (id_.empty()) return;
    const std::wstring id = id_;
    if (win_) win_->killTimer(timerId_);
    if (btn_) {
        if (spinner_) { btn_->removeChild(spinner_); spinner_ = nullptr; }
        if (label_) { btn_->removeChild(label_); label_ = nullptr; }
        if (style_) style_(btn_, false);   // 结束录制：恢复常态配色
    }
    btn_ = nullptr;
    id_.clear();
    // 先撤录制钩子，再恢复全局热键：顺序反了会让重装后的钩子仍处于消费所有按键的状态
    HotkeyHook::endCapture();
    Setting::get()->resumeShortcutKeys();
    if (refresh_) refresh_(id);
}

void KeyCapture::shutdown()
{
    const bool wasActive = !id_.empty();
    if (win_ && wasActive) win_->killTimer(timerId_);
    // 页面正在销毁：控件会随之释放，不再调用 removeChild、修改样式或刷新文案
    btn_ = nullptr;
    spinner_ = nullptr;
    label_ = nullptr;
    id_.clear();
    if (wasActive) {
        HotkeyHook::endCapture();
        Setting::get()->resumeShortcutKeys();
    }
}

void KeyCapture::handleKey(UINT key)
{
    if (!active()) return;
    onKeyDown(key, asyncMods());
}

void KeyCapture::handleKeyUp()
{
    if (!active()) return;
    preview(asyncMods());
}

void KeyCapture::handleMouseDown(POINT pos)
{
    if (!active()) return;
    if (btn_ && btn_->isPosIn(pos)) return;
    end();
}

void KeyCapture::handleBlur()
{
    if (active()) end();
}

void KeyCapture::handleTimer(UINT timerId)
{
    if (!active() || timerId != timerId_ || !spinner_) return;
    spinIdx_ = (spinIdx_ + 1) % kSpinnerFramesCount;
    spinner_->setText(kSpinnerFrames[spinIdx_]);
}

void KeyCapture::onKeyDown(UINT key, UINT mods)
{
    // Esc 取消本次录制：不写入配置，保持原键位（Esc 本身不参与录制）
    if (key == VK_ESCAPE) { end(); return; }
    // 主键按下即完成录制，不等抬起：抬起时修饰键可能已被提前松开，会被误判为保存，
    // 导致三键组合无法录入。
    const std::wstring keyStr = keyToStr(key);
    if (keyStr.empty()) return;
    // 只按修饰键：立即把已按住的修饰键显示出来，继续等主键（纯修饰键不落盘）
    if (isModifierKey(keyStr)) { preview(mods); return; }

    // 裸 Backspace / Delete = 清空这一行（带修饰键时仍按普通组合录制）
    if ((key == VK_BACK || key == VK_DELETE) && mods == 0) {
        const std::wstring id = id_;
        if (clear_) clear_(id);
        end();   // refresh 回调里显示清空后的占位
        return;
    }

    // 组合取"主键按下这一刻"按住的修饰键：与按下/松开的先后顺序无关
    std::vector<std::wstring> keys;
    if (mods & MOD_CONTROL) keys.push_back(L"Ctrl");
    if (mods & MOD_ALT) keys.push_back(L"Alt");
    if (mods & MOD_SHIFT) keys.push_back(L"Shift");
    if (mods & MOD_WIN) keys.push_back(L"LWin");
    keys.push_back(keyStr);

    // 先结束录制，再进行校验与赋值：按钮在确认前必须恢复原值；冲突被取消时不做任何修改
    const std::wstring id = id_;
    end();
    // 最多三键（两个修饰键 + 一个主键）；不合法就丢弃，保持原键位
    if (keys.size() > 3) return;
    if (valid_ && !valid_(keys)) return;
    if (commit_) commit_(id, keys);
}

void KeyCapture::preview(UINT mods)
{
    if (!label_) return;
    const std::wstring prefix = modifierPrefix(mods);
    label_->setText(prefix.empty() ? Lang::get(L"shortcut.pressKey") : prefix);
}

std::wstring KeyCapture::joinKeys(const std::vector<std::wstring>& keys)
{
    std::wstring out;
    for (const auto& key : keys) {
        if (!out.empty()) out += L"+";
        out += (key == L"LWin" || key == L"RWin") ? L"Win" : key;
    }
    return out;
}

bool KeyCapture::isModifierKey(const std::wstring& key)
{
    return key == L"Ctrl" || key == L"Alt" || key == L"Shift"
        || key == L"Win" || key == L"LWin" || key == L"RWin";
}

std::wstring KeyCapture::fillMessage(const wchar_t* langKey, const std::wstring& chord,
    const std::wstring& otherLabel)
{
    std::wstring s = Lang::get(langKey);
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

UINT KeyCapture::asyncMods()
{
    // 非钩子路径（注入按键 / 钩子没装上）仍然看实时键状态：这些按键没被吞，状态是准的
    UINT mods = 0;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
    if (GetAsyncKeyState(VK_MENU) & 0x8000) mods |= MOD_ALT;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) mods |= MOD_SHIFT;
    if ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) mods |= MOD_WIN;
    return mods;
}

std::wstring KeyCapture::keyToStr(UINT vkCode)
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
