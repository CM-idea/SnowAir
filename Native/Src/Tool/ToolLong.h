#pragma once
#include <include/Ling.h>
#include "IconCodes.h"
#include "ToolbarTheme.h"

class WinCap;
class Tip;
// 长截图底栏：默认在选区底部右对齐；编辑态切成标注栏（无「显示边框」）
class ToolLong : public Ling::WinBase
{
public:
	ToolLong(WinCap* win);
	~ToolLong();
	void syncCaptureState(bool capturing, bool paused, bool finished);
	bool isEditMode() const { return editMode_; }
	void setEditMode(bool on);
	void cancelAnnotSelect();
	void updateUndoEnabled();
	float getBtnCenterX() const;
private:
	void onCreated() override;
	void layout() override;
	void onMinMaxInfo(MINMAXINFO* mmi) override;
	void refreshSize();
	void paintChrome();
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
	Ling::Node* contentNode{ nullptr };
	Ling::Canvas* chromeCanvas{ nullptr };
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushBg;
	Ling::Button* dragHandle{ nullptr };
	Ling::Button* btnLong{ nullptr };
	Ling::Button* btnEdit{ nullptr };
	Ling::Button* btnUndo{ nullptr };
	std::vector<Ling::Button*> annotBtns;
	std::wstring curAnnotId_;
	bool capturing{ false };
	bool paused{ false };
	bool finished{ false };
	static constexpr float btnSize{ 42.f };
	static constexpr float toolbarRadius{ ToolbarTheme::borderRadius };
	static constexpr float hoverRadius{ ToolbarTheme::hoverRadius };
	static constexpr float hoverInset{ 4.f };
	static constexpr float spliterH{ 24.f };
	static constexpr float spliterW{ 1.f };
	std::unique_ptr<Tip> tip;
};
