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
	// 「标注」（原线段）：从箭头属性栏独立出来，只有虚线/圆头/颜色/粗细
	void showLineTools();
	// 标注的形态：直线 / 波浪线 / 气泡 / 角度 / 尺寸 / 爆裂
	enum LineKind : int { LineStraight = 0, LineWave = 1, LineBubble = 2, LineAngle = 3, LineDimension = 4, LineBurst = 5 };
	int lineKind{ LineStraight };
	// 矩形工具形态：矩形 / 多边形 / 星形（椭圆是单独的 isEllipse 开关）
	enum RectKind : int { RectShape = 0, PolygonShape = 1, StarShape = 2 };
	int rectKind{ RectShape };
	int polygonSides{ 3 };   // 多边形边数（3–20）
	int starPoints{ 5 };     // 星形角数（3–20）
	// 多边形/星形滑条：拖动时让当前选中的同形态标注实时跟着变
	void applyShapeSidesLive(bool star, int sides);
	// 圆角滑条：拖动时让当前选中的矩形/多边形/星形实时跟着变
	void applyCornerRadiusLive();
	// 多边形/星形二级滑条：悬停/拖动时在画布上显示形状预览
	void setShapePreviewHold(bool hold);
	void pulseShapePreview();
	void showPenTools();
	void showNumberTools();
	void showTextTools();
	void showMosaicTools();
	void showPatinaTools();
	void showWatermarkTools();
	void showHighlightTools();
	void showEraserTools();
	void showOpacityTools();   // 贴图「透明度」：仅滑条，无图标/色板
	// 演示画布「白板」属性栏：无色 / 调整背景 / 选择背景 + 颜色预设
	void showBoardTools();
	bool boardToolsVisible() const { return isVisible && hasTools && curToolId == L"board"; }
	// 演示画布内的屏幕录制属性栏
	void showDemoRecTools();
	void refreshDemoRecState();
	bool demoRecToolsBuilt() const { return hasTools && curToolId == L"demo-rec"; }
	bool demoRecGifMode() const { return demoRecGif_; }
	// 白板背景透明度（二级面板滑条调用）
	void setBoardOpacity(int percent);
	// 白板栏已构建但可能还没 show（isVisible=false）时也要允许布局放行
	bool boardToolsBuilt() const { return hasTools && curToolId == L"board"; }
	void refreshBoardState();

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
	// 序号工具的「当前序号」：落笔时取值。autoVal = 按已有形状推算的下一个值；
	// label 非空表示这一次直接用这段文字（支持中文）
	int takeNumberValue(int style, int autoVal, std::wstring& label);
	// 属性栏里的序号输入框当前应显示的文字（供外部刷新）
	std::wstring numberInputDisplay() const;
	RECT workAreaRect() const;
	// 视窗内滚轮：按工具量程微调主滑条
	bool adjustPrimarySize(int dir);
	// 属性栏是否在主栏上方（箭头朝下）；二级弹窗据此决定默认开向
	bool tipDown() const { return tipDown_; }
public:
	// 「渐隐画笔 / 画笔」是同级一级槽位，底层工具同为 pen，只差 isPenFade
	void setPenSlotMode(const std::wstring& slotId);
private:
	LRESULT onHitTest(const POINT pos) override; // 选区边/角穿透给底层 WinCap
