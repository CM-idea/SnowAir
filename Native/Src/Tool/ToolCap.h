#pragma once
#include <include/Ling.h>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "IconCodes.h"
#include "ToolPicker.h"
#include "ToolbarTheme.h"

class WinCap;
class WinPin;
class Tip;

// 截图统一工具条：标注 + 撤销 + 录屏/贴图/OCR/翻译/长截图/二维码 + 关闭/保存/完成。
// 框选与框选内标注都挂在 WinCap（遮罩）owned 链上；仅桌面钉图改绑 WinPin。
class ToolCap : public Ling::WinBase
{
public:
	ToolCap(WinCap* winCap);
	ToolCap(WinPin* winPin);
	~ToolCap();
	void attachToPin(WinPin* winPin, const std::wstring& initialTool = L"");
	// 钉图：隐藏截图专属钮、折叠相邻分隔线、切换确认/取消文案
	void applyPinLayout();
	// 演示模式（全屏画布）：按默认全屏布局的槽位重建工具条
	void applyDemoLayout();
	// 演示模式：穿透开关按钮的选中态
	void setDemoThroughState(bool on);
	bool isDemoLayout() const { return demoLayout; }
	float getBtnCenterX() const;
	void cancelSelect();
	void selectTool(const std::wstring& id);
	// 框选同表面标注：选中工具并打开属性栏（不移交窗口）
	void selectAnnotTool(const std::wstring& id);
	void bindOwner(HWND ownerHwnd);
	void hideHoverTip();
	void updateUndoEnabled();
	std::wstring resolveToolId(const std::wstring& slotId) const;
	// 当前槽位 id。演示模式下 laser / pen 是同级排他槽位，laser 经 resolveToolId
	// 映射到底层画笔工具（见 applyDemoLayout 的 pickedSub），所以标注引擎拿到的仍是 pen。
	std::wstring curId;
private:
	class AnnotHost* annotHost() const;
	void onCreated() override;
	void layout() override;
	void onMinMaxInfo(MINMAXINFO* mmi) override;
	LRESULT onHitTest(const POINT pos) override; // pos=屏幕坐标；选区边/角穿透给 WinCap
	void onClick(Ling::Button* btn);
	void onClickCap(Ling::Button* btn);
	void onClickPin(Ling::Button* btn);
	void onHoverMove(POINT pos);
	void scheduleHidePicker();
	void cancelHidePicker();
	void onPickerHideTimer();
	void pickSub(const std::wstring& slot, const std::wstring& subId);
	void updateSlotButton(const std::wstring& slot);
	void showToolSubForEffective(const std::wstring& eff);
	void showComingSoon();
	void applyNormalStyle(Ling::Button* btn);
	void applySelectedStyle(Ling::Button* btn);
	void applyActionColors(Ling::Button* btn);
	void refreshSize();
	void paintChrome();
	void initDragHandle();
	void onDragDown(POINT pos, bool isRight);
	void onDragMove(POINT pos);
	void onDragUp(POINT pos, bool isRight);
	float contentLeadWidth() const;
	float dragMarginRight() const;
	float contentPadRight() const;
	float iconSideInset() const;
	float splitterSlotW() const;
	std::vector<bool> visibleSlots() const;
	void applySlotVisibility();
	static bool isAnnotationTool(const std::wstring& id);
	static bool isCapFeature(const std::wstring& id);
	static bool isPinHidden(const std::wstring& id);
	static bool isPinOnly(const std::wstring& id);
	static bool hasSubTools(const std::wstring& slotId);
	static const std::vector<ToolPickItem>* getSubTools(const std::wstring& slotId);
private:
	WinCap* winCap{ nullptr };
	WinPin* winPin{ nullptr };
	bool demoLayout{ false };   // 演示模式工具条（槽位见 applyDemoLayout）
	bool dpiChanged{ false };
	bool draggingBar{ false };
	POINT dragGrabOffset{};
	std::unique_ptr<Tip> tip;
	std::unique_ptr<ToolPicker> picker;
	std::map<std::wstring, std::wstring> pickedSub;
	Ling::Button* hoverBtn{ nullptr };
	// 点按关闭气泡后，在离开该按钮前不再因停悬重开（二次停悬才弹）
	Ling::Button* hoverSuppress{ nullptr };
	static constexpr UINT pickerHideTimerId{ 210 };
	Ling::Node* contentNode{ nullptr };
	Ling::Button* dragHandle{ nullptr };
	Ling::Canvas* chromeCanvas{ nullptr };
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushBg;
	static constexpr float btnSize{ 42.f };
	static constexpr float toolbarRadius{ ToolbarTheme::borderRadius };
	static constexpr float hoverRadius{ ToolbarTheme::hoverRadius };
	static constexpr float hoverInset{ 4.f };
	static constexpr float spliterW{ 1.f };
	static constexpr float spliterH{ 24.f };
	void styleToolbarBtn(Ling::Button* btn);
	std::vector<std::wstring> btnIds = {
		L"border",L"|",
		L"show-cursor",
		L"rect",L"arrow",L"pen",L"text",L"number",L"mosaic",L"eraser",L"|",
		L"undo",L"|",
		L"extra",L"pin",L"ocr",L"translate",L"long",
		L"save",L"close",L"clipboard"
	};
	std::vector<std::wstring> btnCodes = {
		Icon::DashRect,L"|",
		Icon::Cursor,
		Icon::Rect,Icon::Arrow,Icon::Pen,Icon::Text,Icon::Number,Icon::Mosaic,Icon::Eraser,L"|",
		Icon::Undo,L"|",
		Icon::Video,Icon::Pin,Icon::Ocr,Icon::Translate,Icon::LongShot,
		Icon::Save,Icon::Cancel,Icon::Done
	};
	std::vector<std::wstring> btnTips = {
		L"pin.showBorder",L"",
		L"tool.showCursor",
		L"tool.rect",L"tool.arrow",L"tool.pen",L"tool.text",L"tool.number",L"tool.mosaic",L"tool.eraser",L"",
		L"tool.undo",L"",
		L"cap.video",L"tool.pin",L"",L"tool.translate",L"cap.long",
		L"tool.save",L"tool.close",L"tool.clipboard"
	};
	std::vector<Ling::Button*> btns;
	std::vector<Ling::Node*> splitterNodes;
};
