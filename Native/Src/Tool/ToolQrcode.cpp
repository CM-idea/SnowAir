#include "pch.h"
#include "../Win/WinCap.h"
#include "../Lang.h"
#include "ToolQrcode.h"
#include "IconCodes.h"
#include "ToolbarChrome.h"
#include "ToolbarTheme.h"
#include <algorithm>

using namespace Microsoft::WRL;

ToolQrcode::ToolQrcode(WinCap* win, std::wstring text, bool url, D2D1_RECT_F anchor)
	: Ling::WinBase(), win_(win), text_(std::move(text)), url_(url), anchor_(anchor)
{
	dpi = win->dpi;
	// 卡片不订阅键盘：它自己不需要键盘，也就不存在"把按键转给截图窗口"这条路径。
	// 正文是纯文本时，卡片允许被激活（拖选 / Ctrl+C 走 TextBox 自己那套）；
	// 网址卡是不抢焦点的浮层（WS_EX_NOACTIVATE），点正文=开链接，永远收不到按键。
	if (!url_) {
		onKeyDown.add([this](UINT key) {
			if (key != VK_ESCAPE || !win_) return;
			win_->closeQrcodeCard();
			win_->refresh();
		});
	}
}

ToolQrcode::~ToolQrcode()
{
	// WinBase 析构不销毁 HWND；直接 reset 会留下"幽灵窗"，重入已释放对象
	if (hwnd) { close(); hwnd = nullptr; }
}

void ToolQrcode::applyGeometry()
{
	const float d = (win_ && win_->dpi > 0.f) ? win_->dpi : 1.f;
	dpi = d;
	const float caret = ToolbarTheme::caretSize;
	const float winW = cardW_ * d;
	const float winH = (cardH_ + caret) * d;
	const float edge = 4.f * d;
	const float gapPx = 5.f * d;

	// 默认挂在锚点上方（尖角朝下）；上面放不下就翻到下方
	tipDown_ = true;
	float top = anchor_.top - gapPx - winH;
	if (top < edge) {
		tipDown_ = false;
		top = anchor_.bottom + gapPx;
	}
	if (top < edge) top = edge;

	// 默认以锚点（按钮）为中心左右居中；贴边时横向避让，尖角跟着锚点走
	float left = (anchor_.left + anchor_.right) * 0.5f - winW * 0.5f;
	left = std::clamp(left, edge, (std::max)(edge, (win_ ? win_->w : 1920.f) - winW - edge));
	const float caretPad = (ToolbarTheme::borderRadius + caret) * d;
	caretX_ = std::clamp((anchor_.left + anchor_.right) * 0.5f - left, caretPad,
		(std::max)(caretPad, winW - caretPad));

	setSize(cardW_, cardH_ + caret);
	setPosition((win_ ? win_->x : 0) + (int)std::lround(left), (win_ ? win_->y : 0) + (int)std::lround(top));
}

