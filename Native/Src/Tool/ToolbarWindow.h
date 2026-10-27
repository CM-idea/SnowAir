#pragma once
#include <include/Ling.h>
#include <memory>

class Tip;
class WinCap;

// 工具条与属性栏共用的窗口外壳：主题、背景画刷、画布、内容节点、最小尺寸、
// 选区边角命中穿透与 layout 骨架；外形由子类 drawChrome 绘制。
class ToolbarWindow : public Ling::WinBase
{
public:
	void hideHoverTip();
protected:
	// pad 为内容区内边距（逻辑值）
	void initChrome(float padLeft, float padRight, float padTop, float padBottom,
		Ling::FlexDirection direction);
	virtual WinCap* maskHost() const { return nullptr; }
	virtual void hideExtraHoverTips() {}
	virtual void drawChrome(ID2D1DeviceContext* ctx) = 0;
	void layout() override;
	void onMinMaxInfo(MINMAXINFO* mmi) override;
	LRESULT onHitTest(const POINT pos) override;

	Ling::Canvas* canvas{ nullptr };
	Ling::Node* contentNode{ nullptr };
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushBg;
	std::unique_ptr<Tip> tip;
};
