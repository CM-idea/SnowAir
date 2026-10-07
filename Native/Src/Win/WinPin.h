#pragma once
#include "AnnotHost.h"

class ShapeText;
// 桌面钉图：AnnotHost 标注 + 拖移/缩放。框选阶段标注在 WinCap 上，不再 lockToCap。
// 非编辑态悬停显示锁/编辑/关闭；编辑态用精简 ToolCap（确认保留矢量 / 取消回滚本轮）。
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
private:
	enum class HoverBtn { None, Lock, Edit, Close };
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
private:
	Ling::Canvas* canvas{ nullptr };
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> borderBrush;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hoverBtnBg;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hoverIconBrush;
	Microsoft::WRL::ComPtr<IDWriteTextLayout> scaleTip;
	Microsoft::WRL::ComPtr<IDWriteTextLayout> iconLock, iconUnlock, iconEdit, iconClose;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushTipBg, brushTipText;
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
	static constexpr float kBtn{ 32.f };
	static constexpr float kBtnGap{ 8.f };
	static constexpr float kBtnInset{ 10.f };
};
