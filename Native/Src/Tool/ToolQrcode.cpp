#include "pch.h"
#include "../Win/WinCap.h"
#include "../Lang.h"
#include "../Tip.h"
#include "ToolQrcode.h"
#include "IconCodes.h"
#include "ToolbarTheme.h"

ToolQrcode::ToolQrcode(WinCap* win, std::wstring text)
	: Ling::WinBase(), win(win), text(std::move(text))
{
	dpi = win->dpi;
	onKeyDown.add([this](UINT key) {
		if (key == VK_RETURN && isHttpUrl() && (GetKeyState(VK_CONTROL) & 0x8000)) {
			onOpenLink();
			return;
		}
		if ((GetKeyState(VK_CONTROL) & 0x8000) && (key == 'A' || key == 'C' || key == 'X'))
			return;
		this->win->onKeyDown(key);
	});
}

ToolQrcode::~ToolQrcode() {}

bool ToolQrcode::isHttpUrl() const
{
	if (text.size() >= 8 && _wcsnicmp(text.c_str(), L"https://", 8) == 0) return true;
	if (text.size() >= 7 && _wcsnicmp(text.c_str(), L"http://", 7) == 0) return true;
	return false;
}

void ToolQrcode::onCreated()
{
	tip = std::make_unique<Tip>(this);
	body->setBg(0); // 透明：白底由 CutMask 画在控点/信息栏下面

	// TextBox 构造里写死 240×120；跟选区缩放时在 syncToMask 里 setSize，否则折行宽度不变
	textBox = body->makeChild<Ling::TextBox>();
	textBox->setPositionType(Ling::Position::Absolute);
	textBox->setPosition(Ling::Edge::Left, pad);
	textBox->setPosition(Ling::Edge::Top, pad);
	textBox->setPadding(0.f);
	textBox->setBg(0);
	textBox->setFontSize(fontSize);
	textBox->setReadOnly(true);
	textBox->setVerticalCenter(false);
	if (text.empty()) {
		textBox->setColor(Icon::ColorDisabled);
		textBox->setText(Lang::get(L"cap.qrcodeEmpty"));
	}
	else {
		textBox->setColor(isHttpUrl() ? linkColor : Icon::ColorNormal);
		textBox->setText(text);
		textBox->selectAll();
		textBox->focus();
	}

	if (!text.empty()) {
		copyBtn = body->makeChild<Ling::Button>();
		copyBtn->setText(Icon::Copy);
		copyBtn->setSize(btnSize, btnSize);
		copyBtn->setPositionType(Ling::Position::Absolute);
		copyBtn->setPosition(Ling::Edge::Right, hoverInset);
		copyBtn->setPosition(Ling::Edge::Top, hoverInset);
		copyBtn->setBg(0);
		copyBtn->setHoverBg(ToolbarTheme::hoverBg);
		copyBtn->setBorderRadius(ToolbarTheme::hoverRadius);
		copyBtn->setAlignItems(Ling::Align::Center);
		copyBtn->setJustifyContent(Ling::Justify::Center);
		copyBtn->setFontFamily(Icon::Family);
		copyBtn->setFontSize(Icon::Size);
		copyBtn->setColor(Icon::ColorNormal);
		copyBtn->setHoverColor(Icon::ColorNormal);
		copyBtn->onClick.add([this](Ling::Button*) { onCopy(); });
		onTimer.add([this](UINT id) {
			if (id != 1) return;
			killTimer(1);
			win->close();
		});
		copyBtn->onEnter.add([this](Ling::Button*) {
			btnHover = true;
			if (hwnd) SendMessage(hwnd, WM_SETCURSOR, (WPARAM)hwnd, MAKELPARAM(HTCLIENT, 0));
		});
		copyBtn->onLeave.add([this](Ling::Button*) {
			btnHover = false;
			if (hwnd) SendMessage(hwnd, WM_SETCURSOR, (WPARAM)hwnd, MAKELPARAM(HTCLIENT, 0));
		});
		tip->bind(copyBtn, Lang::get(L"cap.qrcodeCopyBtn"));
	}

	onMouseMove.add([this](POINT pos) {
		const bool overBtn = copyBtn && copyBtn->isPosIn(pos);
		const bool overGlyph = textBox && textBox->isPosOverText(pos) && !overBtn;
		const bool nextLink = isHttpUrl() && overGlyph;
		if (overBtn != btnHover) btnHover = overBtn;
		if (nextLink != linkHover) {
			linkHover = nextLink;
			if (hwnd) SendMessage(hwnd, WM_SETCURSOR, (WPARAM)hwnd, MAKELPARAM(HTCLIENT, 0));
		}
	});
	onCursor.add([this](bool* handled) {
		if (btnHover) { SetCursor(LoadCursor(nullptr, IDC_ARROW)); *handled = true; return; }
		if (linkHover) { SetCursor(LoadCursor(nullptr, IDC_HAND)); *handled = true; }
	});
	if (isHttpUrl()) {
		onMouseUp.add([this](POINT pos, bool isRight) {
			if (isRight || !textBox || !textBox->isPosOverText(pos)) return;
			if (copyBtn && copyBtn->isPosIn(pos)) return;
			if ((GetKeyState(VK_CONTROL) & 0x8000) == 0) return;
			onOpenLink();
		});
	}
	show();
}

