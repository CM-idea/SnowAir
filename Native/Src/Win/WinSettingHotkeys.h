#pragma once
#include <include/Ling.h>
#include <utility>
/**
 * 热键设置页 —— 分组结构（AI 对话组已随 AI 功能移除）：
 *   截图组(6) / 翻译组(2) / 其他组(4) = 共 12 项
 * 旧版只有 1 项，现扩展为 3 分组。
 */
class WinSettingHotkeys :public Ling::Node
{
public:
	WinSettingHotkeys(Ling::WinBase* parent);
	~WinSettingHotkeys();
private:
	void onBtnClick(Ling::Button* btn);
	void beginCapture(Ling::Button* btn);
	void endCapture();
	// mods = 按键发生那一刻的修饰键状态（MOD_CONTROL/MOD_ALT/MOD_SHIFT/MOD_WIN）。
	// 录制期间物理修饰键会被钩子吞掉，不能事后用 GetAsyncKeyState 补查。
	void onKeyDown(UINT key, UINT mods);
	void onSpinnerTick(UINT id);
	// 录制中的实时预览：按当前按住的修饰键刷新按钮里的文案（如 "Ctrl+Shift+"）
	void refreshCapturePreview(UINT mods);
	std::wstring keyToStr(UINT vkCode);
	// 冲突判定：chord 已被同页另一行占用时返回那一行的 id（空 = 没冲突）
	std::wstring conflictOwner(const std::wstring& chord, const std::wstring& selfId) const;
	// 落值（含同页撞键确认）并刷新按钮文字；调用前录制已收尾
	void applyShortcut(const std::wstring& id, const std::vector<std::wstring>& keys);
	// id → 本页显示名（冲突提示里用）
	std::wstring labelOf(const std::wstring& id) const;
	// 构造一组：先加 groupLabel，再加若干行白底卡片录入按钮
	void buildGroup(const std::wstring& grpTitle,
		const std::vector<std::pair<std::wstring, std::wstring>>& items, Ling::Node* p);
	// 「恢复默认」的置灰：键位都还是出厂值 → 灰字且点击不做事
	void syncRestoreState();
private:
	std::vector<Ling::Button*> btns;
	std::vector<std::pair<std::wstring, std::wstring>> labelById;   // id → 行标签
	Ling::Button* restoreBtn{ nullptr };
	winrt::event_token onMouseMoveToken{};
	std::wstring curKey;
	winrt::event_token onMouseDownToken, onKeyDownToken, onKeyUpToken, onTimerToken, onBlurToken;
	// 等待按键时按钮内的"转圈圈"（加载感）：用定时器轮换四分之一圆字形
	Ling::Label* curSpinner{ nullptr };
	Ling::Label* curLabel{ nullptr };
	int spinIdx{ 0 };
	// 与 TextBox(0x4200+) 错开，避免同窗口定时器 id 冲突
	static constexpr UINT kSpinnerTimerId{ 0x5000 };
	static constexpr UINT kSpinnerMs{ 100 };
};
