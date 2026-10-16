#pragma once
#include <include/Ling.h>
#include <array>
#include "SettingWidgets.h"

class WinSettingFeatures : public Ling::Node
{
public:
	WinSettingFeatures(Ling::WinBase* parent);
	~WinSettingFeatures();
private:
	void build();
	void showTab(int index);
	void buildBasic(Ling::Node* p);
	void buildRecord(Ling::Node* p);
	void buildOutput(Ling::Node* p);
	void buildOcr(Ling::Node* p);
	void buildTranslate(Ling::Node* p);
	void rebuildTab(int index);
	// 延迟到调度队列再重建：调用点若位于卡片内按钮/下拉自身的回调中，同步 removeAllChildren
	// 会在事件派发中途销毁正在执行回调的控件（同 WinSettingHistory 筛选下拉的处理）。
	void deferRebuildTab(int index);
	static std::wstring newId();
	// 「鼠标高亮提醒」的悬停预览：鼠标停在该行上时，在光标下垫一个与实际效果同色同径的圆
	void updateMouseHlPreview(const POINT& pos);
	// 「恢复默认」的置灰：当前子页各项都还在出厂值 → 灰字且点击不做事（与外观页同款口径）
	void syncRestoreState();
private:
	std::array<Ling::Node*, 5> tabs{};
	SettingUi::SegmentedPillsRef segPills;   // 分段胶囊 Tab（滑动指示）
	Ling::Button* restoreBtn{ nullptr };
	int tabIndex{ 0 };
	Ling::Node* mouseHlArea{ nullptr };      // 触发预览的区域（卡片中部的空白处；随页面重建刷新）
	Ling::Node* mouseHlLabel{ nullptr };     // 该行的标题文本：落在它上面不算"中部空白"，不触发
	Ling::Node* mouseHlPreview{ nullptr };   // 预览圆；挂在 win->body 上，故用窗口坐标
	winrt::event_token mouseMoveTok{};
};
