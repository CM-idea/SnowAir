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
	// 工具条 / 属性栏都是独立窗口，缩放必须来自**它自己所在的显示器**。
	// 演示画布的宿主窗口铺满所有显示器，它的 dpi 只代表其中一块屏；照抄会让整条栏
	// 在另一块屏上偏大 / 偏小，属性栏也会跟着对不齐。返回 true = 本次按（新）缩放
	// 重算过窗口尺寸，调用方接着重排位置即可。
	bool adoptMonitorDpi();
	// 唯一采纳入口：dpi 真变了才重推子节点并通知子类。调用点禁止再裸赋值 dpi。
	// 返回 true = 这次真正走了 onDpiAdopted()（同一个 dpi 不会重复重算）。
	bool applyDpi(float scale);
protected:
	// 适配显示器缩放后，由子类按新 dpi 重算自己的窗口尺寸（通常就是 refreshSize）
	virtual void onDpiAdopted() {}
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
private:
	// 上次按哪个缩放重算过窗口尺寸。不能用 dpi 判断“换没换过”：
	// WinBase 收到 WM_DPICHANGED 时已经先把 dpi 改成新值，调用方回来再比较永远相等。
	float appliedDpi_{ 0.f };
};
