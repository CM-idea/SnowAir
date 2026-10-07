#pragma once
#include <include/Ling.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct ToolPickItem {
	std::wstring id;
	const wchar_t* icon{ nullptr };
	const wchar_t* tipKey{ nullptr };
};

// 主栏悬停子工具：外壳复用 ToolbarChrome::paintPropBubble（与属性栏同款，箭头朝下）
class ToolPicker : public Ling::WinBase
{
public:
	ToolPicker();
	~ToolPicker();
	using PickFn = std::function<void(const std::wstring&)>;
	void showFor(Ling::WinBase* toolbar, Ling::Button* anchor, const std::vector<ToolPickItem>& items,
		const std::wstring& selectedId, PickFn onPick);
	void hidePicker();
	void reposition();
	bool isOpen() const { return isVisible; }
	bool hitTestScreen(POINT screenPt) const;
	bool hitKeepOpen(POINT screenPt) const;
	// 进入/离开气泡时回调（对齐 QT SubToolPopover::hoverChanged）
	std::function<void(bool inside)> onHoverChange;
private:
	void onCreated() override;
	void layout() override;
	void rebuildButtons();
	void placeNearAnchor();
	void applyRowInsets();
private:
	Ling::WinBase* toolbar{ nullptr };
	Ling::Button* anchor{ nullptr };
	Ling::Node* row{ nullptr };
	Ling::Canvas* canvas{ nullptr };
	std::unique_ptr<class Tip> tip;
	std::vector<ToolPickItem> items;
	std::vector<Ling::Button*> pickBtns;
	std::wstring selectedId;
	PickFn onPick;
	bool isVisible{ false };
	bool preferAbove{ true };
	float arrowX{ 0.f };
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushBg;
};
