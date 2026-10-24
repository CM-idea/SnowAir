#pragma once
#include <include/Ling.h>
#include "ToolbarTheme.h"
#include "PropSlider.h"
#include <functional>

class ToolSub;

// 属性栏二级气泡：圆角滑条 / 水印角度 / 包浆水印位置+大小
class ToolNestedPanel : public Ling::WinBase
{
public:
	enum class Kind { None, Corner, WmAngle, PatinaWm, Emoji, BoardOpacity, PenOpacity, ShapeSides };
	explicit ToolNestedPanel(ToolSub* owner);
	~ToolNestedPanel();
	void openCorner(const RECT& workArea, Ling::Button* anchor);
	void openWmAngle(const RECT& workArea, Ling::Button* anchor);
	void openPatinaWm(const RECT& workArea, Ling::Button* anchor);
	void openEmoji(const RECT& workArea, Ling::Button* anchor);
	void openBoardOpacity(const RECT& workArea, Ling::Button* anchor, float value);
	// 荧光笔透明度：与水印角度同款单滑条气泡
	void openPenOpacity(const RECT& workArea, Ling::Button* anchor);
	// 多边形边数 / 星形角数（3–20）：与水印角度同款单滑条气泡
	void openShapeSides(const RECT& workArea, Ling::Button* anchor, bool star);
	void closePanel();
	bool isOpen() const { return open_; }
	Kind kind() const { return kind_; }
	void updatePosition(const RECT& workArea);
	void hideHoverTip() { if (tip) tip->hide(); }
private:
	void onCreated() override;
	void layout() override;
	void onMinMaxInfo(MINMAXINFO* mmi) override;
	void paintChrome(ID2D1DeviceContext* ctx);
	void clearContent();
	void place(const RECT& workArea, Ling::Button* anchor);
	void finishOpen(const RECT& workArea, Ling::Button* anchor, float logicW, float logicH = 0.f);
	void prepareContentRow();
	void applyTipDirection(bool tipDown);
	void rebuildPatinaWmRow();
	void rebuildEmojiGrid();
	Ling::Button* makeIconBtn(const wchar_t* icon, const std::wstring& tip, bool active, std::function<void()> onClick);
	PropSlider::Pair addSlider(float minV, float maxV, float val, std::function<void(float)> onChange);
private:
	ToolSub* owner;
	Kind kind_{ Kind::None };
	Ling::Node* contentNode{ nullptr };
	Ling::Canvas* chromeCanvas{ nullptr };
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushBg;
	std::unique_ptr<class Tip> tip;
	Ling::Button* anchorBtn{ nullptr };
	RECT workArea_{};
	float arrowX{ 0.f };
	bool tipDown_{ false };
	bool open_{ false };
	bool needPaint_{ false };
	bool sidesStar_{ false };   // ShapeSides 当前调的是星形（否则多边形）
	// 当前内容的滑条（悬停时在滑条上方显示说明；多边形/星形还会在画布上预览形状）
	Ling::Slider* activeSlider_{ nullptr };
	Ling::Label* activeValue_{ nullptr };
	std::wstring sliderTip_;
	bool sliderHover_{ false };
	bool sidesHover_{ false };
	static constexpr float btnSize{ ToolbarTheme::propBtnSize };
	static constexpr float iconInner{ ToolbarTheme::propIconInner };
	static constexpr float marginTop{ ToolbarTheme::caretSize };
};
