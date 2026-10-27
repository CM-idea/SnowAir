#pragma once
#include <include/Ling.h>
#include <utility>
#include "KeyCapture.h"
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
	// 录制提交：先处理 Windows 保留键 / 通用编辑键，再进入同页撞键判定
	void commitChord(const std::wstring& id, const std::vector<std::wstring>& keys);
	// 落值（含同页撞键确认）并刷新按钮文字；调用前录制已收尾
	void applyShortcut(const std::wstring& id, const std::vector<std::wstring>& keys);
	// 冲突判定：chord 已被同页另一行占用时返回那一行的 id（空 = 没冲突）
	std::wstring conflictOwner(const std::wstring& chord, const std::wstring& selfId) const;
	// id → 本页显示名（冲突提示里用）
	std::wstring labelOf(const std::wstring& id) const;
	// 构造一组：先加 groupLabel，再加若干行白底卡片录入按钮
	void buildGroup(const std::wstring& grpTitle,
		const std::vector<std::pair<std::wstring, std::wstring>>& items, Ling::Node* p);
	// 「恢复默认」的置灰：键位都还是出厂值 → 灰字且点击不做事
	void syncRestoreState();
private:
	KeyCapture capture_;
	std::vector<Ling::Button*> btns;
	std::vector<std::pair<std::wstring, std::wstring>> labelById;   // id → 行标签
	Ling::Button* restoreBtn{ nullptr };
	winrt::event_token onMouseMoveToken{};
	winrt::event_token onMouseDownToken, onKeyDownToken, onKeyUpToken, onTimerToken, onBlurToken;
	// 与 TextBox(0x4200+) 错开，避免同窗口定时器 id 冲突
	static constexpr UINT kSpinnerTimerId{ 0x5000 };
};
