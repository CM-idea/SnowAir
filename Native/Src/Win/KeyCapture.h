#pragma once
#include <include/Ling.h>
#include <functional>
#include <string>
#include <vector>

// 键位录制交互：点键位框 → 挂起全局热键 + 低级键盘钩子 → 转圈等按键 → 主键落下即结束。
//
// 这里只负责"怎么录"：预览、转圈、钩子生命周期、键码转字符串、组合拼装。
// "录到的键合不合法、落到哪、撞键怎么提示"由页面通过回调决定 ——
// 「全局热键」页与「系统按键」页的存储和冲突规则不同，但录制过程完全一样。
class KeyCapture
{
public:
    using StyleFn   = std::function<void(Ling::Button*, bool capturing)>;
    using RefreshFn = std::function<void(const std::wstring& id)>;
    using CommitFn  = std::function<void(const std::wstring& id, const std::vector<std::wstring>& keys)>;
    using ClearFn   = std::function<void(const std::wstring& id)>;
    using ValidFn   = std::function<bool(const std::vector<std::wstring>& keys)>;

    KeyCapture() = default;
    ~KeyCapture();

    KeyCapture(const KeyCapture&) = delete;
    KeyCapture& operator=(const KeyCapture&) = delete;

    // timerId 由页面给（两页的定时器 id 必须错开）
    void init(Ling::WinBase* win, UINT timerId,
        RefreshFn refresh, CommitFn commit, ClearFn clear, ValidFn valid);

    bool active() const { return !id_.empty(); }
    Ling::Button* button() const { return btn_; }

    // 开始录制；点同一个框 = 结束。结束后由 refresh 回调把框恢复成已落盘的值
    void begin(Ling::Button* btn, const std::wstring& id, StyleFn style);
    // 正常收尾：撤钩子/恢复热键/恢复按钮外观，并 refresh 一次
    void end();
    // 页面析构用：只撤钩子与定时器，不碰已经要被一起销毁的 UI
    void shutdown();

    // 页面把窗口事件转发进来；非录制状态下一律忽略
    void handleKey(UINT key);
    void handleKeyUp();
    void handleMouseDown(POINT pos);
    void handleBlur();
    void handleTimer(UINT timerId);

    // 两页共用的键名与组合工具（此前各自维护一份实现）
    static std::wstring keyToStr(UINT vkCode);
    static std::wstring joinKeys(const std::vector<std::wstring>& keys);
    static bool isModifierKey(const std::wstring& key);
    static std::wstring fillMessage(const wchar_t* langKey, const std::wstring& chord,
        const std::wstring& otherLabel);
    static UINT asyncMods();

private:
    void onKeyDown(UINT key, UINT mods);
    void preview(UINT mods);

    Ling::WinBase* win_{ nullptr };
    UINT timerId_{ 0 };
    RefreshFn refresh_;
    CommitFn commit_;
    ClearFn clear_;
    ValidFn valid_;
    StyleFn style_;

    Ling::Button* btn_{ nullptr };
    std::wstring id_;
    Ling::Label* spinner_{ nullptr };
    Ling::Label* label_{ nullptr };
    int spinIdx_{ 0 };
};
