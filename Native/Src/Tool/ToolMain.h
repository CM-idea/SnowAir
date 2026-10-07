#pragma once
#include <include/Ling.h>
#include "IconCodes.h"
#include "ToolbarTheme.h"
class WinPin;
class Tip;
class ToolMain : public Ling::WinBase
{
public:
	ToolMain(WinPin* win);
	~ToolMain();
	static void init();
	float getBtnCenterX();
	void cancelSelect();
	void selectTool(const std::wstring& id);
public:
	std::wstring curId;
private:
	void onCreated() override;
	void layout() override;
	void onClick(Ling::Button* btn);
	void onMinMaxInfo(MINMAXINFO* mmi);
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
private:
	WinPin* win;
	bool dpiChanged{ false };
	bool draggingBar{ false };
	POINT dragGrabOffset{};
	Ling::Node* contentNode{ nullptr };
	Ling::Button* dragHandle{ nullptr };
	Ling::Canvas* chromeCanvas{ nullptr };
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushBg;
	static constexpr float btnSize{ 42.f };
	static constexpr float toolbarRadius{ ToolbarTheme::borderRadius };
	static constexpr float hoverRadius{ ToolbarTheme::hoverRadius };
	static constexpr float hoverInset{ 4.f };
	static constexpr float spliterH{ 24.f };
	static constexpr float spliterW{ 1.f };
	void styleToolbarBtn(Ling::Button* btn);
	std::vector<std::wstring> btnIds = { L"rect",L"arrow",L"number",L"text",L"mosaic", L"eraser",L"|",L"undo",L"|",L"save",L"close",L"clipboard" };
	std::vector<std::wstring> btnCodes = {
		Icon::Rect,Icon::Arrow,Icon::Number,Icon::Text,Icon::Mosaic,Icon::Eraser,L"|",
		Icon::Undo,L"|",
		Icon::Save,Icon::Cancel,Icon::Done
	};
	std::vector<Ling::Button*> btns;
	std::vector<Ling::Node*> splitterNodes;
	std::unique_ptr<Tip> tip;
};
