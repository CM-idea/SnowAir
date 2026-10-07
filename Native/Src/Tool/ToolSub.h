#pragma once
#include <include/Ling.h>
#include <unordered_map>
#include "ToolbarTheme.h"

class AnnotHost;
class Tip;
class ToolColorPanel;
class ToolNestedPanel;
class ToolSub:public Ling::WinBase
{
public:
	ToolSub(AnnotHost* win);
	~ToolSub();
	void showRectTools();
	void showArrowTools();
	void showPenTools();
	void showNumberTools();
	void showTextTools();
	void showMosaicTools();
	void showPatinaTools();
	void showWatermarkTools();
	void showHighlightTools();
	void showEraserTools();

	void hideTools();
	// 拖选区时临时藏起（不拆内容）；抬手 resume 再跟新位置
	void suspendChrome();
	void resumeChrome(const RECT& workArea);
	bool hasContent();
	// 属性栏窗口当前是否真的显示着（区别于 hasContent：拖选区挂起时内容还在但窗是隐藏的）
	bool chromeVisible() const { return isVisible; }
	float getDesiredHeight();
	void updatePosition(const RECT& workArea);
	D2D1_COLOR_F getSelectedColor() const;
	UINT32 getSelectedColorValue() const;
	float getSliderVal() const;
	float setShapeSliderVal(const std::wstring& tool, float px);
	static constexpr float mainGap{ 2.f };
	void bindOwner(HWND ownerHwnd);
	void hideHoverTip();
	// 供 ToolColorPanel 读写色板
	const std::vector<UINT32>& colorPalette() const { return colors; }
	size_t customColorIdx() const { return customColorIndex; }
	size_t selectedColorIdx() const { return selectColorIndex; }
	void onColorPicked(size_t index);
	void getColorTriggerAnchor(float& screenCenterX, float& screenBottom) const;
	bool pickCustomColor();
	void notifyStyleChanged();
	void refreshNestedTrigger();
	void updateEmojiTriggerGlyph();
	RECT workAreaRect() const;
	// 视窗内滚轮：按工具量程微调主滑条（对齐 QT adjustPrimarySize）
	bool adjustPrimarySize(int dir);
	// 属性栏是否在主栏上方（箭头朝下）；二级弹窗据此决定默认开向
	bool tipDown() const { return tipDown_; }
public:
	// 「渐隐画笔 / 画笔」是同级一级槽位，底层工具同为 pen，只差 isPenFade
	void setPenSlotMode(const std::wstring& slotId);
private:
	LRESULT onHitTest(const POINT pos) override; // 选区边/角穿透给底层 WinCap
public:
	// numberStyle: 0 数字 / 1 字母 / 2 Emoji（对齐 QT 三种序号，无中文）
	enum NumberStyle : int { NumberDigit = 0, NumberLetter = 1, NumberEmoji = 2 };
	// arrowHead: 0 普通 / 1 大箭头 / 2 双箭头 / 3 标注箭头（对齐产品属性气泡）
	enum ArrowHead : int { ArrowDefault = 0, ArrowBig = 1, ArrowBoth = 2, ArrowAnnot = 3 };
	bool isRectFill{ false }, isEllipse{ false }, isRectDash{ false };
	bool isArrowFill{ true }, isNumberFill{ true };
	bool isLine{ false }; // 箭头属性栏内切换直线（无独立主栏按钮）
	bool isArrowDash{ false }, isArrowRound{ false };
	bool isPenDash{ false }, isPenSoft{ false }, isPenFade{ false };
	bool isLineTransparent{ false }, isTextBold{ false }, isTextItalic{ false };
	bool isMosaicBlur{ false };
	bool isPatinaGreen{ true }, isPatinaWatermark{ true };
	bool isHighlightEllipse{ false }, isHighlightBorder{ false };
	int numberStyle{ NumberDigit };
	int arrowHead{ ArrowDefault };
	int patinaPlan{ 0 }; // 0 右下 / 1 右下角 / 2 中下
	int patinaWmSize{ 20 };
	float watermarkAngle{ 45.f };
	std::wstring watermarkText;
	std::wstring serialEmoji; // Emoji 序号当前表情
	float cornerRadius{ 0.f }; // 逻辑像素；0 = 直角
	float lastCornerRadius{ 10.f }; // 关圆角后再开时恢复
	float getCornerRadiusPx() const { return cornerRadius * dpi; }
private:
	void onCreated() override;
	void layout() override;
	void onMinMaxInfo(MINMAXINFO* mmi);
	void paintBorder(ID2D1DeviceContext* ctx);
	void initColorTrigger();
	void updateColorTriggerSwatch();
	void toggleColorPanel();
	void closeColorPanel();
	void closeNestedPanel();
	void initSlider();
	void updateSliderValueLabel();
	void loadStrokeSlider(const std::wstring& toolId);
	std::wstring getSliderTipKey() const;
	void applyToggleStyle(Ling::Button* btn, bool selected);
	Ling::Button* makeToggleBtn(const std::wstring& text, bool* flag, const std::wstring& tipKey, const std::wstring& cfgKey);
	// 复合图标开关：一个 Node 里叠两个字形的 Label
	Ling::Button* makeOverlayToggleBtn(const std::wstring& baseGlyph, const std::wstring& overlayGlyph,
		float overlayAlpha, bool* flag, const std::wstring& tipKey);
	void applyOverlayToggleStyle(bool selected);
	void makeNumberStyleBtns();
	void refreshNumberStyleBtns();
	void makeArrowHeadBtn(const wchar_t* icon, int style, const wchar_t* tipKey);
	void makeArrowHeadBtns();
	void refreshArrowHeadBtns();
	void makeRectStyleBtns();
	void beginTool(const std::wstring& id);
	void initSize(int btnCount, bool withColors, bool centerOnBtn = false, float extraLogicW = 0.f);
	void refreshSize();
	float toPx(float logical) const;
	void styleToolbarBtn(Ling::Button* btn);
	void applyTipDirection(bool tipDown);
	float rememberedSlider(const std::wstring& key, float def, float mn, float mx) const;
	void rememberSlider(const std::wstring& key, float val);
private:
	Ling::Node* contentNode;
	Ling::Button* colorTrigger{ nullptr };
	Ling::Node* colorTriggerRing{ nullptr };
	Ling::Label* colorTriggerSwatch{ nullptr };
	std::unique_ptr<ToolColorPanel> colorPanel;
	std::unique_ptr<ToolNestedPanel> nestedPanel;
	Ling::Slider* slider{ nullptr };
	Ling::Label* sliderValue{ nullptr };
	Ling::TextBox* wmTextBox{ nullptr };
	std::unique_ptr<Tip> tip;
	static constexpr float btnSize{ ToolbarTheme::propBtnSize };
	static constexpr float hoverRadius{ ToolbarTheme::hoverRadius };
	static constexpr float hoverInset{ 4.f };
	static constexpr float iconInner{ ToolbarTheme::propIconInner };
	static constexpr float marginTop{ ToolbarTheme::caretSize };
	static constexpr float swatchSize{ ToolbarTheme::swatchSize };
	static constexpr float swatchRing{ ToolbarTheme::swatchRing };
	static constexpr float swatchRadius{ ToolbarTheme::swatchRadius };
	static constexpr size_t customColorIndex{ 6 };
	float arrowX{0.f};
	bool tipDown_{ false }; // false=在主栏下方（尖朝上）；true=上方（尖朝下）
	AnnotHost* win;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushBg;
	Ling::Canvas* canvas{ nullptr };
	bool isVisible{ false };
	bool hasTools{ false };
	bool centerOnBtn{ false };
	bool dpiChanged{ false };
	bool sliderHover{ false };
	int sizeBtnCount{ 0 };
	bool sizeWithColors{ false };
	UINT selectColorIndex{ 0 };
	float sliderVal{ 2.f };
	float sliderMin{ 1.f }, sliderMax{ 20.f };
	bool suppressSliderStyleNotify{ false };
	std::wstring curToolId, curSliderKey;
	static constexpr float numberMin{ 6.f }, numberMax{ 86.f };
	std::vector<UINT32> colors = { 0xFF272CFF, 0x00FF40FF, 0x1677FFFF, 0xFFCC00FF, 0x000000FF, 0xFFFFFFFF, 0xFF272CFF };
	std::vector<Ling::Button*> numberStyleBtns;
	std::vector<Ling::Button*> arrowHeadBtns;
	Ling::Button* fillBtn{ nullptr };
	Ling::Button* dashBtn{ nullptr };
	Ling::Button* softBtn{ nullptr };
	Ling::Button* roundBtn{ nullptr };
	Ling::Button* lineBtn{ nullptr };
	Ling::Button* angleBtn{ nullptr };
	Ling::Button* patinaWmBtn{ nullptr };
	Ling::Button* emojiTrigger{ nullptr };
	// 复合图标按钮（马赛克模糊）：底字形 + 半透明叠字形
	Ling::Button* overlayBtn{ nullptr };
	Ling::Label* overlayBaseLabel{ nullptr };
	Ling::Label* overlayTopLabel{ nullptr };
	float overlayAlpha{ 0.25f };
	bool* overlayFlag{ nullptr };
	float sizeExtraW{ 0.f };
	// 仅本会话记忆（对齐 QT PropertyBubble m_store）；不写盘
	std::unordered_map<std::wstring, float> sliderMem;
	std::unordered_map<std::wstring, UINT> colorIndexMem;
	UINT32 sessionCustomColor{ 0xFF272CFF };
};