public:
	// numberStyle: 0 数字 / 1 字母 / 2 Emoji（三种序号，无中文）
	enum NumberStyle : int { NumberDigit = 0, NumberLetter = 1, NumberEmoji = 2 };
	// arrowHead: 0 普通 / 1 大箭头 / 2 双箭头 / 3 标注箭头 / 4 趣味箭头（对齐产品属性气泡）
	enum ArrowHead : int { ArrowDefault = 0, ArrowBig = 1, ArrowBoth = 2, ArrowAnnot = 3, ArrowFun = 4 };
	bool isRectFill{ false }, isEllipse{ false }, isRectDash{ false };
	bool isArrowFill{ true }, isNumberFill{ true };
	bool isLine{ false }; // 箭头属性栏内切换直线（无独立主栏按钮）
	bool isArrowDash{ false }, isArrowRound{ false };
	bool isPenDash{ false }, isPenSoft{ false }, isPenFade{ false };
	// 荧光笔：与普通画笔共用同一个粗细滑条（显示 4 时实际 20）、半透明
	bool isPenMarker{ false };
	int penMarkerOpacity{ 35 };   // 荧光笔不透明度（10–100）
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
	float cornerRadius{ 0.f }; // 0–100：0 = 直角，100 = 该形状能到的最大圆角
	float lastCornerRadius{ 10.f }; // 关圆角后再开时恢复
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
	// 切换荧光笔（与画笔共用粗细滑条，只是画出来按倍数加宽）
	void applyPenMarkerState(bool on);
	// 序号输入框：日常显示下一个序号，点进去可改成数字或任意文字（含中文）
	void buildNumberInput();
	void syncNumberInput();
	void applyNumberInputEdited();
	float measureNumberInputW(const std::wstring& text) const;
	int autoNextNumber(int style) const;
	static std::wstring formatNumberLabel(int style, int val);
	void makeArrowHeadBtn(const wchar_t* icon, int style, const wchar_t* tipKey);
	void makeArrowHeadBtns();
	void refreshArrowHeadBtns();
	void makeRectStyleBtns();
	// 矩形/多边形/星形三个形态按钮的选中态（椭圆走 makeToggleBtn 自己的 flag）
	void refreshRectKindBtns();
	// 圆角按钮：椭圆没有圆角 → 置灰
	void refreshRoundBtnStyle();
	// —— 标注属性栏形态按钮的选中态 ——
	void refreshLineKindBtns();
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
	// —— 序号工具：属性栏输入框与「下一次落笔」的取值 ——
	Ling::TextBox* numberInput{ nullptr };
	int numberStartVal{ 0 };              // >0 = 手动指定的起始序号（落笔后自增）
	std::wstring numberCustomLabel;       // 非空 = 下一次落笔直接使用这段文字
	bool numberInputSyncing{ false };     // 程序回写输入框时屏蔽 onTextChanged
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
	std::vector<int> numberStyleVals;   // 与 numberStyleBtns 一一对应
	std::vector<Ling::Button*> arrowHeadBtns;
	std::vector<int> arrowHeadStyles;   // 与 arrowHeadBtns 一一对应（默认形态会换成「基础箭头」）
	std::vector<std::pair<Ling::Button*, int>> lineKindBtns;   // 标注形态按钮（气泡/角度/波浪线/尺寸）
	Ling::Button* bubbleKindBtn{ nullptr };
	Ling::Button* angleKindBtn{ nullptr };
	Ling::Button* waveKindBtn{ nullptr };
	Ling::Button* dimKindBtn{ nullptr };
	Ling::Button* fillBtn{ nullptr };
	Ling::Button* dashBtn{ nullptr };
	Ling::Button* softBtn{ nullptr };
	Ling::Button* lineBtn{ nullptr };   // 箭头属性栏的「线段」临时开关
	Ling::Button* markerBtn{ nullptr };
	Ling::Button* penBaseBtn{ nullptr };   // 画笔形态默认不是常规笔时，属性栏里的「常规画笔」
	Ling::Button* roundBtn{ nullptr };
	// 矩形工具的形态按钮（椭圆 / 多边形 / 星形；矩形是默认态）
	Ling::Button* ellipseBtn{ nullptr };
	Ling::Button* rectBtn{ nullptr };   // 形态默认不是矩形时，属性栏里的「基础矩形」
	Ling::Button* polyBtn{ nullptr };
	Ling::Button* starBtn{ nullptr };
	Ling::Button* angleBtn{ nullptr };
	Ling::Button* patinaWmBtn{ nullptr };
	Ling::Button* emojiTrigger{ nullptr };
	// 白板属性栏
	Ling::Button* boardAdjustBtn{ nullptr };
	Ling::Button* boardPickBtn{ nullptr };
	Ling::Button* boardNoColorBtn{ nullptr };
	std::vector<Ling::Button*> boardSwatches;   // 7 个常驻色块（含自定义）
	std::vector<Ling::Node*> boardRings;        // 选中态画在环上
	Ling::Canvas* boardCustomCanvas{ nullptr }; // 自定义色块的七彩渐变（与颜色面板共用绘制）
	Ling::Button* boardOpacityBtn{ nullptr };   // 「透明度」二级属性触发按钮（纯色背景可用）
	// —— 演示画布录屏属性栏 ——
	Ling::Button* recPlayBtn{ nullptr };
	Ling::Button* recAudioBtn{ nullptr };
	Ling::Button* recMicBtn{ nullptr };
	Ling::Button* recGifBtn{ nullptr };
	Ling::Button* recSaveBtn{ nullptr };
	Ling::Button* recCancelBtn{ nullptr };
	Ling::Button* recDoneBtn{ nullptr };
	Ling::Label* recTimerLabel{ nullptr };
	// 属性栏里选择的录制模式/音频开关（开录前可改，开录后锁定）
	bool demoRecGif_{ false };
	bool demoRecAudio_{ true };
	bool demoRecMic_{ false };
	// 复合图标按钮（马赛克模糊）：底字形 + 半透明叠字形
	Ling::Button* overlayBtn{ nullptr };
	Ling::Label* overlayBaseLabel{ nullptr };
	Ling::Label* overlayTopLabel{ nullptr };
	float overlayAlpha{ 0.25f };
	bool* overlayFlag{ nullptr };
	float sizeExtraW{ 0.f };
	// 仅本会话记忆；不写盘
	std::unordered_map<std::wstring, float> sliderMem;
	std::unordered_map<std::wstring, UINT> colorIndexMem;
	UINT32 sessionCustomColor{ 0xFF272CFF };
};
