#pragma once
#include <include/Ling.h>
#include "ToolbarTheme.h"

class WinCap;
// 二维码/条码结果卡：一个**不抢焦点**的浮层（WS_EX_NOACTIVATE），
// 里面是框架自己的 TextBox + Button —— 光标、停悬、命中、折行都由控件负责，
// 不在绘制层里手搓第二套。气泡外壳（圆角 + 45° 尖角 + 描边）直接调
// ToolbarChrome::paintPropBubble，与工具栏属性气泡逐像素同源。
class ToolQrcode : public Ling::WinBase
{
public:
	// anchor：按钮（或码）的客户区物理矩形。卡片左对齐它，尖角对准它中心。
	ToolQrcode(WinCap* win, std::wstring text, bool url, D2D1_RECT_F anchor);
	~ToolQrcode();
	void applyGeometry();
	const std::wstring& text() const { return text_; }
	bool url() const { return url_; }
protected:
	void onCreated() override;
	void layout() override;
	LRESULT onHitTest(const POINT pos) override;
	void onMinMaxInfo(MINMAXINFO* mmi) override;
private:
	void paintBubble();
private:
	WinCap* win_;
	std::wstring text_;
	bool url_{ false };
	D2D1_RECT_F anchor_{};
	Ling::Canvas* bubble_{ nullptr };
	Ling::TextBox* content_{ nullptr };
	Ling::Button* copy_{ nullptr };
	float cardW_{ 280.f }, cardH_{ 80.f };
	float caretX_{ 0.f };
	bool tipDown_{ true };
	bool linkHover_{ false };
	uint32_t baseColor_{ 0 };   // 正文常态色（网址也是这个色，停悬才变蓝）
	POINT downPos_{};
	bool downMoved_{ false };
	static constexpr float pad{ 12.f };
	static constexpr float btnSize{ 30.f };
	static constexpr float btnInset{ 8.f };
	static constexpr float btnFont{ 20.f };
	static constexpr float fontSize{ 13.f };
	static constexpr uint32_t linkColor{ 0x597ef7ff };
};
