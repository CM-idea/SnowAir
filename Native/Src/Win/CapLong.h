#pragma once
#include <include/Ling.h>

class WinCap;
class ToolLong;
// 滚动截图。侧栏预览随长图加长但不超过屏幕；悬停/拖预览时选区同步大图。
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
	bool hasImage() const { return !imgData.empty(); }
	void layoutTool();
	void setToolVisible(bool visible);
	// 长图阶段的工具条（ToolLong）。与 CapVideo::toolBar() 同款：给「系统按键」的
	// 绘图工具 / 工具循环用 —— 长图阶段的工具在这条工具条上，不在截图条上。
	ToolLong* toolBar() const { return tool.get(); }
	void raiseTool();
	void toggleCapture();
	void startCapture();
	void setPaused(bool on);
	// 暂停/继续（「系统按键」截图组的「暂停/继续滚动」，默认 Space）
	void togglePaused();
	// 是否处于滚动截图会话中（含暂停与编辑态，它们属同一次滚动）。
	// 选区锁定判定用；⚠ 不可命名为 isCapturing（成员变量已占用该名字）
	bool capturing() const { return isCapturing; }
	void onPreviewPanChanged();
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
	void firstStep();
	void makeImgPreview();
	void capStep();
	bool tryAppendFrame(); // 暂停前再拼一帧；不调度下一轮滚动
	void paintStartBtn(ID2D1DeviceContext* ctx);
	void paintImgPreview(ID2D1DeviceContext* ctx);
	void paintSelViewport(ID2D1DeviceContext* ctx);
	void stopCap();
	void makeStopText();
	void syncToolBtn();
	bool showSelViewport() const;
	void ensureExcludeFromCapture(bool on);
	void resolveScrollTarget();
	void sendScrollWheel();
	void updatePreviewLayout();
	void setPreviewPanY(float y, bool fromUser);
	void setInspectRatio(float r, bool fromUser);
	void nudgeInspectByDelta(float dyPx);
	void nudgeInspectBySelDelta(float dyPx); // 选区相对拖动
	void scrubInspectAtClientY(float clientY); // 拖动：光标位置绝对映射预览位置
	bool autoPanPreviewAtEdge(float clientY); // 拖动贴近上下沿时缓慢滚缩略图
	void startPreviewDragPanTimer();
	void stopPreviewDragPanTimer();
	void syncPierceWithPreviewHot();
	void ensureSelPreviewBmp();
	bool isInspecting() const { return previewHot || previewPanning; }
	float previewTopRatio() const;
	float viewRatio() const; // 选区大图浏览位置
	void paintPreviewViewFrame(ID2D1DeviceContext* ctx);
private:
	WinCap* win;
	bool isShowStartBtn{ false };
	bool isCapturing{ false };
	bool isPaused{ false };
	bool isFinish{ false };
	bool firstCheck{ true };
	// 首帧如果走了 DXGI（HDR 屏友好），后续拼接帧也走它，避免同一张长图混两种色彩管线
	bool useDxgiCapture{ false };
	bool excludedFromCapture{ false };
	int dismissTime{ 0 };
	int settleRecheckCount{ 0 };
	int changeStartY{ -1 };
	D2D1_RECT_F stopTextRect{};
	D2D1_POINT_2F stopTextPos{};
	D2D1_SIZE_F startTextSize{};
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> textBrush;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> bgBrush;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> viewportBgBrush;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> previewFrameBrush;
	Microsoft::WRL::ComPtr<IDWriteTextLayout> layoutTextStart;
	Microsoft::WRL::ComPtr<IDWriteTextLayout> layoutTextEnd;
	Microsoft::WRL::ComPtr<ID2D1Bitmap1> imgPreview;
	Microsoft::WRL::ComPtr<ID2D1Bitmap1> fullPreviewBmp;
	float startCircleR{ 30.f };
	POINT circleCenter{};
	POINT scrollCenterScreen{};
	std::unique_ptr<ToolLong> tool;
	std::vector<BYTE> imgData;
	std::vector<BYTE> img1;
	int imgW{ 0 }, imgH{ 0 };
	int resultH{ 0 };
	POINT capStartPos{};
	// 预览视窗（客户区）；高度随长图加长但钳在屏幕内，超出则平移
	D2D1_RECT_F previewViewRect{};
	float previewFullH{ 0.f };
	float previewViewH{ 0.f };
	float previewPanY{ 0.f };
	float inspectRatio{ 1.f }; // 0=顶 1=底；侧栏未溢出时用此浏览长图
	bool previewFollowBottom{ true };
	bool previewHot{ false };
	bool previewPanning{ false };
	POINT previewPanLast{};
	bool selViewportPanning{ false };
	POINT selPanLast{};
	bool scrollPending_{ false }; // 已发滚轮、尚未 capStep
};
