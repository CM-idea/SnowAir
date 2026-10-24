#include "pch.h"
#include "ClipPin.h"
#include "PinMenu.h"
#include "../Lang.h"
#include "../Util.h"
#include "../Setting.h"
#include "../App.h"
#include "SettingTheme.h"
#include "../Tool/IconCodes.h"
#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>

using namespace Microsoft::WRL;

namespace {
	std::vector<std::unique_ptr<ClipPin>> g_clipPins;
	// 打开文件后盯"前台窗口是谁"的定时器：默认程序可能起得慢，隔一会儿多试几次
	constexpr UINT kOpenWatchTimerId = 0x71;
	constexpr UINT kOpenWatchDelayMs = 260;
	constexpr int kOpenWatchTries = 5;
	// 悬停链接的高亮色（blue-500：深浅色底上都看得清）
	constexpr uint32_t kLinkHoverColor = 0x3B82F6FF;

	bool extIs(const std::wstring& path, std::initializer_list<const wchar_t*> exts)
	{
		const auto dot = path.find_last_of(L'.');
		if (dot == std::wstring::npos) return false;
		std::wstring e = path.substr(dot + 1);
		for (auto& c : e) c = (wchar_t)towlower(c);
		for (auto x : exts) if (e == x) return true;
		return false;
	}
	bool isVideoPath(const std::wstring& p)
	{
		return extIs(p, { L"mp4", L"mov", L"mkv", L"avi", L"wmv", L"webm", L"flv", L"m4v", L"gif", L"apng" });
	}
	bool isImagePath(const std::wstring& p)
	{
		return extIs(p, { L"png", L"jpg", L"jpeg", L"webp", L"bmp", L"gif", L"avif", L"jxl" });
	}
	bool isDirPath(const std::wstring& p)
	{
		std::error_code ec;
		return std::filesystem::is_directory(p, ec);
	}

	std::wstring trimWs(std::wstring s)
	{
		while (!s.empty() && iswspace(s.front())) s.erase(s.begin());
		while (!s.empty() && iswspace(s.back())) s.pop_back();
		return s;
	}
	bool looksLikeLink(const std::wstring& s)
	{
		if (s.empty() || s.find_first_of(L" \t\r\n") != std::wstring::npos) return false;
		std::wstring l = s;
		for (auto& c : l) c = (wchar_t)towlower(c);
		return l.rfind(L"http://", 0) == 0 || l.rfind(L"https://", 0) == 0
			|| l.rfind(L"ftp://", 0) == 0 || l.rfind(L"file://", 0) == 0;
	}
	// 与历史页同一套判定：#RGB/#RRGGBB/#RRGGBBAA 与 rgb(r,g,b)/rgba(r,g,b,a)
	bool isColorText(const std::wstring& raw)
	{
		const std::wstring s = trimWs(raw);
		if (s.empty()) return false;
		if (s[0] == L'#') {
			const size_t n = s.size() - 1;
			if (n != 3 && n != 6 && n != 8) return false;
			for (size_t i = 0; i < n; ++i) {
				const wchar_t c = s[i + 1];
				const bool hex = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
				if (!hex) return false;
			}
			return true;
		}
		std::wstring l = s;
		for (auto& c : l) c = (wchar_t)towlower(c);
		if (s.back() != L')') return false;
		const size_t open = l.find(L'(');
		if (open == std::wstring::npos) return false;
		const bool rgba = l.rfind(L"rgba(", 0) == 0;
		if (!rgba && l.rfind(L"rgb(", 0) != 0) return false;
		int parts = 0;
		size_t start = open + 1;
		while (start < s.size() - 1) {
			const size_t comma = s.find(L',', start);
			const size_t end = comma == std::wstring::npos ? s.size() - 1 : comma;
			const std::wstring piece = trimWs(s.substr(start, end - start));
			wchar_t* tail = nullptr;
			const long v = wcstol(piece.c_str(), &tail, 10);
			if (piece.empty() || !tail || *tail != L'\0' || v < 0 || v > 255) return false;
			++parts;
			if (comma == std::wstring::npos) break;
			start = comma + 1;
		}
		return parts == (rgba ? 4 : 3);
	}
}

ClipPin::ClipPin(bool isFiles, std::wstring text, std::vector<std::wstring> paths)
	: isFiles_(isFiles), text_(std::move(text)), files_(std::move(paths))
{
}

ClipPin::~ClipPin()
{
	if (hwnd) { close(); hwnd = nullptr; }
}

void ClipPin::showText(const std::wstring& text)
{
	if (text.empty()) return;
	POINT cur{};
	GetCursorPos(&cur);
	auto ptr = std::unique_ptr<ClipPin>(new ClipPin(false, text, std::vector<std::wstring>{}));
	ClipPin* raw = ptr.get();
	g_clipPins.push_back(std::move(ptr));
	raw->w = 350.f;
	raw->h = 150.f;
	raw->setPosition(cur.x + 16, cur.y + 16);
	raw->createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, WS_POPUP);
	raw->show();
}

