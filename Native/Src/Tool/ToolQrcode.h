#pragma once
#include <include/Ling.h>
#include "ToolbarTheme.h"

class WinCap;
class Tip;
// 二维码/条码结果：透明窗盖文字/复制钮；白底由选区画在控点下层
class ToolQrcode : public Ling::WinBase
{
public:
	ToolQrcode(WinCap* win, std::wstring text);
	~ToolQrcode();
	void syncToMask(const D2D1_RECT_F& maskRect, int hostX, int hostY, float hostDpi);
private:
	void onCreated() override;
	LRESULT onHitTest(const POINT pos) override;
	void onMinMaxInfo(MINMAXINFO* mmi) override;
	void onCopy();
	void onOpenLink();
	bool isHttpUrl() const;
private:
	WinCap* win;
	std::wstring text;
	std::unique_ptr<Tip> tip;
	Ling::TextBox* textBox{ nullptr };
	Ling::Button* copyBtn{ nullptr };
	bool linkHover{ false };
	bool btnHover{ false };
	static constexpr float pad{ 12.f };
	static constexpr float fontSize{ 14.f };
	static constexpr float btnSize{ 34.f };
	static constexpr float hoverInset{ 4.f };
	static constexpr uint32_t linkColor{ 0x597ef7ff };
};
