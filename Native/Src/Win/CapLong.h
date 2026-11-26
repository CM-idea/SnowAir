#pragma once
#include <atomic>
#include <mutex>
#include <thread>
#include <include/Ling.h>
#include "LongMatch.h"

class WinCap;
class ToolLong;
// 滚动截图。
// 采集：一个常驻抓帧线程按“最新帧覆盖”的方式把画面交给拼接线程，拼接线程只认最新一帧，
//       因此手动快滚时不会因为排队而积压旧画面。
// 拼接：位移估计只用来预测，落点由“已拼画布”裁定（见 LongMatch.h），画布只增不减。
// 驱动：自动滚动优先直接设置目标窗口的滚动位置（位置由我们给定，因此是匀速的），
//       容器不支持时退回滚轮事件流；手动滚动完全交给用户，滚轮一动就自动接管。
// 侧栏预览随长图加长但不超过屏幕；悬停/拖预览时选区同步大图（这套交互保持不变）。
class CapLong
{
public:
	CapLong(WinCap* win);
	~CapLong();
	void dispose();
	void makeTool();
	void onMove(POINT pos);
	void onDown(POINT pos, bool isRight);
	void onUp(POINT pos);
	bool onWheel(POINT pos, float space);
	void onTimerCB(UINT timerId);
	void setCursor();
	void paint(ID2D1DeviceContext* ctx);
	void copyToClipboard();
	bool saveToFile();
	void pin();
	bool hasImage() const { return resultH > 0; }
	void layoutTool();
	void setToolVisible(bool visible);
	ToolLong* toolBar() const { return tool.get(); }
	void raiseTool();
	// 开始/暂停/继续：未开始时即开滚；滚动中暂停；暂停中再按继续（保持原自动/手动模式）
	void toggleCapture();
	void startCapture();             // 自动滚动
	void startManualScroll(float wheelSpace); // 用户滚轮：切手动并把这下滚轮转给页面
	void setPaused(bool on);
	// 鼠标停在右侧预览窗 = 临时暂停（手/自动滚动通用）：停滚动、停抓帧拼接，
	// 恢复遮罩在选区里显示大图预览；离开时原样接续。
	void enterPreviewHold();
	void exitPreviewHold();
	bool capturing() const { return isCapturing; }
	bool canAdjustSelection() const { return !isCapturing && !isFinish; }
	bool hitPreview(POINT clientPos) const;
	bool hitSelViewport(POINT clientPos) const;
	bool isEditMode() const;
	bool isPreviewPanning() const;
	bool isSelViewportPanning() const { return selViewportPanning; }
	bool beginSelViewportPan(POINT clientPos);
	void setEditMode(bool on);
	void cancelAnnotSelect();
	void notifyHistoryChanged();
	HWND toolHwnd() const;
	bool queryBarAnchor(float& x, float& y, float& w, float& h, float& btnCenterX, float& dpi) const;
	// 选区视口 ↔ 长图像素
	bool getSelViewMapping(float& scale, int& srcY, int& srcH) const;
	POINT clientToLongImg(POINT client) const;
	D2D1_POINT_2F longImgToClient(float ix, float iy) const;
	bool beginLongAnnotPaint(ID2D1DeviceContext* ctx, const D2D1_MATRIX_3X2_F& parentXform);
	bool beginLongPreviewAnnotPaint(ID2D1DeviceContext* ctx, const D2D1_MATRIX_3X2_F& parentXform);
	void endLongAnnotPaint(ID2D1DeviceContext* ctx, const D2D1_MATRIX_3X2_F& parentXform);
	bool exportPixels(std::vector<BYTE>& out);
private:
	// —— 采集 / 拼接 ——
	std::vector<BYTE> captureFrame();
	void startGrabThread();
	void stopGrabThread();
	void grabLoop(const RECT& sel);
	void publishFrame(std::vector<BYTE>&& data);
	bool takeFrame(std::vector<BYTE>& out);
	void startStitchThread();
	void stopStitchThread();
	void stitchLoop();
	void resetStitchState();
	void firstStep();
	void flushTail();
	// —— 驱动 ——
	void startCaptureMode(bool manual);
	void switchToAuto();
	void switchToManual();
	void startAutoScroll();
	void stopAutoScroll();
	void autoPulseTick();
	void pumpTick();
	// 自动滚动优先“按位置滚动”（UIA ScrollPattern）：位置由我们给定，没有应用自己的加速动画；
	// 容器不支持时才退回滚轮事件流。
	struct UiaScroll;
	bool initUiaScroll();
	void releaseUiaScroll();
	bool uiaDrive(double px);
	void resolveScrollTarget();
	HWND scrollTarget();
	bool postWheelStep(int delta);
	bool injectWheelStep(int delta);
	void sendWheelDelta(int delta);
	void installMouseHook();
	void removeMouseHook();
	static LRESULT CALLBACK mouseHookProc(int code, WPARAM wParam, LPARAM lParam);
	void noteUserWheel(POINT screenPt, int wheelDelta);
	void requestManualTakeover(POINT screenPt);
	void syncToolBtn();
	// —— 预览 / 交互 ——
	void paintImgPreview(ID2D1DeviceContext* ctx);
	void paintSelViewport(ID2D1DeviceContext* ctx);
	void paintStartHint(ID2D1DeviceContext* ctx);
	void paintTooFastHint(ID2D1DeviceContext* ctx); // 滚太快、这一帧拼不上时的临时提示
	void paintHintBadge(ID2D1DeviceContext* ctx, IDWriteTextLayout* layout);
	// 选区所在显示器的缩放：长截图遮罩铺满所有屏，提示文字必须按选区落点那块屏排版，
	// 不能用窗口自身的 dpi（那只代表其中一块屏）。
	float selDpi() const;
	bool makeImgPreview();
	void updatePreviewLayout();
	float previewFrameH() const;              // 绿框在缩略图里的高度（与选区高度同比）
	void syncPreviewPanToRatio(float ratio);  // 按比例滚动缩略图，保证绿框可见
	void setInspectRatio(float r, bool fromUser);
	void nudgeInspectByDelta(float dyPx);
	void scrubInspectAtClientY(float clientY);
	bool autoPanPreviewAtEdge(float clientY);
	void startPreviewDragPanTimer();
	void stopPreviewDragPanTimer();
	void syncPierceWithPreviewHot();
	void ensureSelPreviewBmp();
	void paintPreviewViewFrame(ID2D1DeviceContext* ctx);
	bool showSelViewport() const;
	bool isInspecting() const { return previewHot || previewPanning; }
	float viewRatio() const;
	float viewportRatio() const; // 当前采集视口在长图里的位置（0=顶，1=底）
	// —— 收尾 ——
	void stopCap();
	void makeStopText();
	void ensureExcludeFromCapture(bool on);
private:
	WinCap* win;
	bool isCapturing{ false };
	bool isPaused{ false };
	bool isFinish{ false };
	bool manualMode{ false };
	// 抓帧：单槽“最新帧”，拼接线程取走后置空
	std::thread grabThread;
	std::atomic<bool> grabRunning{ false };
	std::mutex frameMutex;
	std::vector<BYTE> latestFrame;
	bool latestFresh{ false };
	// 拼接：常驻线程 + 画布互斥
	std::thread stitchThread;
	std::atomic<bool> stitchRunning{ false };
	std::mutex canvasMutex;
	LongStitch::Stitcher stitcher;
	std::atomic<bool> previewDirty{ false };
	// 画面尺寸与画布快照（拼接线程写，UI 读）
	int imgW{ 0 }, imgH{ 0 };
	int resultH{ 0 };
	int viewportTop{ 0 };
	bool stoppedAtEnd{ false };
	bool hitMaxHeight{ false };
	bool excludedFromCapture{ false };
	bool toolChromeHidden{ false }; // 工具条被临时收起（拖边/拖移选区），右上角文字提示随之隐藏
	bool timerResRaised{ false };
	bool manualMagActive{ false }; // 抓帧线程当前是否走放大镜（决定能否恢复窗口）
	int capPipeline{ -1 };         // 抓帧管线：-1 未定，0 放大镜，1 DXGI，2 GDI
	HHOOK mouseHook{ nullptr };
	// 自动滚动：匀速。按拍投递很小的滚轮量，投递量/投递频率按“画布实际滚过的像素”闭环校准。
	double autoUnitsPerPulse{ 3.0 };    // 每个滚轮事件携带的滚轮单位（120 = 一个 notch）
	double autoPulseIntervalMs{ 12.0 }; // 相邻两个事件之间的间隔
	double autoPulseAccum{ 0.0 };       // 毫秒预算（不足一个间隔就不发）
	std::atomic<int> movedPx{ 0 };      // 上一次测速以来画布实际滚过的像素
	std::atomic<int> manualUnits{ 0 };  // 手动滚动时累计的用户滚轮单位（用来学刻度）
	ULONGLONG manualScaleMs{ 0 };
	int autoQuickSamples{ 0 };          // 还剩几个“快速收敛”采样窗口
	std::wstring rateKey_;              // 目标进程名：用来记住/复用这个应用的滚动刻度
	ULONGLONG autoPulseMs{ 0 };
	ULONGLONG speedSampleMs{ 0 };
	ULONGLONG autoStartMs{ 0 };
	HWND scrollTargetCache{ nullptr };
	ULONGLONG scrollTargetCacheMs{ 0 };
	std::atomic<ULONGLONG> lastMotionMs{ 0 }; // 最后一次确认画面在滚动的时刻
	bool autoUseInjection{ false };
	std::unique_ptr<UiaScroll> uia;    // 位置驱动（可用时优先于滚轮事件流）
	double uiaPxCarry{ 0.0 };          // 不足一个像素的余量，攒到下一拍
	// 供手动/自动切换用
	bool takeoverPending{ false };
	POINT scrollCenterScreen{};
	// 预览
	D2D1_RECT_F stopTextRect{};
	D2D1_POINT_2F stopTextPos{};
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> textBrush;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> bgBrush;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> viewportBgBrush;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> previewFrameBrush;
	Microsoft::WRL::ComPtr<IDWriteTextLayout> layoutTextEnd;
	Microsoft::WRL::ComPtr<IDWriteTextLayout> hintLayout;
	Microsoft::WRL::ComPtr<IDWriteTextLayout> hintTooFastLayout;
	float hintDpi{ 0.f };
	float hintTooFastDpi{ 0.f };
	std::atomic<ULONGLONG> tooFastUntilMs{ 0 }; // 这段时间内显示“滚得太快”提示
	int fastRejects{ 0 };                       // 连续“在动但拼不上”的帧数（拼接线程用）
	bool hintTooFastShown{ false };             // UI 侧记录当前是否在显示该提示
	Microsoft::WRL::ComPtr<ID2D1Bitmap1> imgPreview;
	Microsoft::WRL::ComPtr<ID2D1Bitmap1> fullPreviewBmp;
	// 选区大图只存“当前可见那一屏”的 1:1 像素，而不是整张长图。
	// 整张长图会超过 D2D 纹理上限，降采样后又会把标注背景糊掉。
	int fullPreviewW{ 0 };
	int fullPreviewSrcY{ -1 };
	int fullPreviewSrcH{ 0 };
	int fullPreviewRows{ 0 };
	ULONGLONG canvasStamp{ 0 };
	ULONGLONG fullPreviewStamp{ 0 };
	POINT capStartPos{};
	// 预览视窗（客户区）；高度随长图加长但钳在屏幕内，超出则平移
	D2D1_RECT_F previewViewRect{};
	float previewFullH{ 0.f };
	float previewViewH{ 0.f };
	float previewPanY{ 0.f };
	float inspectRatio{ 1.f }; // 0=顶 1=底；侧栏未溢出时用此浏览长图
	bool previewFollowBottom{ true };
	bool previewFollowTop{ false };
	bool previewHot{ false };
	// 悬停预览触发的临时暂停（独立于用户主动暂停 isPaused）
	bool previewHold{ false };
	bool previewPanning{ false };
	bool selViewportPanning{ false };
	POINT selPanLast{};
	ULONGLONG lastPreviewMs{ 0 };
	ULONGLONG lastFullPreviewMs{ 0 };
	std::unique_ptr<ToolLong> tool;
};