void ClipPin::showFiles(const std::vector<std::wstring>& files)
{
	if (files.empty()) return;
	POINT cur{};
	GetCursorPos(&cur);
	auto ptr = std::unique_ptr<ClipPin>(new ClipPin(true, L"", files));
	ClipPin* raw = ptr.get();
	g_clipPins.push_back(std::move(ptr));
	raw->w = 350.f;
	raw->h = 75.f;
	raw->setPosition(cur.x + 16, cur.y + 16);
	raw->createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, WS_POPUP);
	raw->show();
}

void ClipPin::onCreated()
{
	body->setBg(SettingTheme::card);
	body->setBorder(1.f, SettingTheme::border);
	body->setBorderRadius(SettingTheme::radiusLg);
	body->setFlexDirection(Ling::FlexDirection::Column);

	card_ = body->makeChild<Ling::Node>();
	card_->setWidthPercent(100.f);
	card_->setHeightPercent(100.f);
	// 单文件 / 文件夹：图标 + 名字一行；多文件：逐行列表。
	// 所有贴屏内容统一在顶部让出停悬三控件（锁定 / 另存为 / 关闭：高 28、距顶 6），
	// 文字、列表、单文件都从同一条基线开始，不压在按钮下面。
	const bool singleFile = isFiles_ && files_.size() == 1;
	constexpr float kChromeTop = 6.f + 28.f + 4.f;   // 6 上边距 + 28 按钮高 + 4 间隙
	constexpr float kContentBottom = 12.f;
	card_->setFlexDirection(singleFile ? Ling::FlexDirection::Row : Ling::FlexDirection::Column);
	card_->setPadding(14.f, kChromeTop, 14.f, kContentBottom);
	if (singleFile) card_->setAlignItems(Ling::Align::Center);

	if (singleFile) {
		const bool video = isVideoPath(files_.front());
		auto* icon = card_->makeChild<Ling::Label>();
		icon->setText(video ? Icon::Video : Icon::Folder);
		icon->setFontFamily(Icon::Family);
		icon->setFontSize(28.f);
		icon->setColor(video ? 0x1890FFFFu : 0xFFB020FFu);
		icon->setMarginRight(12.f);

		auto* info = card_->makeChild<Ling::Node>();
		info->setFlexDirection(Ling::FlexDirection::Column);
		info->setFlexGrow(1.f);
		info->setFlexShrink(1.f);

		auto* title = info->makeChild<Ling::Label>();
		title->setText(std::filesystem::path(files_.front()).filename().wstring());
		title->setFontSize(SettingTheme::fontBase);
		title->setColor(SettingTheme::textPrimary);
		title->setFlexShrink(1.f);

		auto* dir = info->makeChild<Ling::Label>();
		dir->setText(std::filesystem::path(files_.front()).parent_path().wstring());
		dir->setFontSize(SettingTheme::fontXs);
		dir->setColor(SettingTheme::textTertiary);
		dir->setFlexShrink(1.f);
	}
	else if (isFiles_) {
		// 多文件：逐行列出文件名（超出高度可滚轮滚动）；双击某一行用默认程序打开该文件
		auto* list = card_->makeChild<Ling::ScrollerBox>();
		list->setWidthPercent(100.f);
		list->setHeightPercent(100.f);
		list->setScrollBarVisible(false);
		list->content->setWidthPercent(100.f);
		list->content->setFlexDirection(Ling::FlexDirection::Column);

		fileRows_.clear();
		fileRows_.reserve(files_.size());
		for (const auto& f : files_) {
			auto* row = list->makeChild<Ling::Node>();
			row->setWidthPercent(100.f);
			row->setHeight(26.f);
			row->setFlexShrink(0.f);
			row->setMarginBottom(4.f);
			row->setFlexDirection(Ling::FlexDirection::Row);
			row->setAlignItems(Ling::Align::Center);

			const wchar_t* glyph = Icon::Plugin;
			uint32_t glyphColor = SettingTheme::textMuted;
			if (isDirPath(f)) { glyph = Icon::Folder; glyphColor = 0xFFB020FFu; }
			else if (isVideoPath(f)) { glyph = Icon::Video; glyphColor = 0x1890FFFFu; }
			else if (isImagePath(f)) glyph = Icon::ImageBackground;
			auto* rowIcon = row->makeChild<Ling::Label>();
			rowIcon->setText(glyph);
			rowIcon->setFontFamily(Icon::Family);
			rowIcon->setFontSize(14.f);
			rowIcon->setColor(glyphColor);
			rowIcon->setFlexShrink(0.f);
			rowIcon->setMarginRight(6.f);

			auto* name = row->makeChild<Ling::Label>();
			name->setText(std::filesystem::path(f).filename().wstring());
			name->setFontSize(SettingTheme::fontBase);
			name->setColor(SettingTheme::textPrimary);
			name->setFlexShrink(1.f);

			fileRows_.push_back(row);
		}
	}
	else {
		auto* box = card_->makeChild<Ling::TextBox>();
		box->setWidthPercent(100.f);
		box->setFlexGrow(1.f);
		box->setFlexShrink(1.f);
		box->setBg(0);
		box->setBorder(0.f, 0);
		// 左右都贴到卡片内边距：贴屏内不显示滚动条（超出仍可用滚轮），
		// 右侧不再为滚动条单独留一条间距
		box->setPadding(0.f, 0.f, 0.f, 0.f);
		box->setScrollBarVisible(false);
		box->setColor(SettingTheme::textPrimary);
		box->setFontSize(SettingTheme::fontBase);
		box->setText(text_);
		box->setReadOnly(true);
		// 只有落在文字上才归文本框（拖选 / 复制）；其余区域留给窗口拖动。
		box->setMouseSelectOverTextOnly(true);
		textBox_ = box;
	}

	// 停悬控件（同贴图）：左上锁定，右上「另存为 / 关闭」。平时隐藏，鼠标进入才浮出。
	auto mkHoverBtn = [this](const wchar_t* glyph, bool danger, std::function<void()> onClick) {
		auto* b = body->makeChild<Ling::Button>();
		b->setPositionType(Ling::Position::Absolute);
		b->setSize(28.f, 28.f);
		b->setPadding(0.f, 0.f, 0.f, 0.f);
		b->setBorder(0.f, 0);
		b->setBorderRadius(SettingTheme::radiusFull);
		b->setBg(0x00000073u);          // 同贴图：黑色 45%
		b->setHoverBg(0x00000099u);     // 同贴图：悬停 60%
		b->setText(glyph);
		b->setFontFamily(Icon::Family);
		b->setFontSize(14.f);
		b->setColor(0xFFFFFFFFu);
		b->setHoverColor(danger ? 0xFF383CFFu : 0xFFFFFFFFu);
		b->setJustifyContent(Ling::Justify::Center);
		b->setAlignItems(Ling::Align::Center);
		b->onClick.add([cb = std::move(onClick)](Ling::Button*) { cb(); });
		return b;
	};
	btnLock_ = mkHoverBtn(Icon::Unlock, false, [this]() {
		locked_ = !locked_;
		refreshHoverChrome();
		});
	btnLock_->setPosition(Ling::Edge::Left, 6.f);
	btnLock_->setPosition(Ling::Edge::Top, 6.f);

	// 文件夹 / 多文件没有「另存为文件」的语义
	canSave_ = !isFiles_ || (files_.size() == 1 && !isDirPath(files_.front()));
	// 「另存为」用文件夹图标：存盘图标容易被理解成"保存当前贴屏状态"
	btnSave_ = mkHoverBtn(Icon::Folder, false, [this]() { this->saveAs(); });
	btnSave_->setPosition(Ling::Edge::Right, 40.f);   // 6 + 28 + 6
	btnSave_->setPosition(Ling::Edge::Top, 6.f);

	btnClose_ = mkHoverBtn(Icon::Cancel, true, [this]() { this->closeSelf(); });
	btnClose_->setPosition(Ling::Edge::Right, 6.f);
	btnClose_->setPosition(Ling::Edge::Top, 6.f);

	btnLock_->hide();
	btnSave_->hide();
	btnClose_->hide();

	// 文字贴屏：按内容算高度，150 起步、最多自动长到 350；再超出就滚动
	float targetH = 150.f;
	if (!isFiles_) {
		if (auto* d2d = Ling::D2D::get()) {
			ComPtr<IDWriteTextLayout> tl;
			if (SUCCEEDED(d2d->dwriteFactory->CreateTextLayout(text_.c_str(), (UINT32)text_.size(),
				d2d->getTextFormat(L""), (350.f - 30.f) * dpi, 2000.f * dpi, tl.GetAddressOf())) && tl) {
				tl->SetFontSize(SettingTheme::fontBase * dpi, { 0, UINT32_MAX });
				DWRITE_TEXT_METRICS m{};
				if (SUCCEEDED(tl->GetMetrics(&m)) && m.height > 0.f)
					// 上下内边距（让出三控件）：窗口按内容长高，文字不会被裁
					targetH = std::clamp(m.height / dpi + kChromeTop + kContentBottom, 150.f, 350.f);
			}
		}
	}
	else {
		// 文件贴屏：统一让出上下内边距；多文件再按行数长高（上限 350，超出用滚轮滚）。
		constexpr float kRowH = 30.f;            // 行高 26 + 行距 4
		constexpr float kSingleContent = 40.f;   // 图标 28 / 名字 + 目录两行
		targetH = files_.size() > 1
			? std::clamp(kChromeTop + kContentBottom + files_.size() * kRowH, 75.f, 350.f)
			: (kChromeTop + kContentBottom + kSingleContent);
	}
	setSize(350.f, targetH);
	clampToWorkArea();

	onMouseDown.add([this](POINT pos, bool isRight) {
		pressOnText_ = false;
		pressFileIndex_ = -1;
		if (isRight) {
			// 右键：弹菜单（不再直接关闭）
			POINT sp{};
			GetCursorPos(&sp);
			this->showContextMenu(sp);
			return;
		}
		if (hoverShown_) {
			// 停悬控件自己处理点击；按下时不要顺手把窗口拖走/缩放
			if (btnLock_ && btnLock_->isPosIn(pos)) return;
			if (!locked_ && canSave_ && btnSave_ && btnSave_->isPosIn(pos)) return;
			if (!locked_ && btnClose_ && btnClose_->isPosIn(pos)) return;
		}
		if (locked_) return;   // 锁定后不能拖动 / 缩放
		// 拖边缩放：自己算边距，不让系统做非客户区缩放（系统的会一跳一跳地变大）
		const int edges = edgeHit(pos);
		if (edges != 0) {
			POINT sp{};
			GetCursorPos(&sp);
			beginResize(sp, edges);
			return;
		}
		// 双击打开：单文件 / 多文件里双击某一行名字；
		// 文字贴屏的链接走"单击打开"（见 onMouseUp），这里不重复处理
		POINT screenPos = pos;
		ClientToScreen(hwnd, &screenPos);
		const auto now = GetTickCount64();
		const bool dbl = (now - lastDownTick_ <= GetDoubleClickTime())
			&& std::abs(screenPos.x - lastDownPos_.x) <= GetSystemMetrics(SM_CXDOUBLECLK)
			&& std::abs(screenPos.y - lastDownPos_.y) <= GetSystemMetrics(SM_CYDOUBLECLK);
		lastDownTick_ = now;
		lastDownPos_ = screenPos;
		if (dbl && isFiles_) {
			if (files_.size() == 1) { openContent(); return; }
			const int row = fileRowAt(pos);
			if (row >= 0) { openFileAt((size_t)row); return; }
		}
		// 文字贴屏：文字（含文本框自己的滚动条）归文本框做拖选 / 复制 / 滚动，
		// 只有非文字区域（内边距、行末空白、边角）才拖动整个窗口。
		if (textBox_ && (overTextGlyph(pos) || overTextScrollBar(pos))) {
			// 落在文字上：记下按下位置，松开时判断是不是"单击链接"
			pressOnText_ = overTextGlyph(pos);
			pressPos_ = pos;
			return;
		}
		if (isFiles_) {
			// 单击（没拖动）时把该文件上次打开的窗口拉回前台，见 onMouseUp
			pressFileIndex_ = files_.size() > 1 ? fileRowAt(pos) : 0;
			pressPos_ = pos;
		}
		POINT sp{};
		GetCursorPos(&sp);
		dragStartScreen_ = sp;
		dragStartWin_ = { x, y };
		dragging_ = true;
		SetCapture(hwnd);
		});
	onMouseMove.add([this](POINT pos) {
		const bool inside = pos.x != INT_MAX && pos.y != INT_MAX;
		this->updateHoverChrome(inside && !resizing_);
		this->updateLinkHover((resizing_ || dragging_) ? POINT{ INT_MAX, INT_MAX } : pos);
		if (resizing_) { updateResize(); return; }
		if (!dragging_) return;
		POINT cur{};
		GetCursorPos(&cur);
		setPosition(dragStartWin_.x + cur.x - dragStartScreen_.x,
			dragStartWin_.y + cur.y - dragStartScreen_.y);
		});
	onMouseUp.add([this](POINT pos, bool isRight) {
		if (resizing_) { endResize(); return; }
		if (dragging_) { dragging_ = false; ReleaseCapture(); }
		if (isRight) return;
		// 文件贴屏：按下后没有拖动 = 单击 → 把该文件上次打开的窗口拉回前台
		if (isFiles_ && pressFileIndex_ >= 0) {
			const int index = pressFileIndex_;
			pressFileIndex_ = -1;
			if (std::abs(pos.x - pressPos_.x) <= 4 && std::abs(pos.y - pressPos_.y) <= 4)
				focusOpenedWindow(index);
			return;
		}
		if (!pressOnText_) return;
		pressOnText_ = false;
		if (!textBox_ || pos.x == INT_MAX) return;
		// 只有"单击"（按下后没有拖动）才算点链接；拖选文字不受影响
		if (std::abs(pos.x - pressPos_.x) > 4 || std::abs(pos.y - pressPos_.y) > 4) return;
		const std::wstring link = linkHitAt(textBox_->hitTextIndex(pos)).text;
		if (link.empty()) return;
		// 双击的第二下不要再开一次（单击已经打开过了）
		const ULONGLONG now = GetTickCount64();
		if (now - lastLinkOpenTick_ <= GetDoubleClickTime()
			&& std::abs(pos.x - lastLinkOpenPos_.x) <= GetSystemMetrics(SM_CXDOUBLECLK)
			&& std::abs(pos.y - lastLinkOpenPos_.y) <= GetSystemMetrics(SM_CYDOUBLECLK))
			return;
		lastLinkOpenTick_ = now;
		lastLinkOpenPos_ = pos;
		ShellExecuteW(nullptr, L"open", link.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		});
	// 捕获被别的窗口抢走（另存为等系统对话框）：松手消息不会再送来，拖拽/拖边必须复位，
	// 否则之后鼠标一动窗口就自己跟着跑 —— 看起来就是"贴屏内容位移、不受鼠标控制"。
	onCaptureLost.add([this]() {
		dragging_ = false;
		resizing_ = false;
		resizeEdges_ = 0;
		pressOnText_ = false;
		pressFileIndex_ = -1;
		});
	onKeyDown.add([this](UINT key) {
		if (key == VK_ESCAPE) this->closeSelf();
		});
	// 打开文件后：等一小会儿看是哪个窗口接住了它（见 onOpenWatchTimer）
	openedWnds_.assign(files_.size(), nullptr);
	onTimer.add([this](UINT id) {
		if (id == kOpenWatchTimerId) this->onOpenWatchTimer();
		});
	onDestroy.add([this]() { this->onClosed(); });
}

void ClipPin::layout()
{
	Ling::WinBase::layout();
}

BOOL ClipPin::setCursor()
{
	POINT pt{};
	GetCursorPos(&pt);
	ScreenToClient(hwnd, &pt);
	const int m = (int)std::lround(6 * dpi);
	if (hoverShown_) {
		if ((btnLock_ && btnLock_->isPosIn(pt))
			|| (!locked_ && canSave_ && btnSave_ && btnSave_->isPosIn(pt))
			|| (!locked_ && btnClose_ && btnClose_->isPosIn(pt))) {
			SetCursor(LoadCursor(nullptr, IDC_HAND));
			return TRUE;
		}
	}
	// 文字贴屏：链接上是手型（单击打开），普通文字上是文本光标，文本框自己的滚动条上是箭头
	if (textBox_) {
		if (textBox_->isPosOverText(pt) && !linkHitAt(textBox_->hitTextIndex(pt)).text.empty()) {
			SetCursor(LoadCursor(nullptr, IDC_HAND));
			return TRUE;
		}
		if (overTextGlyph(pt)) { SetCursor(LoadCursor(nullptr, IDC_IBEAM)); return TRUE; }
		if (overTextScrollBar(pt)) { SetCursor(LoadCursor(nullptr, IDC_ARROW)); return TRUE; }
	}
	if (pt.x <= m && pt.y <= m) { SetCursor(LoadCursor(nullptr, IDC_SIZENWSE)); return TRUE; }
	if (pt.x >= (int)w - m && pt.y >= (int)h - m) { SetCursor(LoadCursor(nullptr, IDC_SIZENWSE)); return TRUE; }
	if (pt.x >= (int)w - m && pt.y <= m) { SetCursor(LoadCursor(nullptr, IDC_SIZENESW)); return TRUE; }
	if (pt.x <= m && pt.y >= (int)h - m) { SetCursor(LoadCursor(nullptr, IDC_SIZENESW)); return TRUE; }
	if (pt.x <= m || pt.x >= (int)w - m) { SetCursor(LoadCursor(nullptr, IDC_SIZEWE)); return TRUE; }
	if (pt.y <= m || pt.y >= (int)h - m) { SetCursor(LoadCursor(nullptr, IDC_SIZENS)); return TRUE; }
	// 文件贴屏（单个 / 多个）：停在文件名等内容上是普通箭头（不给"可拖拽手柄"的暗示），
	// 按住拖动时才换成移动光标
	if (isFiles_) {
		SetCursor(LoadCursor(nullptr, dragging_ ? IDC_SIZEALL : IDC_ARROW));
		return TRUE;
	}
	// 其余区域都能拖动整窗（锁定态除外）
	SetCursor(LoadCursor(nullptr, locked_ ? IDC_ARROW : IDC_SIZEALL));
	return TRUE;
}

bool ClipPin::overTextGlyph(POINT clientPos) const
{
	return textBox_ && textBox_->isPosOverText(clientPos);
}

bool ClipPin::overTextScrollBar(POINT clientPos) const
{
	return textBox_ && textBox_->isPosInScrollBar(clientPos);
}

LRESULT ClipPin::onHitTest(POINT screenPos)
{
	// 缩放完全由鼠标事件自绘（见 beginResize/updateResize）：这里恒为客户区，
	// 避免系统非客户区缩放把窗口一次性拉大。
	(void)screenPos;
	return HTCLIENT;
}

int ClipPin::edgeHit(POINT clientPos) const
{
	const int m = (int)std::lround(6 * dpi);
	const int wpx = (int)std::lround(w);
	const int hpx = (int)std::lround(h);
	int edges = 0;
	if (clientPos.x <= m) edges |= 1;
	if (clientPos.x >= wpx - m) edges |= 2;
	if (clientPos.y <= m) edges |= 4;
	if (clientPos.y >= hpx - m) edges |= 8;
	return edges;
}

void ClipPin::beginResize(POINT screenPt, int edges)
{
	resizing_ = true;
	resizeEdges_ = edges;
	resizeStartScreen_ = screenPt;
	resizeStartX_ = x;
	resizeStartY_ = y;
	resizeStartW_ = w;
	resizeStartH_ = h;
	SetCapture(hwnd);
}

void ClipPin::updateResize()
{
	POINT cur{};
	GetCursorPos(&cur);
	const int dx = cur.x - resizeStartScreen_.x;
	const int dy = cur.y - resizeStartScreen_.y;
	float nx = (float)resizeStartX_, ny = (float)resizeStartY_;
	float nw = resizeStartW_, nh = resizeStartH_;
	if (resizeEdges_ & 1) { nx += dx; nw -= dx; }
	if (resizeEdges_ & 2) { nw += dx; }
	if (resizeEdges_ & 4) { ny += dy; nh -= dy; }
	if (resizeEdges_ & 8) { nh += dy; }
	// 最小尺寸（物理像素）：文字窗留出至少两行，文件窗放得下图标 + 名字
	const float minW = (isFiles_ ? 240.f : 220.f) * dpi;
	const float minH = (isFiles_ ? 70.f : 120.f) * dpi;
	if (nw < minW) {
		if (resizeEdges_ & 1) nx -= (minW - nw);
		nw = minW;
	}
	if (nh < minH) {
		if (resizeEdges_ & 4) ny -= (minH - nh);
		nh = minH;
	}
	setPosition((int)std::lround(nx), (int)std::lround(ny));
	setSize(nw / dpi, nh / dpi);
}

void ClipPin::endResize()
{
	resizing_ = false;
	resizeEdges_ = 0;
	ReleaseCapture();
}

std::wstring ClipPin::openTarget() const
{
	if (!isFiles_) {
		// 文字贴屏只有"单个链接"有可打开的东西：纯文字、色值、多行/多个链接都不算
		const std::wstring t = trimWs(text_);
		return looksLikeLink(t) ? t : std::wstring{};
	}
	// 多文件没有唯一的打开目标
	return files_.size() == 1 ? files_.front() : std::wstring{};
}

void ClipPin::openContent()
{
	if (isFiles_) {
		if (files_.size() == 1) openFileAt(0);
		return;
	}
	const std::wstring target = openTarget();
	if (target.empty()) return;
	ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

int ClipPin::fileRowAt(POINT clientPos) const
{
	for (size_t i = 0; i < fileRows_.size(); ++i) {
		if (fileRows_[i] && fileRows_[i]->isPosIn(clientPos)) return (int)i;
	}
	return -1;
}

void ClipPin::openFileAt(size_t index)
{
	if (index >= files_.size()) return;
	ShellExecuteW(nullptr, L"open", files_[index].c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	// 盯一下是哪个窗口接住了它：之后单击贴屏可以把那个窗口拉回前台
	pendingOpenIndex_ = (int)index;
	openWatchTries_ = 0;
	setTimer(kOpenWatchDelayMs, kOpenWatchTimerId);
}

bool ClipPin::focusOpenedWindow(int index)
{
	if (index < 0 || (size_t)index >= openedWnds_.size()) return false;
	HWND target = openedWnds_[index];
	if (!target || !IsWindow(target) || !IsWindowVisible(target)) return false;
	if (IsIconic(target)) ShowWindow(target, SW_RESTORE);
	SetForegroundWindow(target);
	BringWindowToTop(target);
	return true;
}

void ClipPin::onOpenWatchTimer()
{
	killTimer(kOpenWatchTimerId);
	if (pendingOpenIndex_ < 0) return;
	HWND foreground = GetForegroundWindow();
	DWORD pid = 0;
	if (foreground) GetWindowThreadProcessId(foreground, &pid);
	// 前台要换成"别的程序的窗口"才算接住了（还是我们自己 / 还没有窗口都不算）
	if (foreground && foreground != hwnd && pid != 0 && pid != GetCurrentProcessId()
		&& IsWindowVisible(foreground) && GetWindowTextLengthW(foreground) > 0) {
		if ((size_t)pendingOpenIndex_ < openedWnds_.size())
			openedWnds_[pendingOpenIndex_] = foreground;
		pendingOpenIndex_ = -1;
		return;
	}
	// 默认程序起得慢：再等几拍
	if (++openWatchTries_ < kOpenWatchTries) {
		setTimer(kOpenWatchDelayMs, kOpenWatchTimerId);
		return;
	}
	pendingOpenIndex_ = -1;
}

ClipPin::LinkHit ClipPin::linkHitAt(int textIndex) const
{
	LinkHit out;
	if (textIndex < 0) return out;
	// 按空白切词：点中的那个词本身是链接才算（多个链接时各点各的）
	const size_t idx = (size_t)textIndex;
	const size_t n = text_.size();
	static const std::wstring kLeading = L"([{<\"'";
	static const std::wstring kTrailing = L".,;:!?)]}>\"'";
	size_t i = 0;
	while (i < n) {
		while (i < n && iswspace(text_[i])) ++i;
		const size_t start = i;
		while (i < n && !iswspace(text_[i])) ++i;
		if (start == i) break;
		if (idx < start || idx > i) continue;   // 没点在这个词上
		size_t b = start, e = i;
		while (b < e && kLeading.find(text_[b]) != std::wstring::npos) ++b;
		while (e > b && kTrailing.find(text_[e - 1]) != std::wstring::npos) --e;
		if (b >= e) return out;
		std::wstring token = text_.substr(b, e - b);
		if (!looksLikeLink(token)) return out;
		out.text = std::move(token);
		out.start = (int)b;
		out.end = (int)e;
		return out;
	}
	return out;
}

void ClipPin::updateLinkHover(POINT clientPos)
{
	if (!textBox_) return;
	int start = 0, end = 0;
	if (clientPos.x != INT_MAX && textBox_->isPosOverText(clientPos)) {
		const LinkHit hit = linkHitAt(textBox_->hitTextIndex(clientPos));
		start = hit.start;
		end = hit.end;
	}
	if (start == hoverLinkStart_ && end == hoverLinkEnd_) return;
	hoverLinkStart_ = start;
	hoverLinkEnd_ = end;
	// 0/0 会把上一条链接的蓝色撤销掉
	textBox_->setTextRangeColor(start, end, Ling::Color(kLinkHoverColor));
}

ClipPin::Kind ClipPin::contentKind() const
{
	if (!isFiles_) {
		const std::wstring t = trimWs(text_);
		if (looksLikeLink(t)) return Kind::Link;
		if (isColorText(t)) return Kind::Color;
		return Kind::Text;
	}
	bool allDirs = !files_.empty(), allImages = !files_.empty(), allVideos = !files_.empty();
	for (const auto& f : files_) {
		allDirs = allDirs && isDirPath(f);
		allImages = allImages && isImagePath(f);
		allVideos = allVideos && isVideoPath(f);
	}
	if (allDirs) return Kind::Folder;
	if (allImages) return Kind::Image;
	if (allVideos) return Kind::Video;
	return Kind::File;
}

std::wstring ClipPin::copyLabel() const
{
	switch (contentKind()) {
	case Kind::Link:   return Lang::get(L"clip.copyLink");
	case Kind::Color:  return Lang::get(L"clip.copyColor");
	case Kind::Image:  return Lang::get(L"clip.copyImage");
	case Kind::Video:  return Lang::get(L"clip.copyVideo");
	case Kind::Folder: return Lang::get(L"clip.copyFolder");
	case Kind::File:   return Lang::get(L"clip.copyFile");
	default:           return Lang::get(L"clip.copyText");
	}
}

void ClipPin::copyToClipboard() const
{
	if (isFiles_) Util::addFilesToClipboard(files_);
	else Ling::Util::setTextToClipboard(text_);
	// 自己写入的剪贴板不当成「新内容」再记一条
	Setting::get()->markOwnClipboardWrite();
}

void ClipPin::saveAs() const
{
	if (!isFiles_) {
		// 文字 / 链接 / 色值：存成 UTF-8（带 BOM）的 txt
		const std::wstring path = Util::getSaveFilePath(hwnd, L"txt");
		if (path.empty()) return;
		std::ofstream f(std::filesystem::path(path), std::ios::binary);
		if (!f) return;
		const unsigned char bom[3]{ 0xEF, 0xBB, 0xBF };
		f.write(reinterpret_cast<const char*>(bom), 3);
		const int bytes = WideCharToMultiByte(CP_UTF8, 0, text_.c_str(), (int)text_.size(),
			nullptr, 0, nullptr, nullptr);
		if (bytes > 0) {
			std::string utf8((size_t)bytes, '\0');
			WideCharToMultiByte(CP_UTF8, 0, text_.c_str(), (int)text_.size(),
				utf8.data(), bytes, nullptr, nullptr);
			f.write(utf8.data(), (std::streamsize)utf8.size());
		}
		return;
	}
	if (files_.size() != 1) return;
	const std::wstring src = files_.front();
	if (isDirPath(src)) return;
	const std::wstring name = std::filesystem::path(src).filename().wstring();
	const auto dot = name.find_last_of(L'.');
	const std::wstring ext = dot == std::wstring::npos ? L"dat" : name.substr(dot + 1);
	const std::wstring path = Util::getSaveFilePath(hwnd, ext, name);
	if (path.empty()) return;
	std::error_code ec;
	std::filesystem::copy_file(src, path, std::filesystem::copy_options::overwrite_existing, ec);
}

void ClipPin::showContextMenu(POINT screenPt)
{
	std::vector<PinMenu::Item> items;
	auto add = [&](const std::wstring& text, std::function<void()> action,
		bool checkable = false, bool checked = false, bool enabled = true) {
		PinMenu::Item it;
		it.text = text;
		it.action = std::move(action);
		it.checkable = checkable;
		it.checked = checked;
		it.enabled = enabled;
		items.push_back(std::move(it));
	};
	auto sep = [&]() {
		PinMenu::Item it;
		it.separator = true;
		items.push_back(it);
	};

	add(copyLabel(), [this]() { this->copyToClipboard(); });
	// 「打开」只在有唯一目标（单个文件 / 文件夹 / 链接）时出现；多文件、多个链接、文字不显示这一条
	if (const std::wstring target = openTarget(); !target.empty()) {
		add(Lang::get(L"pin.menuOpen"), [this]() { this->openContent(); });
	}
	// 文件夹 / 多文件没有"另存为文件"的语义，置灰
	const bool canSave = !isFiles_ || (files_.size() == 1 && !isDirPath(files_.front()));
	add(Lang::get(L"pin.menuSaveAs"), [this]() { this->saveAs(); }, false, false, canSave);
	sep();
	add(Lang::get(L"pin.menuTop"), [this]() { this->setTopMost(!topMost_); }, true, topMost_);
	add(Lang::get(L"pin.showBorder"), [this]() { this->setBorderVisible(!borderVisible_); }, true, borderVisible_);
	sep();
	add(Lang::get(L"pin.menuClose"), [this]() { this->closeSelf(); });
	PinMenu::popup(std::move(items), screenPt);
}

void ClipPin::setTopMost(bool on)
{
	topMost_ = on;
	SetWindowPos(hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
		SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void ClipPin::setBorderVisible(bool on)
{
	borderVisible_ = on;
	if (body) {
		body->setBorder(on ? 1.f : 0.f, SettingTheme::border);
		refresh();
	}
}

void ClipPin::updateHoverChrome(bool inside)
{
	if (hoverShown_ == inside) return;
	hoverShown_ = inside;
	refreshHoverChrome();
}

void ClipPin::refreshHoverChrome()
{
	if (!btnLock_ || !btnSave_ || !btnClose_) return;
	// 锁定态只留锁定按钮（绿色实心），其余停悬控件收起
	btnLock_->setText(locked_ ? Icon::Lock : Icon::Unlock);
	btnLock_->setColor(locked_ ? Icon::ColorActive : 0xFFFFFFFFu);
	btnLock_->setHoverColor(locked_ ? Icon::ColorActive : 0xFFFFFFFFu);
	if (hoverShown_) btnLock_->show(); else btnLock_->hide();
	const bool showOthers = hoverShown_ && !locked_;
	(canSave_ && showOthers) ? btnSave_->show() : btnSave_->hide();
	showOthers ? btnClose_->show() : btnClose_->hide();
	refresh();
}

void ClipPin::clampToWorkArea()
{
	RECT rc{ x, y, x + (int)std::lround(w), y + (int)std::lround(h) };
	HMONITOR mon = MonitorFromRect(&rc, MONITOR_DEFAULTTONEAREST);
	MONITORINFO mi{ sizeof(MONITORINFO) };
	if (!mon || !GetMonitorInfoW(mon, &mi)) return;
	const int maxX = (std::max)((int)mi.rcWork.left, (int)mi.rcWork.right - (int)std::lround(w));
	const int maxY = (std::max)((int)mi.rcWork.top, (int)mi.rcWork.bottom - (int)std::lround(h));
	setPosition(std::clamp(x, (int)mi.rcWork.left, maxX), std::clamp(y, (int)mi.rcWork.top, maxY));
}

void ClipPin::closeSelf()
{
	close();
}

void ClipPin::onClosed()
{
	auto* self = this;
	Ling::App::get()->dq.TryEnqueue([self]() {
		std::erase_if(g_clipPins, [self](const std::unique_ptr<ClipPin>& p) { return p.get() == self; });
		});
}

