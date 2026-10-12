#pragma once
#include "AnnotHost.h"

class ShapeText;
class ToolOcr;
// 桌面钉图：AnnotHost 标注 + 拖移/缩放。框选阶段标注在 WinCap 上，不再 lockToCap。
// 非编辑态悬停显示锁/编辑/关闭；编辑态用精简 ToolCap（确认保留矢量 / 取消回滚本轮）。
// 另含：右键菜单、双击缩略图、滚轮/拖边缩放、不透明度、变换、专注/置顶、识别文字。
class WinPin : public AnnotHost
{
public:
	~WinPin();
	static void init(int x, int y, int w, int h, const std::wstring& initialTool = L"", std::unique_ptr<ToolCap> toolCap = nullptr);
	static void initFromData(int x, int y, int w, int h, std::vector<BYTE>& data);
	static bool hasWindow();
	static void dispose();
	void layoutTools() override;
	void forwardKey(UINT key) override;
	void copyToClipboard();
	void saveToFile();
	void bindToolChrome();
	void raiseToolChrome();
	void setEditing(bool on);
	void finishEditing(bool apply);
	void toggleBorderVisible();
	bool isEditing() const { return editing; }
	bool isLocked() const { return locked; }
	bool isBorderVisible() const { return borderVisible; }
	bool isThumbnail() const { return thumbnail; }
	int getOpacityPercent() const { return opacityPercent; }
	// 编辑工具栏「透明度」按钮回写入口
	void setOpacityPercent(int percent);
	// AnnotHost 接口：供 ToolSub 的「透明度」属性栏读写画面不透明度
	int  pinOpacityPercent() const override { return getOpacityPercent(); }
	void setPinOpacityPercent(int percent) override { setOpacityPercent(percent); }
private:
	enum class HoverBtn { None, Lock, Edit, Close };
	enum class ZoomMode { Direct, Crop };
	WinPin(int x, int y, int w, int h, const std::vector<BYTE>* data = nullptr, const std::wstring& initialTool = L"", std::unique_ptr<ToolCap> toolCap = nullptr);
	void captureEditBaseline();
	void restoreEditBaseline();
	void clearEditBaseline();
	void onCreated() override;
	void layout() override;
	void onMinMaxInfo(MINMAXINFO* mmi) override;
	void onDown(POINT pos, BOOL isRight);
	void onMove(POINT pos);
	void onUp(POINT pos, BOOL isRight);
	void onKey(UINT key);
	void onTimerCB(UINT id);
	void onClosed();
	BOOL setCursor() override;
	void applyEmptyToolCursor() override;
	void restoreWindowState(HWND foregroundBeforeDialog);
	void applyWinSize();
	D2D1_SIZE_U getImgSize() const;
	void applyScale(float newScale, POINT anchor);
	void paintScaleTip(ID2D1DeviceContext* ctx);
	void paintHoverChrome(ID2D1DeviceContext* ctx);
	void paintHoverBtn(ID2D1DeviceContext* ctx, const D2D1_RECT_F& r, IDWriteTextLayout* icon, uint32_t iconColor, bool hovered);
	D2D1_RECT_F lockBtnRect() const;
	D2D1_RECT_F closeBtnRect() const;
	D2D1_RECT_F editBtnRect() const;
	HoverBtn hitHoverBtn(POINT pos) const;
	Microsoft::WRL::ComPtr<IDWriteTextLayout> makeHoverIcon(const wchar_t* code);
	bool sizePreviewOrigin(float& cx, float& cy, float& halfSpan) const override;
	bool annotRightClick() override;
	bool annotDoubleClick() override;
	bool annotEmptyToolDrag(POINT pos) override;
	void annotAfterEmptyToolUp() override;

	// —— 本次新增功能 ——
	void showContextMenu(POINT screenPt);          // 右键菜单
	void enterThumbnail(POINT cursorClientPos);    // 进入缩略图
	void exitThumbnail();                          // 退出缩略图
	float effectiveOpacity() const;                // 画面层不透明度（悬停按 100%）
	void applyTopMost(bool on);
	void setFocusMode(bool on);
	void rotateImage90(bool clockwise);
	void mirrorImage(bool horizontal);
	void applyImageTransform(int quarterTurns, bool flipH); // 烘焙标注 + 变换底图（quarterTurns: +1=顺时针90, -1=逆时针90, 0=不转）
	void setZoomMode(ZoomMode m);
	D2D1_RECT_F cropSourceRect(float& f) const;    // 取景源矩形（图像像素）+ 缩放因子
	int hitResizeEdges(POINT pos) const;           // 命中的边位（0=无）
	void refreshOcrPanel();
	void toggleOcrPanel(bool copyText = true);
private:
	Ling::Canvas* canvas{ nullptr };
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> borderBrush;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hoverBtnBg;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hoverIconBrush;
	Microsoft::WRL::ComPtr<IDWriteTextLayout> scaleTip;
	Microsoft::WRL::ComPtr<IDWriteTextLayout> iconLock, iconUnlock, iconEdit, iconClose;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushTipBg, brushTipText;
	Microsoft::WRL::ComPtr<ID2D1Layer> opacityLayer;
	Microsoft::WRL::ComPtr<ID2D1Layer> clipLayer;                    // 圆角裁剪层（边框开启且圆角>0）
	Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> clipGeo;  // 与边框外缘一致的圆角形状
	float clipGeoW{ -1.f }, clipGeoH{ -1.f }, clipGeoRad{ -1.f };    // clipGeo 对应的窗口尺寸/圆角（变了才重建）
	bool isClosed{ false };
	bool dpiChanged{ false };
	bool editing{ false };
	bool locked{ false };
	bool borderVisible{ true }; // 对齐当前始终画绿边；可用工具栏开关关闭
	bool hoverChromeVisible{ false };
	HoverBtn hoverBtn{ HoverBtn::None };
	std::wstring pendingTool;
	// 进入编辑时的标注快照，取消时回滚
	size_t editBaselineSize{ 0 };
	std::vector<bool> editBaselineUndo;
	bool hasEditBaseline{ false };

	// —— 缩略图 ——
	bool thumbnail{ false };
	int preThumbX{ 0 }, preThumbY{ 0 }, preThumbW{ 0 }, preThumbH{ 0 };
	float preThumbScale{ 1.f };
	// —— 缩放模式 ——
	ZoomMode zoomMode{ ZoomMode::Direct };
	float cropScale{ 1.f };
	float cropCenterX{ -1.f }, cropCenterY{ -1.f };
	// —— 外观/状态 ——
	int opacityPercent{ 100 };
	bool topMost{ true };
	bool focusMode{ false };
	bool hiddenByFocus{ false };
	// —— 拖边缩放 ——
	int edgeResize{ 0 };
	POINT edgeStartGlobal{ 0, 0 };
	int edgeStartX{ 0 }, edgeStartY{ 0 }, edgeStartW{ 0 }, edgeStartH{ 0 };
	float edgeStartScale{ 1.f };
	// —— 识别文字 ——
	std::unique_ptr<ToolOcr> toolOcr;

	static constexpr float kBtn{ 32.f };
	static constexpr float kBtnGap{ 8.f };
	static constexpr float kBtnInset{ 10.f };
};
