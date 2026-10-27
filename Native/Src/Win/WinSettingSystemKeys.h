#pragma once
#include <include/Ling.h>
#include <string>
#include <utility>
#include <vector>
#include "KeyCapture.h"

// 「系统按键」页 —— 软件内**命令**的可配置按键（见 CommandKeys.h）。
// 分组 / 行序 / 默认键位 / 只读标记全部来自命令表，本页不另抄一份清单。
//
// 录制交互与「全局热键」页同款（点键位框 → 等按键 → 主键按下即落盘）：
//   · 只读行（「常规」组整组 + 层级退出 + 删除标注）：行名称保持正常色，仅键位框置灰且不可点；
//   · 录到固定键（只读行占用的键，跨界面一律算占用）→ 直接拒绝；
//   · 录到与同一界面上其他可改命令重复的键 → 弹「是否覆盖」，确认后才解绑旧主并落值。
class WinSettingSystemKeys : public Ling::Node
{
public:
	explicit WinSettingSystemKeys(Ling::WinBase* parent);
	~WinSettingSystemKeys();
private:
	void onBtnClick(Ling::Button* btn);
	// 录到的组合：保留键拒绝 → 同界面撞键确认 → 落值并刷新
	void applyChord(const std::wstring& id, const std::vector<std::wstring>& keys);
	// 把某条命令的当前键位刷到它的键位框上（空 = 「点击设置」占位、muted 色）
	void refreshRow(const std::wstring& id);
	// 「恢复默认」的置灰：一条改绑都没有 → 灰字且点击不做事
	void syncRestoreState();
private:
	struct Row { std::wstring id; Ling::Button* btn; };
	KeyCapture capture_;
	std::vector<Row> rows;          // 可录制的行（只读行不进这里）
	Ling::Button* restoreBtn{ nullptr };
	winrt::event_token onMouseMoveToken{};
	winrt::event_token onMouseDownToken, onKeyDownToken, onKeyUpToken, onTimerToken, onBlurToken;
	// 与「全局热键」页(0x5000) / TextBox(0x4200+) 错开，避免同窗口定时器 id 冲突
	static constexpr UINT kSpinnerTimerId{ 0x5100 };
};
