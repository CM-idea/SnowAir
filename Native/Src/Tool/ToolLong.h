#pragma once
#include <include/Ling.h>
#include "IconCodes.h"
#include "ToolbarTheme.h"
#include "ToolbarWindow.h"

class WinCap;
// 长截图底栏：默认在选区底部右对齐；编辑态切成标注栏（无「显示边框」）
class ToolLong : public ToolbarWindow
{
public:
	ToolLong(WinCap* win);
	~ToolLong();
	// 采集状态：进行中 / 暂停 / 结束。autoActive 表示当前是自动滚动（手动滚动时按钮保持“未开始”的样子）。
	void syncCaptureState(bool capturing, bool paused, bool finished, bool autoActive);
	bool isEditMode() const { return editMode_; }
	void setEditMode(bool on);
	void cancelAnnotSelect();
	void updateUndoEnabled();
	// —— 「系统按键」用：绘图工具命令键 / 工具循环（长图阶段的绘图工具在本条工具条上）——
	void cmdAnnotTool(const std::wstring& id);   // 选中某件绘图工具（等同点击该按钮）
	void cycleAnnotTool();                       // 工具循环（Tab）
	float getBtnCenterX() const;
private:
	void onCreated() override;
	void onDpiAdopted() override;
	void drawChrome(ID2D1DeviceContext* ctx) override;
	void refreshSize();
	void buildBar();
	void applyLongBtnStyle();
	void bindLongTip();
	void applyToggleStyle(Ling::Button* btn, bool selected);
	void applyAnnotSelect(const std::wstring& id);
	void onAnnotClick(const std::wstring& id);
	void onClick(Ling::Button* btn);
	void onDragDown(POINT pos, bool isRight);
	void onDragMove(POINT pos);
	void onDragUp(POINT pos, bool isRight);
	Ling::Button* makeIconBtn(const std::wstring& code, const std::wstring& id);
	Ling::Node* makeSpliter();
	float contentWidth() const;
	float iconSideInset() const;
	float dragMarginRight() const;
	float contentPadRight() const;
private:
	WinCap* win;
	bool dpiChanged{ false };
	bool editMode_{ false };
	bool draggingBar{ false };
	POINT dragMouseScreen{};
	int dragWinX{ 0 }, dragWinY{ 0 };
	Ling::Button* dragHandle{ nullptr };
	Ling::Button* btnLong{ nullptr };
	Ling::Button* btnUndo{ nullptr };
	std::vector<Ling::Button*> annotBtns;
	std::wstring curAnnotId_;
	bool capturing{ false };
	bool paused{ false };
	bool finished{ false };
	bool autoActive{ false };
	static constexpr float btnSize{ 42.f };
	static constexpr float toolbarRadius{ ToolbarTheme::borderRadius };
	static constexpr float hoverRadius{ ToolbarTheme::hoverRadius };
	static constexpr float hoverInset{ 4.f };
	static constexpr float spliterH{ 18.f };
	static constexpr float spliterW{ 1.f };
};
