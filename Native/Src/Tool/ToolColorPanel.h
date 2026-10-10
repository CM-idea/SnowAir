#pragma once
#include <include/Ling.h>
#include "ToolbarTheme.h"

class ToolSub;
// 颜色二级属性栏：主属性栏只留当前色块，点开后出现本窗。
class ToolColorPanel : public Ling::WinBase
{
public:
	explicit ToolColorPanel(ToolSub* owner);
	~ToolColorPanel();
	void open(const RECT& workArea);
	void closePanel();
	bool isOpen() const { return open_; }
	void refreshSelection();
	void updatePosition(const RECT& workArea);
	void hideHoverTip() { if (tip) tip->hide(); }
private:
	void onCreated() override;
	void layout() override;
	void onMinMaxInfo(MINMAXINFO* mmi) override;
	void paintChrome(ID2D1DeviceContext* ctx);
	void applyTipDirection(bool tipDown);
	void rebuildBtns();
	void paintCustomSwatch();
	void applyActiveStyle(size_t index, bool on);
	void onSwatchClick(Ling::Button* btn);
private:
	ToolSub* owner;
	Ling::Node* contentNode{ nullptr };
	Ling::Canvas* chromeCanvas{ nullptr };
	Ling::Canvas* customCanvas{ nullptr };
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushBg;
	std::vector<Ling::Button*> btns;
	std::unique_ptr<class Tip> tip;
	float arrowX{ 0.f };
	bool tipDown_{ false };
	bool open_{ false };
	static constexpr float btnSize{ ToolbarTheme::propBtnSize };
	static constexpr float hoverInset{ 4.f };
	static constexpr float iconInner{ ToolbarTheme::propIconInner };
	static constexpr float hoverRadius{ ToolbarTheme::hoverRadius };
	static constexpr float marginTop{ ToolbarTheme::caretSize };
	static constexpr float swatchSize{ ToolbarTheme::swatchSize };
	static constexpr float swatchRing{ ToolbarTheme::swatchRing };
	static constexpr float swatchRadius{ ToolbarTheme::swatchRadius };
	std::vector<Ling::Node*> rings;
};
