#pragma once
#include "AnnotHost.h"
#include "CutMask.h"

class CapLong;
class CapVideo;
class ToolQrcode;
class ToolOcr;
// 录制期的选框层：独立的透明置顶窗，只画选框描边/控点/标签，并用 WDA_EXCLUDEFROMCAPTURE
// 排除在录制画面之外（主窗口还要录标注/鼠标高亮，不能整窗排除）。定义见 WinCap.cpp。
class SelectionChrome;
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
	// —— 演示画布：白板（背景）——
	void toggleDemoBoard();                       // 首次开启（白色铺满）/ 已开启则收展属性栏
	bool demoBoardActive() const { return demoBoard_.mode != DemoBoard::Mode::None; }
	bool demoBoardAdjusting() const { return demoBoard_.adjust; }
	// 白板「编辑背景」期间画的标注归入背景层（见 ShapeBase::backgroundLayer）
	bool boardBackgroundEditing() const override { return demoMode && demoBoard_.adjust; }
	// 白板属性栏是否处于"打开"意图：选别的工具只是临时把它替换掉，
	// 取消工具后要恢复回来（见 ToolCap::cancelSelect）
	bool demoBoardBarOpen() const { return demoBoardBarOpen_; }
	void restoreDemoBoardBar();
	bool demoBoardPpt() const { return demoBoard_.mode == DemoBoard::Mode::Ppt; }
	bool demoBoardImages() const { return demoBoard_.mode == DemoBoard::Mode::Images; }
	UINT32 demoBoardColor() const { return demoBoard_.color; }
	int demoBoardOpacity() const { return demoBoard_.opacity; }
	void demoBoardSetOpacity(int percent);       // 纯色背景不透明度 0–100
	int demoBoardPageCount() const { return (int)demoBoard_.pages.size(); }
	void demoBoardClear();                        // 无色：清背景（PPT 同时结束放映）
	void demoBoardSetColor(UINT32 color);
	void demoBoardPickColor();                    // 「自定义」取色
	void demoBoardPickBackground();               // 选择背景（图片多选 / PPT）
	void demoBoardToggleAdjust();                 // 调整背景开关（PPT 时不可用）
	void demoBoardStepPage(int delta);            // PgUp/PgDn
	// 背景尺寸的撤销/重做：page<0 表示纯色背景，否则是图片页码
	int demoBoardPageIndex() const;
	void demoBoardRestoreRect(int page, const D2D1_RECT_F& r);
	// —— 演示画布：屏幕录制 ——
	// 点工具栏「屏幕录制」：展开/收起专用属性栏（录制中再点只是重新摆出来）
	void toggleDemoRecorder();
	void demoRecToggle();          // 开始 / 暂停 / 继续
	void demoRecToggleAudio();
	void demoRecToggleMic();
	void demoRecToggleGif();
	void demoRecSaveAs();          // 停止 → 保存对话框 → 留在演示
	void demoRecCancel();          // 停止 → 丢弃 → 留在演示
	void demoRecFinish();          // 停止 → 文件进剪贴板 → 退出演示
	bool demoRecRunning() const { return demoRecRunning_; }
	bool demoRecPaused() const { return demoRecPaused_; }
	bool demoRecGifMode() const { return demoRecGif_; }
	bool demoRecAudioOn() const { return demoRecAudio_; }
	bool demoRecMicOn() const { return demoRecMic_; }
	int demoRecElapsed() const { return demoRecElapsed_; }
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
	// 会话开始那一刻前台窗口的应用名（不含扩展名）；供 {{FOCUS_WINDOW_APP_NAME}} 用
	const std::wstring& focusAppName() const { return focusAppName_; }
	// 「录制中显示按键」：全局键盘钩子把按键组合写进来（钩子回调在 UI 线程，可直接调用）
	void onGlobalKeyCombo(const std::wstring& text);
	// 「显示光标」工具按钮：截图/导出时把会话开始那一刻的系统光标一起拍进去
	void toggleShowCursor();
	// 「系统按键」派发：按键 → 命令表 → 与工具栏点击**同一批**入口。true = 已处理
	bool commandKey(UINT key);
	// 已拼好的 chord 版本：全局热键在会话进行中被按下时，先问会话里有没有命令接这个键位
	bool commandFromChord(const std::wstring& chord);
	// 执行一条命令（窗口内按键与全局钩子**共用**这一段）；查表在 commandKey
	bool runCommand(const std::wstring& id);
	// 当前界面掩码（截图 / 录屏 / 全屏画布）
	unsigned currentSurface() const;
	// 会话期间把「系统按键」里的临时全局键挂到钩子上；surface=0 表示撤掉。
	// static：调用方 beginDemoCanvas() 本身就是静态的；钩子回调也靠 get() 现取会话实例，
	// 不捕获 this（免得会话销毁后回调撞到悬空指针）。
	static void syncGlobalCommandKeys(unsigned surface);
	bool showCursorEnabled() const { return showCursor; }
	void hollowWin();
	void restoreWin();
	// 长截图：禁止选区挖洞 / WS_EX_TRANSPARENT 鼠标穿透
	void ensureNoMousePierce();
	// 选区被挪动（拖拽 / 鼠标跟随 / 方向键 / 滚轮）后由 CutMask 回调：
	// 录制中把平移同步给采集线程的裁剪框，否则画面里选区挪了、录出来还是原地
	void onMaskRectChanged();
	void startAnnotate(const std::wstring& toolId);
	void startPin(const std::wstring& toolId = L"");
	void startLong();
	void startVideo();
	void startOcr();
	void startTranslate();
	void clearOcrPanel();
	// 文字识别面板是否开着（工具栏据此决定该按钮是否显示选中态）
	bool hasOcrPanel() const { return toolOcr != nullptr; }
	// 当前浮层是「翻译」还是「文字识别」（无浮层时都返回 false）—— 工具栏据此点亮对应按钮
	bool isTranslatePanel() const;
	bool isOcrPanel() const;
	void startQrcode();
	void syncQrcodePanel();
	void syncOcrPanel();
	// keepOpen：演示画布用 —— 复制/保存后不关窗（画布只由 ✕ / Esc 退出）
	void saveToFile(bool keepOpen = false);
	void copyToClipboard(bool keepOpen = false);
	// 用户主动退出截图（Esc / 关闭按钮）：开启「截图自动保存」时先把当前选区落盘再关
	void closeByUser();
	bool isAnnotating() const { return annotLive; }
	void startMp4(bool useSpeaker, bool useMic);
	void startGif();
	std::wstring stopRecord();
	// 录屏收工（点 ✓ / 保存）时先隐藏屏幕上的那层：编码收尾要 join 采集线程，
	// 别让用户看着选框和遮罩多停留几百毫秒
	void hideRecordOverlay();
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
	// 截图 / 长图 / 录屏共用：按工具 id 打开属性栏并刷新布局
	void showAnnotToolBar(const std::wstring& toolId);
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
	// 放大镜此刻是否可见（绘制与 Ctrl+C 取色共用同一判据，避免"看得到提示却复制不到色值"）
	bool magnifierVisible() const;
	void paintPix(ID2D1DeviceContext* ctx);
	void samplePixColor();
	// 放大镜里的色值文本：Hex 模式是 #RRGGBB，RGB 模式只显示分量（255,0,0）
	std::wstring pixColorText() const;
	// Ctrl+C 复制用的色值文本：RGB 模式下补全成 rgb(255,0,0)
	std::wstring pixColorCopyText() const;
	void onKey(UINT key);
	// 录屏中（未穿透）按 Esc：先弹确认框，确认才结束并放弃本次录制
	void confirmExitRecording();
	void onWheel(POINT pos, float space);
	void copyCurrentStage();
	void onDown(POINT pos, bool isRight);
	void startOcrInternal(bool translateMode);
	void finishOcrTranslate(const struct OcrResult& merged, const struct TranslateResult& tr);
	// 记录"上一次识别结果"缓存（只允许在 UI 线程调用：后台识别线程通过 toUi 回投）
	void storeOcrCache(const struct OcrResult& r, const D2D1_RECT_F& rect);
	// 记录"上一次译文"缓存（同上，只在 UI 线程调用；只缓存成功的结果）
	void storeTranslateCache(const struct OcrResult& merged, const D2D1_RECT_F& rect,
		const std::wstring& key, const struct TranslateResult& tr);
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
	// applyEffects=false：不要「选区圆角/阴影」这类导出效果，拿原始选区像素（OCR 叠加层要用）
	bool exportSelection(std::vector<BYTE>& pixels, int& cw, int& ch, bool includeAnnot = true,
		bool applyEffects = true);
	// 记住本次选区（屏幕坐标），供全局热键「重复截图」重新抓同一块
	void rememberLastRegion();
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
	// 选区画幅是否已锁定（不许拖边/拖角改大小）：
	//   · 录屏已开录 —— 仍允许整体挪动（见 maskMoveLocked）
	//   · 长图已开始滚动（含暂停 / 编辑态）
	bool maskSizeLocked() const;
	// 连整体挪动是否也锁定：仅长图滚动中。录屏开录后不在其中，故与上面分开判定
	bool maskMoveLocked() const;
	// 「显示光标」：会话开始抓一次系统光标位图；屏幕上/导出图里按开关叠加
	void captureCursorSnapshot();
	void paintCursor(ID2D1DeviceContext* ctx);
	// 「录制中显示按键」：装/摘全局键盘钩子 + 画浮层
	void startKeyOverlay();
	void stopKeyOverlay();
	void paintKeyOverlay(ID2D1DeviceContext* ctx);
	void onKeyOverlayTimer();
	void compositeCursor(std::vector<BYTE>& pixels, int w, int h, int originX, int originY);
