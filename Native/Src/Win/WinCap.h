#pragma once
#include "AnnotHost.h"
#include "CutMask.h"

class CapLong;
class CapVideo;
class ToolQrcode;
class ToolOcr;
// 截图主窗口：全屏底图上框选 + 同表面标注；导出/贴图再按选区裁切。
class WinCap : public AnnotHost
{
public:
	~WinCap();
	// 进入截图会话时的起始形态（决定建窗后要不要接着切到录屏/演示画布）
	enum class StartMode { Normal, Record, Demo };
	static void init(StartMode mode = StartMode::Normal);
	// init 的第二段：抓屏在工作线程做完后回 UI 线程建窗（见 init 的实现注释）。
	static void initNow(StartMode mode, std::shared_ptr<std::vector<BYTE>> grab);
	static WinCap* get();
	static void dispose();
	static void stopIfRecording();
	// 托盘「屏幕录制」：照常进截图态（悬停找元素 → 拖框选），框完直接换成录屏工具栏
	static void beginRecordPick();
	// 托盘「演示画布」：整屏实时画布 + 顶部居中工具栏（全屏画布/演示模式）
	static void beginDemoCanvas();
	// 演示模式：选画笔（fade=true 为渐隐画笔/laser），并同步工具条按钮选中态
	void startPenSlot(bool fade);
	// 演示模式：穿透开关（让出鼠标给桌面，标注仍留在屏幕上）
	void toggleDemoThrough();
	// 演示模式：重置画布（清掉所有标注）
	void resetDemoCanvas();
	// 演示模式：退出演示（清掉标注并关掉窗口）
	void exitDemoCanvas();
	Microsoft::WRL::ComPtr<ID2D1Bitmap1> getCutImg();
	void layoutTool(Ling::WinBase* tool);
	void layoutTools() override;
	void onHistoryChanged() override;
	bool queryMainBarAnchor(MainBarAnchor& out) const override;
	void forwardKey(UINT key) override;
	void setMouseTransparent(bool transparent);
	bool mouseTransparent() const { return isMouseTransparent; }
	// 「显示光标」工具按钮：截图/导出时把会话开始那一刻的系统光标一起拍进去
	void toggleShowCursor();
	bool showCursorEnabled() const { return showCursor; }
	void hollowWin();
	void restoreWin();
	// 长截图：禁止选区挖洞 / WS_EX_TRANSPARENT 鼠标穿透
	void ensureNoMousePierce();
	void startAnnotate(const std::wstring& toolId);
	void startPin(const std::wstring& toolId = L"");
	void startLong();
	void startVideo();
	void startOcr();
	void startTranslate();
	void clearOcrPanel();
	// 文字识别面板是否开着（工具栏据此决定该按钮是否显示选中态）
	bool hasOcrPanel() const { return toolOcr != nullptr; }
	void startQrcode();
	void syncQrcodePanel();
	void syncOcrPanel();
	// keepOpen：演示画布用 —— 复制/保存后不关窗（画布只由 ✕ / Esc 退出）
	void saveToFile(bool keepOpen = false);
	void copyToClipboard(bool keepOpen = false);
	bool isAnnotating() const { return annotLive; }
	void startMp4(bool useSpeaker, bool useMic);
	void startGif();
	std::wstring stopRecord();
	void layoutLongTool();
	void longToggle();
	void longPreviewPanChanged();
	bool longSaveToFile();
	void longCopyToClipboard();
	// 真正开录：藏信息栏、洞穿选区、鼠标穿透
	void beginVideoCapture();
	void setRecordPaused(bool on);
	void startVideoAnnotate(const std::wstring& toolId);
	void clearVideoAnnotTool();
	void toggleLongEdit();
	void startLongAnnotate(const std::wstring& toolId);
	void clearLongAnnotTool();
	// 右键分层退出；fromToolbar=true 时最多退到未选工具。
	// 返回 true 表示应退出截图（由调用方在右键抬起后再 close，避免桌面再吃一次右键）
	bool handleLayerCancel(bool fromToolbar = false);
public:
	std::unique_ptr<CutMask> cutMask;
private:
	WinCap();
	void onCreated() override;
	void layout() override;
	BOOL setCursor() override;
	LRESULT onHitTest(const POINT pos) override;
	bool annotRightClick() override;
	bool annotSkipHitSelect() const override;
	bool annotEmptyToolDrag(POINT pos) override;
	void annotAfterEmptyToolUp() override;
	void applyEmptyToolCursor() override;
	void enterReselect();
	void requestExitAfterRightUp();
	void setPixPos(POINT pos);
	void paintPix(ID2D1DeviceContext* ctx);
	void samplePixColor();
	void onKey(UINT key);
	void onWheel(POINT pos, float space);
	void copyCurrentStage();
	void onDown(POINT pos, bool isRight);
	void startOcrInternal(bool translateMode);
	void finishOcrTranslate(const struct TranslateResult& tr);
	// 把选区设成整屏（演示画布用），并进入"已框选"阶段、弹出工具栏
	void selectFullScreenRect();
	// 演示模式专用：建工具条（Fullscreen 布局）并摆到屏幕顶部居中
	void makeDemoToolCap();
	void onMove(POINT pos);
	void onUp(POINT pos, bool isRight);
	void onClosed();
	void makeToolCap();
	bool enterByArg();
	void relayoutTool();
	void enterLiveStage();
	bool exportSelection(std::vector<BYTE>& pixels, int& cw, int& ch);
	bool beginMaskResize(POINT pos);
	void applyMaskHitCursor(MaskHit hit);
	bool hasAnnotShapes() const;
	void setToolChromeHidden(bool hidden);
	bool nudgeSelectionByOutsideWheel(POINT pos, int dir);
	bool nudgeSelectionByArrow(int dx, int dy);
	bool sizePreviewOrigin(float& cx, float& cy, float& halfSpan) const override;
	POINT toImgPos(const POINT& pos) const override;
	D2D1_POINT_2F annotToClientPt(float ax, float ay) const override;
	D2D1_POINT_2F clientToAnnotPt(float cx, float cy) const override;
	void updateInfoBarTip(POINT pos);
	// 录屏工具条已出、尚未点「开始」：与 Adjust 一样可拖/缩放选区
	bool videoCanAdjust() const;
	// 「显示光标」：会话开始抓一次系统光标位图；屏幕上/导出图里按开关叠加
	void captureCursorSnapshot();
	void paintCursor(ID2D1DeviceContext* ctx);
	void compositeCursor(std::vector<BYTE>& pixels, int w, int h, int originX, int originY);
private:
	enum class CapStage { Select, Adjust, Long, Video };
	CapStage stage{ CapStage::Select };
	std::unique_ptr<CapLong> capLong;
	std::unique_ptr<CapVideo> capVideo;
	std::unique_ptr<ToolQrcode> toolQrcode;
	std::unique_ptr<ToolOcr> toolOcr;
	Ling::Canvas* canvas{ nullptr };
	POINT pixPos{}, pixFocus{};
	bool isPress{ false }, isClosed{ false }, isMouseTransparent{ false };
	// 托盘「屏幕录制」：本次会话框选完成后直接进录屏（不再弹截图工具栏）
	bool pendingRecord{ false };
	// 演示模式（全屏实时画布）：不画静帧、不画遮罩/选框，工具栏在顶部居中
	bool demoMode{ false };
	bool dpiChanged{ false };
	// 「显示光标」：会话开始那一刻的系统光标位图 + 热点 + 屏幕位置（物理像素）
	bool showCursor{ false };
	bool cursorValid{ false };
	std::vector<BYTE> cursorPix;   // 直立 BGRA，straight alpha
	int cursorW{ 0 }, cursorH{ 0 };
	POINT cursorHotspot{ 0, 0 };
	POINT cursorScreenPos{ 0, 0 };
	Microsoft::WRL::ComPtr<ID2D1Bitmap1> cursorBmp;
	// 框选：仅真正拖动后才显示放大镜（单击不触发）
	bool selectDragging_{ false };
	ULONGLONG lastDownTime{ 0 };
	POINT lastDownPos{ 0, 0 };
	bool hideScreenImg{ false };
	// 工作线程抓好的 BGRA（top-down）：onCreated 用它建 screenImg，免去 UI 线程同步抓屏
	std::shared_ptr<std::vector<BYTE>> pendingGrab_;
	bool annotLive{ false };
	// 标注中拖选区边/角（与落笔分离，视窗可调）
	bool maskDragging{ false };
	COLORREF pixColor_{ 0 };
	int pixSampleX_{ -1 }, pixSampleY_{ -1 };
	Microsoft::WRL::ComPtr<ID2D1Bitmap1> pixSampleBmp_;
	ULONGLONG pixCopiedUntil_{ 0 };   // 色值复制成功提示的截止时间（放大镜提示行变绿）
	// 右键将退出：等 RBUTTONUP 再 close，避免抬起落到桌面弹出系统菜单
	bool deferExitAfterRightUp{ false };
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushBg, brushShadow, brushText, brushMuted, brushHint, crossBrush;
	std::unique_ptr<class Tip> tip;
	InfoHit infoTipHit_{ InfoHit::None };
};
