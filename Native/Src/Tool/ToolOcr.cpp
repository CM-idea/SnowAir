#include "pch.h"
#include "../Win/WinCap.h"
#include "../Lang.h"
#include "ToolOcr.h"
#include "IconCodes.h"
#include "ToolbarTheme.h"

ToolOcr::ToolOcr(WinCap* win, OcrResult result, bool translateMode)
	: Ling::WinBase(), win(win), result_(std::move(result)), translateMode_(translateMode)
{
	dpi = win->dpi;
	onKeyDown.add([this](UINT key) {
		if ((GetKeyState(VK_CONTROL) & 0x8000) && (key == 'A' || key == 'C' || key == 'X'))
			return;
		this->win->onKeyDown(key);
	});
}

ToolOcr::~ToolOcr() {}

void ToolOcr::onCreated()
{
	body->setBg(0xF8F8F8EE);
	body->setBorderRadius(4.f);

	copyBtn = body->makeChild<Ling::Button>();
	copyBtn->setText(Icon::Copy);
	copyBtn->setSize(34.f, 34.f);
	copyBtn->setPositionType(Ling::Position::Absolute);
	copyBtn->setPosition(Ling::Edge::Right, 4.f);
	copyBtn->setPosition(Ling::Edge::Top, 4.f);
	copyBtn->setBg(0);
	copyBtn->setHoverBg(ToolbarTheme::hoverBg);
	copyBtn->setBorderRadius(ToolbarTheme::hoverRadius);
	copyBtn->setAlignItems(Ling::Align::Center);
	copyBtn->setJustifyContent(Ling::Justify::Center);
	copyBtn->setFontFamily(Icon::Family);
	copyBtn->setFontSize(Icon::Size);
	copyBtn->setColor(Icon::ColorNormal);
	copyBtn->onClick.add([this](Ling::Button*) { copyAll(); });

	bannerNode = body->makeChild<Ling::Node>();
	bannerNode->setPositionType(Ling::Position::Absolute);
	bannerNode->setPosition(Ling::Edge::Left, 8.f);
	bannerNode->setPosition(Ling::Edge::Top, 8.f);
	bannerNode->setPadding(8.f, 6.f, 8.f, 6.f);
	bannerNode->setBorderRadius(4.f);
	bannerNode->hide();
	bannerLabel = bannerNode->makeChild<Ling::Label>();
	bannerLabel->setFontSize(12.f);

	rebuildEditors();
	show();
}

void ToolOcr::rebuildEditors()
{
	for (auto* e : editors) {
		if (e) body->removeChild(e);
	}
	editors.clear();

	if (!result_.ok) {
		showBanner(result_.error.empty() ? Lang::get(L"ocr.fail") : result_.error, true);
		return;
	}
	clearBanner();

	const float scaleX = (imgW > 0) ? (w / (float)imgW) : 1.f;
	const float scaleY = (imgH > 0) ? (h / (float)imgH) : 1.f;

	for (auto& reg : result_.regions) {
		auto* box = body->makeChild<Ling::TextBox>();
		box->setPositionType(Ling::Position::Absolute);
		float bx = reg.x * scaleX;
		float by = reg.y * scaleY;
		float bw = std::max(40.f, reg.w * scaleX);
		float bh = std::max(18.f, reg.h * scaleY);
		box->setPosition(Ling::Edge::Left, bx / dpi);
		box->setPosition(Ling::Edge::Top, by / dpi);
		box->setSize(bw / dpi, bh / dpi);
		box->setPadding(2.f);
		box->setBg(0xFFFFFFCC);
		box->setBorderRadius(2.f);
		box->setFontSize(std::clamp(bh / dpi * 0.7f, 10.f, 22.f));
		box->setColor(Icon::ColorNormal);
		box->setText(reg.displayText.empty() ? reg.text : reg.displayText);
		editors.push_back(box);
	}
	if (editors.empty() && !result_.text.empty()) {
		auto* box = body->makeChild<Ling::TextBox>();
		box->setPositionType(Ling::Position::Absolute);
		box->setPosition(Ling::Edge::Left, 8.f);
		box->setPosition(Ling::Edge::Top, 40.f);
		box->setSize(std::max(80.f, w / dpi - 16.f), std::max(40.f, h / dpi - 48.f));
		box->setBg(0xFFFFFFCC);
		box->setText(result_.text);
		editors.push_back(box);
	}
	if (!editors.empty()) {
		editors[0]->selectAll();
		editors[0]->focus();
	}
}

void ToolOcr::applyTranslate(const std::vector<std::wstring>& lines)
{
	for (size_t i = 0; i < result_.regions.size(); i++) {
		if (i < lines.size() && !lines[i].empty())
			result_.regions[i].displayText = lines[i];
		else if (result_.regions.size() == 1 && !lines.empty())
			result_.regions[i].displayText = lines[0];
	}
	result_.text.clear();
	for (size_t i = 0; i < result_.regions.size(); i++) {
		if (i) result_.text += L'\n';
		result_.text += result_.regions[i].displayText.empty()
			? result_.regions[i].text : result_.regions[i].displayText;
	}
	rebuildEditors();
}

void ToolOcr::setResult(OcrResult result)
{
	result_ = std::move(result);
	rebuildEditors();
}

void ToolOcr::showBanner(const std::wstring& msg, bool error)
{
	banner_ = msg;
	bannerError_ = error;
	if (!bannerNode || !bannerLabel) return;
	bannerNode->setBg(error ? 0xE81123CC : 0x333333CC);
	bannerLabel->setColor(0xFFFFFFFF);
	bannerLabel->setText(msg);
	bannerNode->show();
}

void ToolOcr::clearBanner()
{
	banner_.clear();
	if (bannerNode) bannerNode->hide();
}

void ToolOcr::copyAll()
{
	std::wstring text;
	for (size_t i = 0; i < editors.size(); i++) {
		if (i) text += L'\n';
		text += editors[i]->getText();
	}
	if (text.empty()) text = result_.text;
	if (OpenClipboard(hwnd)) {
		EmptyClipboard();
		size_t bytes = (text.size() + 1) * sizeof(wchar_t);
		HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
		if (h) {
			memcpy(GlobalLock(h), text.c_str(), bytes);
			GlobalUnlock(h);
			SetClipboardData(CF_UNICODETEXT, h);
		}
		CloseClipboard();
	}
}

void ToolOcr::syncToMask(const D2D1_RECT_F& maskRect, int hostX, int hostY, float hostDpi)
{
	dpi = hostDpi;
	const int left = (int)std::lround(maskRect.left);
	const int top = (int)std::lround(maskRect.top);
	const int right = (int)std::lround(maskRect.right);
	const int bottom = (int)std::lround(maskRect.bottom);
	imgW = std::max(1, right - left);
	imgH = std::max(1, bottom - top);
	setSize((float)imgW / dpi, (float)imgH / dpi);
	setPosition(hostX + left, hostY + top);
	rebuildEditors();
}

LRESULT ToolOcr::onHitTest(const POINT screenPos)
{
	if (win && win->hwnd && win->cutMask && win->cutMask->hasRect()) {
		POINT client = screenPos;
		ScreenToClient(win->hwnd, &client);
		const auto& r = win->cutMask->maskRect;
		if (client.x < r.left || client.x >= r.right || client.y < r.top || client.y >= r.bottom)
			return HTTRANSPARENT;
	}
	return HTCLIENT;
}

void ToolOcr::onMinMaxInfo(MINMAXINFO* mmi)
{
	mmi->ptMinTrackSize.x = 1;
	mmi->ptMinTrackSize.y = 1;
}