private:
	enum class CapStage { Select, Adjust, Long, Video };
	CapStage stage{ CapStage::Select };
	std::unique_ptr<CapLong> capLong;
	std::unique_ptr<CapVideo> capVideo;
	std::unique_ptr<ToolQrcode> toolQrcode;
	std::unique_ptr<ToolOcr> toolOcr;
	std::unique_ptr<SelectionChrome> chrome_;
	Ling::Canvas* canvas{ nullptr };
	POINT pixPos{}, pixFocus{};
	bool isPress{ false }, isClosed{ false }, isMouseTransparent{ false };
	// 「隐藏/显示工具栏」的当前状态（系统按键 F1，只有命令键能触发）。随会话重建，不落盘
	bool toolHidden{ false };
	// 托盘「屏幕录制」：本次会话框选完成后直接进录屏（不再弹截图工具栏）
	bool pendingRecord{ false };
	// 演示模式（全屏实时画布）：不画静帧、不画遮罩/选框，工具栏在顶部居中
	bool demoMode{ false };
	// —— 演示画布：白板背景 ——
	struct DemoBoardPage {
		std::wstring path;
		Microsoft::WRL::ComPtr<ID2D1Bitmap> bmp;
		D2D1_RECT_F rect{};              // 客户区坐标（原始尺寸居中）
		UINT32 srcW{ 0 }, srcH{ 0 };
	};
	struct DemoBoard {
		enum class Mode { None, Color, Images, Ppt } mode{ Mode::None };
		UINT32 color{ 0xFFFFFFFF };      // RRGGBBAA（默认白色）
		int opacity{ 100 };              // 纯色背景不透明度（0–100）
		D2D1_RECT_F rect{};              // 纯色背景矩形
		std::vector<std::shared_ptr<DemoBoardPage>> pages;
		int page{ 0 };
		bool adjust{ false };
		bool dragging{ false };
		int dragEdge{ 0 };               // 1 左 / 2 上 / 4 右 / 8 下
		POINT dragStart{};
		D2D1_RECT_F dragRect{};
		HWND pptWnd{ nullptr };          // PowerPoint 放映窗口（转发按键 / 结束放映）
		Microsoft::WRL::ComPtr<IDispatch> pptApp;   // PowerPoint / WPS 应用（晚绑定）
		Microsoft::WRL::ComPtr<IDispatch> pptShow;  // 当前放映窗 SlideShowWindow
		std::vector<D2D1_RECT_F> chips;  // 底部页码命中区
	};
	DemoBoard demoBoard_;
	bool demoBoardBarOpen_{ false };   // 白板属性栏是否应保持打开（选工具时只是临时替换）
	bool restoreLaserOnBoardBarClose_{ false };  // 开栏时临时取消了渐隐画笔，收起时还原
	// —— 演示画布：屏幕录制 ——
	bool demoRecBarOpen_{ false };     // 属性栏是否展开过（选别的工具会临时替换它）
	bool demoRecRunning_{ false };
	bool demoRecPaused_{ false };
	bool demoRecGif_{ false };
	bool demoRecAudio_{ true };
	bool demoRecMic_{ false };
	int demoRecElapsed_{ 0 };          // 已录秒数（暂停时不累加）
	static constexpr UINT demoRecTickTimerId{ 0x5601 };
	void startDemoRecording();
	// 停止采集并复位状态；deleteFile=true 时把临时文件删掉（取消）
	std::wstring stopDemoRecording(bool deleteFile);
	// 录屏入历史：历史只记视频路径，封面取收工这一刻的选区画面
	void addRecordHistory(const std::wstring& path, int durationSec);
	void closeDemoRecBar();
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushBoard_;
	void paintDemoBoard(ID2D1DeviceContext* ctx);
	D2D1_RECT_F demoBoardCurrentRect() const;
	bool demoBoardContains(POINT client) const;
	int demoBoardEdgeAt(POINT client) const;
	void demoBoardUpdateResize(POINT client);
	void demoBoardLoadImages(const std::vector<std::wstring>& paths);
	void demoBoardStartPpt(const std::wstring& path);
	void demoBoardStopPpt();
	HWND demoBoardPptWindow();
	void demoBoardForwardKey(UINT key);
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
	// 「查找元素」点选到的窗口元素（无拖拽即确认）→ 用 {{FOCUS_WINDOW_APP_NAME}} 与历史来源 focused
	bool selectionFromElement_{ false };
	// 会话开始前记录的前台应用名（用于 {{FOCUS_WINDOW_APP_NAME}}）
	std::wstring focusAppName_;
	// 「录制中显示按键」浮层：文本 + 过期时刻；约 1.5s 后自动清掉
	// 录制中按键提示：最多保留最近 3 条（新的追加，同一组合补全时替换最后一条）
	std::vector<std::wstring> keyOverlayLines_;
	ULONGLONG keyOverlayUntil_{ 0 };
	static constexpr UINT keyOverlayTimerId{ 0x5301 };
	static constexpr UINT pptRaiseTimerId{ 0x5401 };   // PPT 放映出窗后把画布抬回最上层
	// 白板"按点穿透"：窗口一旦带 WS_EX_TRANSPARENT 就收不到鼠标消息，
	// 只能定时轮询光标，按当前位置决定这一帧是让鼠标过去还是自己接收。
	static constexpr UINT boardPierceTimerId{ 0x5402 };
	static constexpr UINT boardPierceTickMs{ 30 };
	void demoBoardApplyPierce();               // 按光标位置切换整窗穿透样式
	void restoreTemporaryLaser();              // 白板属性栏收起时还原临时取消的渐隐画笔
	// 视频录制期间的光标覆盖物。穿透后本窗口收不到鼠标移动事件，故统一用定时器轮询光标：
	//   · 「选框鼠标跟随」：光标贴近选区边缘时自动平移选区
	//   · 「鼠标高亮提醒」：光标下垫一个半透明圆形色块，便于在录屏里找到光标
	static constexpr UINT recCursorTimerId{ 0x5302 };
	// 还没框选时 Ctrl+C 只取色：成功提示显示一会儿后自动退出截图
	static constexpr UINT colorCopyCloseTimerId{ 0x5501 };
	static constexpr UINT recCursorTickMs{ 16 };
	static constexpr float selFollowMargin{ 24.f };   // 判定"贴近边缘"的带宽（逻辑像素）
	// 跟随平移的平滑参数：每帧只走剩余越界距离的 ease 倍，并限制单帧最大位移
	static constexpr float selFollowEase{ 0.35f };
	static constexpr float selFollowMaxStep{ 24.f };  // 逻辑像素/帧
	static constexpr float mouseHlRadius{ 21.f };     // 高亮圆半径（逻辑像素，直径 42）
	static constexpr float kMouseHlAlpha{ 0.35f };    // 高亮圆的透明度（固定）
	void updateSelFollowCursor();
	void updateMouseHighlight();
	void paintMouseHighlight(ID2D1DeviceContext* ctx);
	POINT mouseHlPos_{ -1, -1 };   // 上次画高亮时的圆心（客户区，已含光标本体偏移）；没动就不重画
	UINT32 mouseHlColor_{ 0 };     // 笔刷对应的色号（颜色改了要重建笔刷）
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> mouseHlBrush_;
	ULONGLONG lastDownTime{ 0 };
	POINT lastDownPos{ 0, 0 };
	bool hideScreenImg{ false };
	// 工作线程抓好的 BGRA（top-down）：onCreated 用它建 screenImg，免去 UI 线程同步抓屏
	std::shared_ptr<std::vector<BYTE>> pendingGrab_;
	bool annotLive{ false };
	// 标注中拖选区边/角（与落笔分离，视窗可调）
	bool maskDragging{ false };
	COLORREF pixColor_{ 0 };
	bool pixColorRgb_{ false };   // 色值显示口径：false = #RRGGBB（默认），true = rgb(r, g, b)
	int pixSampleX_{ -1 }, pixSampleY_{ -1 };
	Microsoft::WRL::ComPtr<ID2D1Bitmap1> pixSampleBmp_;
	ULONGLONG pixCopiedUntil_{ 0 };   // 色值复制成功提示的截止时间（放大镜提示行变绿）
	// 右键将退出：等 RBUTTONUP 再 close，避免抬起落到桌面弹出系统菜单
	bool deferExitAfterRightUp{ false };
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushBg, brushShadow, brushText, brushMuted, brushHint, crossBrush;
	std::unique_ptr<class Tip> tip;
	InfoHit infoTipHit_{ InfoHit::None };
};
