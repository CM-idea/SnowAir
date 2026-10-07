#pragma once
#include <include/Ling.h>
/**
 * 热键设置页 —— 分组结构（AI 对话组已随 AI 功能移除）：
 *   截图组(6) / 翻译组(2) / 其他组(4) = 共 12 项
 * 旧版只有 1 项（shortcut.cap），现扩展为 3 分组。
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
	void onKeyDown(UINT key);
	void onKeyUp(UINT key);
	void onSpinnerTick(UINT id);
	std::wstring keyToStr(UINT vkCode);
	// 构造一组：先加 groupLabel，再加若干行白底卡片录入按钮
	void buildGroup(const std::wstring& grpTitle,
		const std::vector<std::pair<std::wstring, std::wstring>>& items, Ling::Node* p);
private:
	std::vector<Ling::Button*> btns;
	std::wstring curKey;
	winrt::event_token onMouseDownToken, onKeyDownToken, onKeyUpToken, onTimerToken, onBlurToken;
	std::vector<std::wstring> tempKeys;
	// 等待按键时按钮内的"转圈圈"（加载感）：用定时器轮换四分之一圆字形
	Ling::Label* curSpinner{ nullptr };
	Ling::Label* curLabel{ nullptr };
	int spinIdx{ 0 };
	// 与 TextBox(0x4200+) 错开，避免同窗口定时器 id 冲突
	static constexpr UINT kSpinnerTimerId{ 0x5000 };
	static constexpr UINT kSpinnerMs{ 100 };
};