void ToolQrcode::onCreated()
{
	// 整窗透明：气泡外壳由下面的 Canvas 画（与属性气泡同一套绘制）
	body->setBg(0);
	bubble_ = body->makeChild<Ling::Canvas>();
	bubble_->setPositionType(Ling::Position::Absolute);
	bubble_->setSizePercent(100.f, 100.f);

	ToolbarTheme::refresh();
	const float barTop = tipDown_ ? 0.f : ToolbarTheme::caretSize;
	const uint32_t textColor = (ToolbarTheme::iconNormal & 0xFFFFFF00u) | 0xFFu;

	content_ = body->makeChild<Ling::TextBox>();
	content_->setPositionType(Ling::Position::Absolute);
	content_->setPosition(Ling::Edge::Left, pad);
	content_->setPosition(Ling::Edge::Top, barTop + pad);
	content_->setPadding(0.f);
	content_->setBg(0);
	content_->setFontSize(fontSize);
	content_->setReadOnly(true);
	content_->setVerticalCenter(false);
	// 与文字识别浮层同一套选中样式：蓝底白字
	content_->setSelectionBgColor(0x1890FFFF);
	content_->setSelectionTextColor(0xFFFFFFFF);
	baseColor_ = textColor;
	if (text_.empty()) {
		content_->setColor(Icon::ColorDisabled);
		content_->setText(Lang::get(L"cap.qrcodeEmpty"));
	}
	else {
		// 网址也按正文色显示，停悬才变蓝（见 onMouseMove）
		content_->setColor(baseColor_);
		content_->setText(text_);
	}

	if (!text_.empty()) {
		copy_ = body->makeChild<Ling::Button>();
		copy_->setText(Icon::Copy);
		copy_->setSize(btnSize, btnSize);
		copy_->setPositionType(Ling::Position::Absolute);
		copy_->setPosition(Ling::Edge::Right, btnInset);
		copy_->setPosition(Ling::Edge::Top, barTop + btnInset);
		copy_->setBg(0);
		copy_->setHoverBg(ToolbarTheme::hoverBg);
		copy_->setBorderRadius(ToolbarTheme::hoverRadius);
		copy_->setAlignItems(Ling::Align::Center);
		copy_->setJustifyContent(Ling::Justify::Center);
		copy_->setFontFamily(Icon::Family);
		copy_->setFontSize(btnFont);
		copy_->setColor(Icon::ColorNormal);
		copy_->setHoverColor(Icon::ColorNormal);
		copy_->onClick.add([this](Ling::Button*) {
			// 复制 = 完成：写剪贴板并收工（与 Enter / Ctrl+C 同一入口，见 WinCap::runCommand）
			if (win_) win_->finishWithQrCard();
		});
	}

	// 光标：只有压在正文文字上（且是网址）才是手型；按钮自己会管自己的光标
	onCursor.add([this](bool* handled) {
		if (linkHover_) { SetCursor(LoadCursor(nullptr, IDC_HAND)); *handled = true; }
	});
	onMouseMove.add([this](POINT pos) {
		const bool over = url_ && content_ && content_->isPosOverText(pos);
		if (over != linkHover_) {
			linkHover_ = over;
			// 网址：常态白字，停悬变蓝
			if (content_) content_->setColor(over ? linkColor : baseColor_);
			if (hwnd) SendMessage(hwnd, WM_SETCURSOR, (WPARAM)hwnd, MAKELPARAM(HTCLIENT, 0));
		}
	});
	onMouseDown.add([this](POINT pos, bool) {
		downPos_ = pos;
		downMoved_ = false;
	});
	onMouseMove.add([this](POINT pos) {
		if (std::abs(pos.x - downPos_.x) > 4 || std::abs(pos.y - downPos_.y) > 4) downMoved_ = true;
	});
	if (url_) {
		// 单击正文 = 打开链接并退出截图；拖选（拖过 4px）不触发
		onMouseUp.add([this](POINT pos, bool isRight) {
			if (isRight || downMoved_ || !content_ || !content_->isPosOverText(pos)) return;
			if (win_) win_->openQrCardLink();
		});
	}

	{
		const float reserve = text_.empty() ? 0.f : (btnSize + btnInset * 2.f);
		const float w = (std::max)(40.f, cardW_ - pad * 2.f - reserve);
		const float h = (std::max)(20.f, cardH_ - pad * 2.f);
		if (content_) content_->setSize(w, h);
	}
	// 出现时不抢焦点：文本卡可以被点击激活，但不该一出现就把键盘从截图窗口抢走
	ShowWindow(hwnd, SW_SHOWNOACTIVATE);
}

void ToolQrcode::layout()
{
	Ling::WinBase::layout();
	paintBubble();
}

void ToolQrcode::paintBubble()
{
	if (!bubble_ || !hwnd) return;
	auto* ctx = bubble_->startPaint();
	if (!ctx) return;
	ctx->Clear(0);
	ComPtr<ID2D1SolidColorBrush> bg;
	const uint32_t cardBg = (ToolbarTheme::background & 0xFFFFFF00u) | 0xF0u;
	if (SUCCEEDED(ctx->CreateSolidColorBrush(Ling::Color(cardBg).getD2DColor(), bg.GetAddressOf())) && bg) {
		// 与工具栏属性气泡同一套：圆角 10 + 45° 尖角 + 1px 描边，尖角半宽 = 高度
		ToolbarChrome::paintPropBubble(ctx, w, h, dpi, bg.Get(), caretX_, tipDown_);
	}
	bubble_->finishPaint();
}

LRESULT ToolQrcode::onHitTest(const POINT pos)
{
	(void)pos;
	return HTCLIENT;
}

void ToolQrcode::onMinMaxInfo(MINMAXINFO* mmi)
{
	mmi->ptMinTrackSize.x = 1;
	mmi->ptMinTrackSize.y = 1;
}