LRESULT ToolQrcode::onHitTest(const POINT screenPos)
{
	if (win && win->hwnd && win->cutMask && win->cutMask->hasRect()) {
		POINT client = screenPos;
		ScreenToClient(win->hwnd, &client);
		const auto hit = win->cutMask->hitTest(client);
		if (hit != MaskHit::None && hit != MaskHit::Inside)
			return HTTRANSPARENT;
		// 信息栏（含落在选区内时）点穿到 WinCap
		if (!win->cutMask->hideLabel
			&& win->cutMask->hitInfoControl(client) != InfoHit::None)
			return HTTRANSPARENT;
	}
	return HTCLIENT;
}

void ToolQrcode::syncToMask(const D2D1_RECT_F& maskRect, int hostX, int hostY, float hostDpi)
{
	dpi = hostDpi > 0.f ? hostDpi : dpi;
	const int left = (int)std::lround(maskRect.left);
	const int top = (int)std::lround(maskRect.top);
	const int pw = (std::max)(1, (int)std::lround(maskRect.right) - left);
	const int ph = (std::max)(1, (int)std::lround(maskRect.bottom) - top);
	const float lw = pw / dpi, lh = ph / dpi;
	setSize(lw, lh);
	setPosition(hostX + left, hostY + top);
	if (!hwnd) return;
	SetWindowPos(hwnd, HWND_TOP, hostX + left, hostY + top, pw, ph, SWP_NOACTIVATE | SWP_SHOWWINDOW);
	w = (float)pw;
	h = (float)ph;
	// 预留右上角复制钮，宽度随选区变 → TextBox 才会重折行/合并行
	const float reserve = copyBtn ? (btnSize + hoverInset * 2.f + 4.f) : 0.f;
	if (textBox)
		textBox->setSize((std::max)(8.f, lw - pad * 2.f - reserve), (std::max)(8.f, lh - pad * 2.f));
	layout();
}

void ToolQrcode::onMinMaxInfo(MINMAXINFO* mmi)
{
	mmi->ptMinTrackSize.x = 1;
	mmi->ptMinTrackSize.y = 1;
}

void ToolQrcode::onCopy()
{
	if (text.empty()) return;
	Ling::Util::setTextToClipboard(text);
	if (copyBtn) {
		copyBtn->setText(Icon::CopyOk);
		copyBtn->setColor(Icon::ColorDone);
		copyBtn->setHoverColor(Icon::ColorDone);
	}
	setTimer(450, 1); // 闪一下「复制成功」再关
}

void ToolQrcode::onOpenLink()
{
	if (!isHttpUrl()) return;
	ShellExecute(hwnd, L"open", text.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	win->close();
}
