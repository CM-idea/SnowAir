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
private:
	std::array<Ling::Node*, 5> tabs{};
	SettingUi::SegmentedPillsRef segPills;   // 分段胶囊 Tab（滑动指示）
	Ling::Button* restoreBtn{ nullptr };
	int tabIndex{ 0 };
};
