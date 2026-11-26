#include "pch.h"
#include "ClipPin.h"
#include "PinMenu.h"
#include "WinPin.h"                 // 条目「贴图」：把图片贴成一张普通贴图
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
#include <shlobj.h>                 // SHParseDisplayName / SHCreateDataObject / ILFree（拖出文件）
#include <shobjidl.h>
#include <ole2.h>                   // DoDragDrop / OleGetClipboard / IDropSource

using namespace Microsoft::WRL;

namespace {
	std::vector<std::unique_ptr<ClipPin>> g_clipPins;
	// 打开文件后盯"前台窗口是谁"的定时器：默认程序可能起得慢，隔一会儿多试几次
	constexpr UINT kOpenWatchTimerId = 0x71;
	constexpr UINT kOpenWatchDelayMs = 260;
	constexpr int kOpenWatchTries = 5;
	// 色值复制成功的绿色提示：亮多久
	constexpr UINT kColorFlashTimerId = 0x72;
	constexpr UINT kColorFlashMs = 700;
	constexpr uint32_t kCopyOkColor = 0x34C759FFu;   // 与工具栏选中绿一致
	// 悬停链接的高亮色（blue-500：深浅色底上都看得清）
	constexpr uint32_t kLinkHoverColor = 0x3B82F6FF;

	// —— 停悬控件 / 行底色的统一缩进（逻辑像素）——
	constexpr float kChromeInset = 6.f;  // 按钮、行底色距卡片边缘的间距：四周都用这个值
	constexpr float kChromeSize = 28.f;  // 停悬三控件（锁定 / 另存为 / 关闭）的直径
	// —— 多文件 / 混合贴屏的行规格（逻辑像素）——
	constexpr float kCardW = 350.f;      // 卡片默认宽度（截断文件名时按它算可用宽）
	constexpr float kThumb = 36.f;       // 左侧缩略图槽（图片）/ 系统图标槽
	constexpr float kRowH = kThumb + 8.f; // 行高：缩略图上下各留 4
	constexpr float kRowGap = 4.f;       // 行底色之间的间距
	constexpr float kIconSize = 34.f;    // 系统图标实际画多大（比图片缩略图再小一号）
	constexpr float kIconInset = (kThumb - kIconSize) * 0.5f;
	constexpr float kRowBtn = 26.f;      // 悬停才浮出的行内按钮
	constexpr float kRowInsetX = 6.f;    // 行底色到卡片左右边缘的缩进
	// 行内左右留白：和缩略图上下留白取同一个值（(44-36)/2 = 4），
	// 这样悬停底色里"左 4 / 上 4 / 下 4"是一圈等距
	constexpr float kRowPadX = (kRowH - kThumb) * 0.5f;
	constexpr float kRowBtnGap = 6.f;    // 文字与右侧按钮之间的安全间距
	constexpr float kRowGapThumb = 10.f; // 缩略图与文字的间距

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
	// 与历史页同一套判定：#RGB/#RRGGBB/#RRGGBBAA 与 rgb(r,g,b)/rgba(r,g,b,a)。
	// 顺带把颜色解析出来（0xRRGGBB，忽略 alpha）：色值贴屏要拿它画色块、拼 HEX/RGB 两行。
	bool parseColorValue(const std::wstring& raw, uint32_t* outRgb = nullptr)
	{
		const std::wstring s = trimWs(raw);
		if (s.empty()) return false;
		auto hexVal = [](wchar_t c) -> int {
			if (c >= L'0' && c <= L'9') return c - L'0';
			if (c >= L'a' && c <= L'f') return c - L'a' + 10;
			if (c >= L'A' && c <= L'F') return c - L'A' + 10;
			return -1;
			};
		if (s[0] == L'#') {
			const size_t n = s.size() - 1;
			if (n != 3 && n != 6 && n != 8) return false;
			for (size_t i = 1; i < s.size(); ++i) if (hexVal(s[i]) < 0) return false;
			uint32_t rgb = 0;
			if (n == 3) {
				// #RGB → 每个分量重复一次（F → FF）
				rgb = ((uint32_t)(hexVal(s[1]) * 17) << 16)
					| ((uint32_t)(hexVal(s[2]) * 17) << 8)
					| (uint32_t)(hexVal(s[3]) * 17);
			}
			else {
				auto two = [&](size_t i) { return (uint32_t)(hexVal(s[i]) * 16 + hexVal(s[i + 1])); };
				rgb = (two(1) << 16) | (two(3) << 8) | two(5);
			}
			if (outRgb) *outRgb = rgb;
			return true;
		}
		std::wstring l = s;
		for (auto& c : l) c = (wchar_t)towlower(c);
		if (s.back() != L')') return false;
		const size_t open = l.find(L'(');
		if (open == std::wstring::npos) return false;
		const bool rgba = l.rfind(L"rgba(", 0) == 0;
		if (!rgba && l.rfind(L"rgb(", 0) != 0) return false;
		int comp[4]{};
		int parts = 0;
		size_t start = open + 1;
		while (start < s.size() - 1) {
			const size_t comma = s.find(L',', start);
			const size_t end = comma == std::wstring::npos ? s.size() - 1 : comma;
			const std::wstring piece = trimWs(s.substr(start, end - start));
			wchar_t* tail = nullptr;
			const long v = wcstol(piece.c_str(), &tail, 10);
			if (piece.empty() || !tail || *tail != L'\0' || v < 0 || v > 255) return false;
			if (parts < 4) comp[parts] = (int)v;
			++parts;
			if (comma == std::wstring::npos) break;
			start = comma + 1;
		}
		if (parts != (rgba ? 4 : 3)) return false;
		if (outRgb) *outRgb = ((uint32_t)comp[0] << 16) | ((uint32_t)comp[1] << 8) | (uint32_t)comp[2];
		return true;
	}
	bool isColorText(const std::wstring& raw)
	{
		return parseColorValue(raw);
	}

	// 时长（秒）→ "00:12" / "1:02:33"
	std::wstring formatDuration(int sec)
	{
		if (sec < 0) sec = 0;
		const int h = sec / 3600, m = (sec % 3600) / 60, s = sec % 60;
		return h > 0 ? wfmt(L"%d:%02d:%02d", h, m, s) : wfmt(L"%02d:%02d", m, s);
	}

	// 条目第二行：只拼拿得到的字段（尺寸 / 时长 / 大小），拿不到的整段省略
	std::wstring rowMetaText(const std::wstring& path, bool image, bool video)
	{
		std::vector<std::wstring> parts;
		if (image) {
			const auto [w, h] = Util::imageSize(path);
			if (w > 0 && h > 0) parts.push_back(wfmt(L"%d×%d", w, h));
		}
		else if (video) {
			// 视频宽高 / 时长走系统属性系统（和资源管理器「详细信息」同源），读不到就只留大小
			const auto mi = Util::mediaInfo(path);
			if (mi.width > 0 && mi.height > 0) parts.push_back(wfmt(L"%d×%d", mi.width, mi.height));
			if (mi.durationSec > 0) parts.push_back(formatDuration(mi.durationSec));
		}
		std::error_code ec;
		const auto bytes = std::filesystem::file_size(path, ec);
		if (!ec) parts.push_back(Util::formatFileSize((uintmax_t)bytes));
		std::wstring out;
		for (auto& p : parts) {
			if (p.empty()) continue;
			if (!out.empty()) out += L" · ";
			out += p;
		}
		return out;
	}

	// 文件夹条目第二行：「N 项」。只数直接子项、不递归、不算总大小 —— 大目录 / 网盘算不起
	std::wstring folderMetaText(const std::wstring& path)
	{
		constexpr int kCap = 1000;
		const int n = Util::folderItemCount(path, kCap);
		const std::wstring num = (n >= kCap) ? (std::to_wstring(kCap) + L"+") : std::to_wstring(n);
		std::wstring fmt = Lang::get(L"clip.itemCount");
		const auto pos = fmt.find(L"%1");
		if (pos != std::wstring::npos) fmt.replace(pos, 2, num);
		return fmt;
	}

	std::wstring formatHexColor(uint32_t rgb)
	{
		return wfmt(L"#%02X%02X%02X",
			(int)((rgb >> 16) & 0xFF), (int)((rgb >> 8) & 0xFF), (int)(rgb & 0xFF));
	}
	std::wstring formatRgbColor(uint32_t rgb)
	{
		return wfmt(L"rgb(%d, %d, %d)",
			(int)((rgb >> 16) & 0xFF), (int)((rgb >> 8) & 0xFF), (int)(rgb & 0xFF));
	}

	// 从一段文本里挑出所有色值。逐字符扫描，所以：
	//   · 一行一个、一行多个（空白 / 逗号 / 分号隔开）都行；
	//   · 连在一起的（#3B82F6#FF8000）也能拆成两条；
	//   · 色值夹在别的文字里也认得出来。
	// 返回 (贴进来时的原始文字, 0xRRGGBB)，顺序按原文。
	void collectColorValues(const std::wstring& text, std::vector<std::pair<std::wstring, uint32_t>>& out)
	{
		out.clear();
		std::wstring low = text;
		for (auto& c : low) c = (wchar_t)towlower(c);

		size_t pos = 0;
		size_t lastEnd = std::wstring::npos;   // 上一个色值结束处：紧跟它的 '#' 也算新起点
		while (pos < text.size()) {
			const size_t hp = text.find(L'#', pos);
			size_t rp = low.find(L"rgb(", pos);
			const size_t rp2 = low.find(L"rgba(", pos);
			if (rp2 != std::wstring::npos && (rp == std::wstring::npos || rp2 < rp)) rp = rp2;

			size_t start = std::wstring::npos;
			bool isRgb = false;
			if (hp != std::wstring::npos && (rp == std::wstring::npos || hp < rp)) { start = hp; isRgb = false; }
			else if (rp != std::wstring::npos) { start = rp; isRgb = true; }
			if (start == std::wstring::npos) break;

			// 起点前必须是边界（避免把 URL 的 #fragment、abc#123def 之类当色值）；
			// 但紧接着上一个色值的 '#' 例外 —— 那正是 #3B82F6#FF8000 这种拼接
			const bool boundary = start == 0 || !iswalnum(text[start - 1]) || start == lastEnd;
			if (!boundary) { pos = start + 1; continue; }

			if (!isRgb) {
				size_t j = start + 1;
				while (j < text.size() && iswxdigit(text[j])) ++j;
				const size_t n = j - start - 1;
				// #RGB / #RRGGBBAA 按原样，其余取前 6 位当 #RRGGBB
				const size_t take = (n == 3 || n == 8) ? n : (n >= 6 ? 6 : 0);
				if (take == 0) { pos = start + 1; continue; }
				const std::wstring token = text.substr(start, take + 1);
				uint32_t rgb = 0;
				if (parseColorValue(token, &rgb)) {
					out.emplace_back(token, rgb);
					lastEnd = start + take + 1;
					pos = lastEnd;
					continue;
				}
				pos = start + 1;
				continue;
			}
			const size_t close = text.find(L')', start);
			if (close == std::wstring::npos) break;
			const std::wstring token = text.substr(start, close - start + 1);
			uint32_t rgb = 0;
			if (parseColorValue(token, &rgb)) {
				out.emplace_back(token, rgb);
				lastEnd = close + 1;
				pos = lastEnd;
				continue;
			}
			pos = close + 1;
		}
	}

	// 最小拖放源：用系统默认光标；Esc 取消，左键松开就落下
	class PinDropSource : public IDropSource
	{
	public:
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
		{
			if (!ppv) return E_POINTER;
			if (riid == IID_IUnknown || riid == IID_IDropSource) {
				*ppv = static_cast<IDropSource*>(this);
				AddRef();
				return S_OK;
			}
			*ppv = nullptr;
			return E_NOINTERFACE;
		}
		ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
		ULONG STDMETHODCALLTYPE Release() override
		{
			const ULONG n = --ref_;
			if (n == 0) delete this;
			return n;
		}
		HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override
		{
			if (escape) return DRAGDROP_S_CANCEL;
			if (!(keys & MK_LBUTTON)) return DRAGDROP_S_DROP;
			return S_OK;
		}
		HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
	private:
		ULONG ref_{ 1 };
	};
}

ClipPin::ClipPin(bool isFiles, std::wstring text, std::vector<std::wstring> paths)
	: isFiles_(isFiles), text_(std::move(text)), files_(std::move(paths))
{
}

ClipPin::~ClipPin()
{
	if (hwnd) { close(); hwnd = nullptr; }
}

bool ClipPin::hasAny()
{
	return !g_clipPins.empty();
}

void ClipPin::hideAll()
{
	for (auto& p : g_clipPins) {
		if (p && p->hwnd) p->hide();
	}
}

void ClipPin::showAll()
{
	for (auto& p : g_clipPins) {
		if (p && p->hwnd) p->show();
	}
}

void ClipPin::pulseDuplicate()
{
	if (!hwnd) return;
	if (!IsWindowVisible(hwnd)) show();
	PinShake::start(shakeState_, hwnd, topMost_);
}

bool ClipPin::sameFileList(const std::vector<std::wstring>& a, const std::vector<std::wstring>& b)
{
	if (a.size() != b.size()) return false;
	auto normalize = [](std::vector<std::wstring> list) {
		for (auto& s : list)
			for (auto& c : s) c = (wchar_t)towlower(c);
		std::sort(list.begin(), list.end());
		return list;
		};
	return normalize(a) == normalize(b);
}

void ClipPin::showText(const std::wstring& text)
{
	POINT cur{};
	GetCursorPos(&cur);
	showTextAt(text, POINT{cur.x + 16, cur.y + 16});
}

void ClipPin::showTextAt(const std::wstring& text, POINT topLeft)
{
	if (text.empty()) return;
	// 同样的文字已经贴着了：抬到最前抖一下，不再叠一张一模一样的卡片
	for (auto& p : g_clipPins) {
		if (!p || !p->hwnd || !IsWindow(p->hwnd) || p->isFiles_ || p->text_ != text) continue;
		p->pulseDuplicate();
		return;
	}
	auto ptr = std::unique_ptr<ClipPin>(new ClipPin(false, text, std::vector<std::wstring>{}));
	ClipPin* raw = ptr.get();
	g_clipPins.push_back(std::move(ptr));
	raw->w = 350.f;
	raw->h = 150.f;
	raw->setPosition(topLeft.x, topLeft.y);
	raw->createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, WS_POPUP);
	raw->show();
}

void ClipPin::showFiles(const std::vector<std::wstring>& files)
{
	POINT cur{};
	GetCursorPos(&cur);
	showFilesAt(files, POINT{cur.x + 16, cur.y + 16});
}

void ClipPin::showFilesAt(const std::vector<std::wstring>& files, POINT topLeft)
{
	if (files.empty()) return;
	// 同一组文件已经贴着了：抬到最前抖一下（路径大小写/顺序不同也算同一组）
	for (auto& p : g_clipPins) {
		if (!p || !p->hwnd || !IsWindow(p->hwnd) || !p->isFiles_) continue;
		if (!sameFileList(p->files_, files)) continue;
		p->pulseDuplicate();
		return;
	}
	auto ptr = std::unique_ptr<ClipPin>(new ClipPin(true, L"", files));
	ClipPin* raw = ptr.get();
	g_clipPins.push_back(std::move(ptr));
	raw->w = 350.f;
	raw->h = 75.f;
	raw->setPosition(topLeft.x, topLeft.y);
	raw->createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, WS_POPUP);
	raw->show();
}

void ClipPin::onCreated()
{
	body->setBg(SettingTheme::card);
	// 外框不在这里画死：默认无边框，打开「显示边框」才按「软件外观 > 贴图边框」的样式描边
	body->setBorder(0.f, 0);
	body->setBorderRadius(SettingTheme::radiusLg);
	body->setFlexDirection(Ling::FlexDirection::Column);

	card_ = body->makeChild<Ling::Node>();
	card_->setWidthPercent(100.f);
	card_->setHeightPercent(100.f);
	// 文件贴屏统一走「行」模板：单文件 / 单个文件夹也只是列表里的一行（缩略图或系统图标 +
	// 名字 + 尺寸/大小/子项数），不再单独显示所在目录 —— 目录常常很长，一行也放不下。
	// 所有贴屏内容统一在顶部让出停悬三控件（锁定 / 另存为 / 关闭：高 28、距顶 6）。
	// 列表的左右内边距单独处理：卡片不留内边距，由每一行的底色自己缩进
	// kChromeInset —— 这样悬停底色四周到卡片边缘的距离，和顶部三控件是同一圈 6。
	constexpr float kChromeTop = kChromeInset + kChromeSize + kChromeInset;   // 40
	constexpr float kContentBottom = 12.f;
	card_->setFlexDirection(Ling::FlexDirection::Column);

	// 纯色值走色块列表：一条色值一行（色块 + HEX / RGB + 悬停复制按钮）；
	// 文本里有多条色值就生成多行（一行一个，或一行里用留白 / 逗号 / 分号隔开）
	if (!isFiles_) {
		collectColorValues(text_, colorValues_);
		isColorCard_ = !colorValues_.empty();
	}

	if (isFiles_) {
		// 行底色左右缩进 6（与顶部三控件同一圈）。底部不再单独留白：超高被裁剪时
		// 内容直接切在卡片底边，最后一行自己的 2 就是常规情况下的底边间距
		card_->setPadding(0.f, kChromeTop, 0.f, 0.f);
		buildFileList(card_);
	}
	else if (isColorCard_) {
		card_->setPadding(0.f, kChromeTop, 0.f, 0.f);
		buildColorRows(card_);
	}
	else {
		card_->setPadding(14.f, kChromeTop, 14.f, kContentBottom);
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
	if (isColorCard_) {
		// 色值卡片：一行两个，按行数长高（上限 350，超出用滚轮滚）
		const size_t lines = (colorValues_.size() + 1) / 2;
		targetH = std::clamp(kChromeTop + (float)lines * (kRowH + kRowGap), 75.f, 350.f);
	}
	else if (!isFiles_) {
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
		// 文件贴屏：按行数长高（上限 350，超出用滚轮滚）。单文件也是一行。
		// 高度 = 顶部控件区 + 每行（行高 + 行间距）；最后一行自带的 6 就是底边留白，
		// 卡片自身不再加下内边距 —— 否则末行底色离底边会比左右更宽。
		targetH = std::clamp(kChromeTop + files_.size() * (kRowH + kRowGap), 75.f, 350.f);
	}
	setSize(kCardW, targetH);
	clampToWorkArea();
	// 外框按「显示边框 + 软件外观的贴图边框」落地（默认不描边）
	applyBorder();

	onMouseDown.add([this](POINT pos, bool isRight) {
		// 另存为对话框关闭后的一小段内吞掉鼠标按下：取消点击可能被系统回放到贴屏上
		if (GetTickCount64() < suppressMouseUntil_) return;
		// 用户已经动手了：结束「重复内容」抖动并回到底座位置
		PinShake::cancel(shakeState_, hwnd);
		pressOnText_ = false;
		pressFileIndex_ = -1;
		if (isRight) {
			// 右键：落在某个条目上就弹条目菜单，空白处弹卡片菜单（不再直接关闭）
			POINT sp{};
			GetCursorPos(&sp);
			const int row = rowAt(pos);
			if (row >= 0 && (size_t)row < rows_.size() && rows_[(size_t)row].colorIndex >= 0)
				this->showColorRowMenu(row, sp);
			else if (row >= 0)
				this->showRowMenu((size_t)row, sp);
			else
				this->showContextMenu(sp);
			return;
		}
		if (hoverShown_) {
			// 停悬控件自己处理点击；按下时不要同时拖动或缩放窗口
			if (btnLock_ && btnLock_->isPosIn(pos)) return;
			if (!locked_ && canSave_ && btnSave_ && btnSave_->isPosIn(pos)) return;
			if (!locked_ && btnClose_ && btnClose_->isPosIn(pos)) return;
		}
		// 行内悬停按钮同理：按钮的 onClick 已经先一步触发，这里不要再拖动整窗
		if (hitRowButton(pos)) return;
		// 色值行：点 HEX / RGB 文本复制对应格式；按在色块上则是"准备拖出去"
		if (isColorCard_) {
			const int r = rowAt(pos);
			if (r >= 0 && (size_t)r < rows_.size()) {
				auto& row = rows_[(size_t)r];
				if (row.colorIndex >= 0 && (size_t)row.colorIndex < colorValues_.size()) {
					const uint32_t rgb = colorValues_[(size_t)row.colorIndex].second;
					if (row.hexLabel && row.hexLabel->isPosIn(pos)) {
						this->copyPlainText(formatHexColor(rgb));
						this->flashCopiedColor(r, true, false);
						return;
					}
					if (row.rgbLabel && row.rgbLabel->isPosIn(pos)) {
						this->copyPlainText(formatRgbColor(rgb));
						this->flashCopiedColor(r, false, true);
						return;
					}
					if (row.btn && row.btn->isPosIn(pos)) return;   // 复制按钮的 onClick 已触发
					if (row.dragArea && row.dragArea->isPosIn(pos)) {
						pendingDragRow_ = r;
						pendingDragStart_ = pos;
						return;
					}
				}
			}
			// 没落在任何色值行上：继续走下面的整窗拖动
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
		// 双击打开：单文件 / 多文件一致，都按行命中（双击某一行用默认程序打开该文件）；
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
			const int row = rowAt(pos);
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
		// 缩略图 / 系统图标上按下去：可能是"拖到别的软件"，先不拖窗口；
		// 松开时没拖动就仍按"单击把上次打开的窗口拉回前台"处理（见 onMouseUp）
		if (!locked_ && isFiles_) {
			const int r = rowAt(pos);
			if (r >= 0 && (size_t)r < rows_.size()
				&& rows_[(size_t)r].dragArea && rows_[(size_t)r].dragArea->isPosIn(pos)) {
				pendingDragRow_ = r;
				pendingDragStart_ = pos;
				pressFileIndex_ = r;
				pressPos_ = pos;
				return;
			}
		}
		if (isFiles_) {
			// 单击（没拖动）时把该文件上次打开的窗口拉回前台，见 onMouseUp。
			// 单文件也按行命中：卡片空白处（控件区）按下去不再当成点了那个文件
			pressFileIndex_ = rowAt(pos);
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
		// 兜底：鼠标已松开却未收到 up（系统对话框 / 捕获丢失）时，避免贴屏继续跟随鼠标
		const bool lbtnDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
		if ((dragging_ || resizing_) && pos.x != INT_MAX && !lbtnDown) {
			dragging_ = false;
			resizing_ = false;
			resizeEdges_ = 0;
			if (GetCapture() == hwnd) ReleaseCapture();
		}
		// 按在缩略图 / 色块上之后移动超过系统拖拽阈值：把这一个条目拖到别的软件去
		if (pendingDragRow_ >= 0) {
			if (pos.x == INT_MAX) {
				pendingDragRow_ = -1;
			}
			else if (std::abs(pos.x - pendingDragStart_.x) > GetSystemMetrics(SM_CXDRAG)
				|| std::abs(pos.y - pendingDragStart_.y) > GetSystemMetrics(SM_CYDRAG)) {
				const int row = pendingDragRow_;
				pendingDragRow_ = -1;
				this->beginItemDrag(row);
				return;
			}
		}
		const bool inside = pos.x != INT_MAX && pos.y != INT_MAX;
		this->updateHoverChrome(inside && !resizing_);
		this->updateRowHover((resizing_ || dragging_ || !inside) ? POINT{ INT_MAX, INT_MAX } : pos);
		this->updateLinkHover((resizing_ || dragging_) ? POINT{ INT_MAX, INT_MAX } : pos);
		if (resizing_) { updateResize(); return; }
		if (!dragging_) return;
		POINT cur{};
		GetCursorPos(&cur);
		setPosition(dragStartWin_.x + cur.x - dragStartScreen_.x,
			dragStartWin_.y + cur.y - dragStartScreen_.y);
		});
	onMouseUp.add([this](POINT pos, bool isRight) {
		const int pendingRow = pendingDragRow_;
		pendingDragRow_ = -1;
		if (resizing_) { endResize(); return; }
		if (dragging_) { dragging_ = false; ReleaseCapture(); }
		if (isRight) return;
		// 单击缩略图 / 色块（按下后没拖动）= 复制这一条，不进剪切板历史
		if (pendingRow >= 0 && std::abs(pos.x - pendingDragStart_.x) <= 4
			&& std::abs(pos.y - pendingDragStart_.y) <= 4) {
			pressFileIndex_ = -1;
			this->copyRowItem(pendingRow);
			return;
		}
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
		pendingDragRow_ = -1;
		});
	onKeyDown.add([this](UINT key) {
		// 另存为对话框刚关闭：取消用的 Esc 会漏到贴屏上，这一小段直接吞掉
		if (key == VK_ESCAPE && GetTickCount64() < suppressEscUntil_) return;
		if (key == VK_ESCAPE) this->closeSelf();
		});
	// 打开文件后：等一小会儿看是哪个窗口接住了它（见 onOpenWatchTimer）
	openedWnds_.assign(files_.size(), nullptr);
	onTimer.add([this](UINT id) {
		if (id == kOpenWatchTimerId) this->onOpenWatchTimer();
		else if (id == kColorFlashTimerId) this->endColorFlash();
		else if (id == PinShake::kTimerId) PinShake::tick(shakeState_, hwnd);
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
	// 行内悬停按钮（贴图 / 打开文件夹）：手型
	if (hitRowButton(pt)) {
		SetCursor(LoadCursor(nullptr, IDC_HAND));
		return TRUE;
	}
	// 缩略图 / 色块：单击复制、按住拖出去 —— 给手型
	if (!locked_) {
		const int r = rowAt(pt);
		if (r >= 0 && (size_t)r < rows_.size()
			&& rows_[(size_t)r].dragArea && rows_[(size_t)r].dragArea->isPosIn(pt)) {
			SetCursor(LoadCursor(nullptr, IDC_HAND));
			return TRUE;
		}
	}
	// 色值行：HEX / RGB 两行是"点一下复制"，给手型
	if (isColorCard_) {
		const int r = rowAt(pt);
		if (r >= 0 && (size_t)r < rows_.size()) {
			const auto& row = rows_[(size_t)r];
			if ((row.hexLabel && row.hexLabel->isPosIn(pt)) || (row.rgbLabel && row.rgbLabel->isPosIn(pt))) {
				SetCursor(LoadCursor(nullptr, IDC_HAND));
				return TRUE;
			}
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

int ClipPin::rowAt(POINT clientPos) const
{
	for (size_t i = 0; i < rows_.size(); ++i) {
		if (rows_[i].node && rows_[i].node->isPosIn(clientPos)) return (int)i;
	}
	return -1;
}

bool ClipPin::hitRowButton(POINT clientPos) const
{
	for (const auto& r : rows_) {
		// 隐藏的按钮布局尺寸为 0，isPosIn 自然为假，不用再判可见性
		if (r.btn && r.btn->isPosIn(clientPos)) return true;
	}
	return false;
}

void ClipPin::updateRowHover(POINT clientPos)
{
	if (rows_.empty()) return;
	// 锁定态不浮出按钮（与悬停三控件一致：锁了就不给动内容）
	int index = -1;
	if (clientPos.x != INT_MAX && !locked_) index = rowAt(clientPos);
	if (index == hoverRow_) return;
	auto clearRow = [this](int i) {
		if (i < 0 || (size_t)i >= rows_.size()) return;
		if (rows_[(size_t)i].node) rows_[(size_t)i].node->setBg(0);
		if (rows_[(size_t)i].btn) rows_[(size_t)i].btn->hide();
	};
	clearRow(hoverRow_);
	hoverRow_ = index;
	if (hoverRow_ >= 0) {
		auto& row = rows_[(size_t)hoverRow_];
		if (row.node) row.node->setBg(SettingTheme::accent);
		if (row.btn) row.btn->show();
	}
	refresh();
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

void ClipPin::resetInteraction()
{
	dragging_ = false;
	resizing_ = false;
	resizeEdges_ = 0;
	pressOnText_ = false;
	pressFileIndex_ = -1;
	if (GetCapture() == hwnd) ReleaseCapture();
}

void ClipPin::saveAs()
{
	resetInteraction();
	suppressEscUntil_ = GetTickCount64() + 600;
	if (!isFiles_) {
		// 文字 / 链接 / 色值：存成 UTF-8（带 BOM）的 txt。
		// 默认文件名沿用「手动保存文件名格式」（带 SnowAir 品牌名 + 时间），只是后缀换成 txt
		std::wstring defName = Util::previewFileName(
			Setting::get()->getManualSaveFormat(), Setting::defaultManualSaveFormat(), L"");
		if (const auto slash = defName.find_last_of(L'/'); slash != std::wstring::npos)
			defName = defName.substr(slash + 1);
		const std::wstring path = Util::getSaveFilePath(hwnd, L"txt", defName);
		// 取消对话框的 Esc 有时会随后落到贴屏上：收尾这一小段吞掉 Esc 与鼠标按下
		suppressEscUntil_ = GetTickCount64() + 400;
		suppressMouseUntil_ = GetTickCount64() + 300;
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
	// 单文件卡片右上角那颗「另存为」：图片按图片另存为（可换格式），其余原样复制一份
	if (files_.size() != 1) return;
	if (!isDirPath(files_.front()) && isImagePath(files_.front())) saveImageAs(0);
	else saveFileAsAt(0);
}

void ClipPin::copyRowToClipboard(size_t index) const
{
	if (index >= files_.size()) return;
	const std::wstring& path = files_[index];
	if (!isDirPath(path) && isImagePath(path)) {
		// 图片：位图进剪贴板（与贴图「复制贴图」同一套 DIBV5 + PNG 双格式）
		auto [w, h, px] = Util::loadImagePixels(path);
		if (w > 0 && h > 0 && !px.empty()) Util::saveToClipboard(w, h, px.data());
		return;
	}
	Util::addFilesToClipboard({ path });
	// 自己写入的剪贴板不当成「新内容」再记一条
	Setting::get()->markOwnClipboardWrite();
}

void ClipPin::saveFileAsAt(size_t index)
{
	if (index >= files_.size()) return;
	const std::wstring src = files_[index];
	if (isDirPath(src)) return;
	resetInteraction();
	suppressEscUntil_ = GetTickCount64() + 600;
	const std::wstring name = std::filesystem::path(src).filename().wstring();
	const auto dot = name.find_last_of(L'.');
	const std::wstring ext = dot == std::wstring::npos ? L"dat" : name.substr(dot + 1);
	const std::wstring path = Util::getSaveFilePath(hwnd, ext, name, Util::desktopDir().wstring());
	suppressEscUntil_ = GetTickCount64() + 400;
	suppressMouseUntil_ = GetTickCount64() + 300;
	if (path.empty() || path == src) return;
	std::error_code ec;
	std::filesystem::copy_file(src, path, std::filesystem::copy_options::overwrite_existing, ec);
}

void ClipPin::saveImageAs(size_t index)
{
	if (index >= files_.size()) return;
	const std::wstring src = files_[index];
	auto [w, h, px] = Util::loadImagePixels(src);
	if (w <= 0 || h <= 0 || px.empty()) return;
	resetInteraction();
	suppressEscUntil_ = GetTickCount64() + 600;
	// 「保存格式」决定对话框里默认选中的类型；默认名取源文件主名
	const std::wstring saveExt = Util::extensionFromSaveFormat(Setting::get()->getSaveFormat());
	const std::wstring defName = std::filesystem::path(src).stem().wstring();
	const std::wstring path = Util::getImageSaveFilePath(hwnd, saveExt, defName, Util::desktopDir().wstring());
	suppressEscUntil_ = GetTickCount64() + 400;
	suppressMouseUntil_ = GetTickCount64() + 300;
	if (path.empty()) return;
	std::wstring outExt = std::filesystem::path(path).extension().wstring();
	for (auto& c : outExt) c = (wchar_t)towlower(c);
	// JPEG / BMP 没有可靠的 alpha：透明区域先按白底合成，否则会编码成黑块
	// （loadImagePixels 给的是直通 alpha，这里按 c*a + 255*(1-a) 算）
	if (outExt == L".jpg" || outExt == L".jpeg" || outExt == L".bmp") {
		for (size_t i = 0; i + 3 < px.size(); i += 4) {
			const int a = px[i + 3];
			if (a >= 255) continue;
			for (int ch = 0; ch < 3; ++ch) {
				const int v = ((int)px[i + ch] * a + 255 * (255 - a)) / 255;
				px[i + ch] = (BYTE)(std::min)(255, v);
			}
			px[i + 3] = 255;
		}
	}
	Util::saveToFile(path, w, h, px.data());
}

void ClipPin::pinImageAt(size_t index)
{
	if (locked_ || index >= files_.size()) return;
	auto [w, h, px] = Util::loadImagePixels(files_[index]);
	if (w <= 0 || h <= 0 || px.empty()) return;
	// 落在「光标所在显示器」的工作区中央，与历史页「贴图」/ 快速贴屏单图同一套落点
	RECT work{};
	POINT cur{};
	GetCursorPos(&cur);
	MONITORINFO mi{ sizeof(MONITORINFO) };
	if (GetMonitorInfoW(MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST), &mi)) work = mi.rcWork;
	else SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
	const int areaW = (int)work.right - (int)work.left;
	const int areaH = (int)work.bottom - (int)work.top;
	WinPin::initFromData(work.left + (std::max)(0, (areaW - w) / 2),
		work.top + (std::max)(0, (areaH - h) / 2), w, h, px, files_[index]);
}

void ClipPin::openContainerAt(size_t index)
{
	if (index >= files_.size()) return;
	const std::wstring& path = files_[index];
	// 文件夹：打开它自己；普通文件：在资源管理器里定位（打开所在目录并选中该文件）
	if (isDirPath(path)) {
		ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		return;
	}
	const std::wstring args = L"/select,\"" + path + L"\"";
	ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

std::wstring ClipPin::ellipsize(const std::wstring& text, float maxLogicalW) const
{
	if (text.empty() || maxLogicalW <= 0.f) return text;
	auto measure = [](const std::wstring& s, float fontPx) {
		auto tl = Ling::D2D::makeTextLayout(s, fontPx);
		if (!tl) return 0.f;
		DWRITE_TEXT_METRICS m{};
		return SUCCEEDED(tl->GetMetrics(&m)) ? m.width : 0.f;
		};
	const float fontPx = SettingTheme::fontBase * (dpi > 0.f ? dpi : 1.f);
	const float maxPx = maxLogicalW * (dpi > 0.f ? dpi : 1.f);
	if (measure(text, fontPx) <= maxPx) return text;
	// 二分：找最长的、加上省略号后仍然放得下的前缀
	static const std::wstring kDots = L"…";
	size_t lo = 0, hi = text.size();
	while (lo < hi) {
		const size_t mid = (lo + hi + 1) / 2;
		if (measure(text.substr(0, mid) + kDots, fontPx) <= maxPx) lo = mid;
		else hi = mid - 1;
	}
	if (lo == 0) return kDots;
	return text.substr(0, lo) + kDots;
}

void ClipPin::buildFileList(Ling::Node* card)
{
	auto* list = card->makeChild<Ling::ScrollerBox>();
	list->setWidthPercent(100.f);
	list->setHeightPercent(100.f);
	list->setScrollBarVisible(false);
	list->content->setWidthPercent(100.f);
	list->content->setFlexDirection(Ling::FlexDirection::Column);

	// 名字可用宽度：行宽（卡片宽 − 两侧缩进）− 行内左右留白 − 缩略图 − 间距 − 按钮位与其间隙
	const float textW = (kCardW - 2.f * kRowInsetX) - 2.f * kRowPadX
		- kThumb - kRowGapThumb - kRowBtn - kRowBtnGap;
	const float thumbPx = kThumb * 2.f * dpi;   // 缩略图按 2 倍分辨率解码，够清晰又不吃内存

	rows_.clear();
	rows_.reserve(files_.size());
	hoverRow_ = -1;
	for (size_t i = 0; i < files_.size(); ++i) {
		const std::wstring& f = files_[i];
		const bool dir = isDirPath(f);
		const bool image = !dir && isImagePath(f);
		const bool video = !dir && isVideoPath(f);

		// 不写宽度：靠父容器的 stretch 撑满，左右缩进由 margin 留出 ——
		// 写了 width:100% 再加 margin 会横向溢出
		auto* row = list->makeChild<Ling::Node>();
		row->setHeight(kRowH);
		row->setFlexShrink(0.f);
		row->setMarginLeft(kRowInsetX);
		row->setMarginRight(kRowInsetX);
		row->setMarginBottom(kRowGap);
		row->setPadding(kRowPadX, 0.f, kRowPadX, 0.f);
		row->setFlexDirection(Ling::FlexDirection::Row);
		row->setAlignItems(Ling::Align::Center);
		row->setBorderRadius(8.f);
		row->setBg(0);

		// —— 左：40×40 槽位。图片是真缩略图；其余取系统「文件类型图标」，取不到退回图标字体 ——
		bool thumbShown = false;
		Ling::Node* dragArea = nullptr;   // 缩略图 / 图标：从这里按下可以把这一条拖到别的软件
		if (image) {
			// WIC 解不开（webp 未装解码器等）时 imageSize 为 0：这时别放空槽，退成类型图标
			const auto [iw, ih] = Util::imageSize(f);
			if (iw > 0 && ih > 0) {
				// 缩略图带一层深色底板 + 2px 内缩：任何比例的图都像一枚"照片卡"，
				// 透明图、超宽图也不会直接贴着卡片底色看不出边界。
				// 底色固定 #111111：比卡片底(#171717)略深，行悬停(#262626)时又能明显托住图片。
				auto* thumb = row->makeChild<Ling::ImageBox>();
				thumb->setSize(kThumb, kThumb);
				thumb->setFlexShrink(0.f);
				thumb->setMarginRight(kRowGapThumb);
				thumb->setBorderRadius(6.f);
				thumb->setBg(0x111111FFu);
				thumb->setPadding(2.f);
				thumb->loadImg(f, thumbPx, thumbPx);
				thumbShown = true;
				dragArea = thumb;
			}
		}
		if (!thumbShown) {
			// 系统图标比图片缩略图收一圈：图标本身自带留白，铺满 40 会显得又大又挤
			// （槽位仍是 40，保证图标行的文字和图片行的文字左对齐）
			const std::wstring iconPng = Util::typeIconPng(f, dir);
			if (!iconPng.empty()) {
				auto* icon = row->makeChild<Ling::ImageBox>();
				icon->setSize(kThumb, kThumb);
				icon->setFlexShrink(0.f);
				icon->setMarginRight(kRowGapThumb);
				icon->setPadding(kIconInset);
				icon->loadImg(iconPng, 48.f, 48.f);
				dragArea = icon;
			}
			else {
				// 拿不到系统图标（数据目录不可写 / Shell 失败）：退回图标字体，保证不空
				auto* glyph = row->makeChild<Ling::Label>();
				glyph->setSize(kThumb, kThumb);
				glyph->setFlexShrink(0.f);
				glyph->setMarginRight(kRowGapThumb);
				glyph->setText(dir ? Icon::Folder : (video ? Icon::Video : Icon::Plugin));
				glyph->setFontFamily(Icon::Family);
				glyph->setFontSize(kThumb - 2.f * kIconInset - 4.f);
				glyph->setColor(dir ? 0xFFB020FFu : (video ? 0x1890FFFFu : SettingTheme::textMuted));
				dragArea = glyph;
			}
		}

		// —— 右：两行 —— 第一行名字，第二行尺寸 / 时长 / 大小 / 子项数 ——
		auto* info = row->makeChild<Ling::Node>();
		info->setFlexDirection(Ling::FlexDirection::Column);
		info->setFlexGrow(1.f);
		info->setFlexShrink(1.f);
		info->setJustifyContent(Ling::Justify::Center);

		auto* name = info->makeChild<Ling::Label>();
		name->setText(this->ellipsize(std::filesystem::path(f).filename().wstring(), textW));
		name->setFontSize(SettingTheme::fontBase);
		name->setColor(SettingTheme::textPrimary);
		name->setFlexShrink(0.f);

		auto* meta = info->makeChild<Ling::Label>();
		const std::wstring metaText = dir ? folderMetaText(f) : rowMetaText(f, image, video);
		meta->setText(metaText);
		meta->setFontSize(SettingTheme::fontXs);
		meta->setColor(SettingTheme::textTertiary);
		meta->setFlexShrink(0.f);
		// 第二行没有内容（文件读不到大小等）就整行收起，名字才会在行内垂直居中
		if (metaText.empty()) meta->hide();

		// —— 悬停才浮出的按钮：图片=贴图，其余=打开所在文件夹 ——
		auto* btn = row->makeChild<Ling::Button>();
		btn->setPositionType(Ling::Position::Absolute);
		// 距底色右边缘和缩略图距左边缘取同一个值，左右看起来才对称
		btn->setPosition(Ling::Edge::Right, kRowPadX);
		btn->setPosition(Ling::Edge::Top, (kRowH - kRowBtn) * 0.5f);
		btn->setSize(kRowBtn, kRowBtn);
		btn->setPadding(0.f, 0.f, 0.f, 0.f);
		btn->setBorder(0.f, 0);
		btn->setBorderRadius(SettingTheme::radiusFull);
		btn->setBg(0x0000004Du);        // 与贴图悬停控件同款：黑色半透明圆
		btn->setHoverBg(0x00000080u);
		btn->setText(image ? Icon::Pin : Icon::Folder);
		btn->setFontFamily(Icon::Family);
		btn->setFontSize(13.f);
		btn->setColor(0xFFFFFFFFu);
		btn->setHoverColor(0xFFFFFFFFu);
		btn->onClick.add([this, i](Ling::Button*) {
			if (locked_ || i >= files_.size()) return;
			if (!isDirPath(files_[i]) && isImagePath(files_[i])) this->pinImageAt(i);
			else this->openContainerAt(i);
			});
		btn->hide();   // 先按"未悬停"收起，鼠标停到行上才浮出

		rows_.push_back(Row{ row, btn, dragArea });
	}
}

void ClipPin::copyPlainText(const std::wstring& s) const
{
	if (s.empty()) return;
	Ling::Util::setTextToClipboard(s);
	// 自己写入的剪贴板不当成「新内容」再记一条
	Setting::get()->markOwnClipboardWrite();
}

// 复制成功的那一刻，被点的那行文字临时变绿；几百毫秒后由定时器复原。
// 连点会重设同一个定时器，不会残留绿色。
void ClipPin::flashCopiedColor(int rowIndex, bool hex, bool rgb)
{
	if (!isColorCard_ || rowIndex < 0 || (size_t)rowIndex >= rows_.size()) return;
	auto& row = rows_[(size_t)rowIndex];
	if (row.hexLabel) row.hexLabel->setColor(hex ? kCopyOkColor : SettingTheme::textPrimary);
	if (row.rgbLabel) row.rgbLabel->setColor(rgb ? kCopyOkColor : SettingTheme::textTertiary);
	setTimer(kColorFlashMs, kColorFlashTimerId);
	refresh();
}

void ClipPin::endColorFlash()
{
	killTimer(kColorFlashTimerId);
	// 所有色值行统一复原（只有刚点过的那行是绿的）
	for (auto& row : rows_) {
		if (row.hexLabel) row.hexLabel->setColor(SettingTheme::textPrimary);
		if (row.rgbLabel) row.rgbLabel->setColor(SettingTheme::textTertiary);
	}
	refresh();
}

// 色值卡片：一行两个色值（两栏网格），每格规格与文件列表的行一致
// （悬停底色、缩略图/色块可拖出、单击色块复制）。行高 44、格间距 8。
void ClipPin::buildColorRows(Ling::Node* card)
{
	auto* list = card->makeChild<Ling::ScrollerBox>();
	list->setWidthPercent(100.f);
	list->setHeightPercent(100.f);
	list->setScrollBarVisible(false);
	list->content->setWidthPercent(100.f);
	list->content->setFlexDirection(Ling::FlexDirection::Column);

	constexpr float kCellGap = 8.f;
	// 每格宽度：卡片宽 − 左右缩进 − 两格之间的间距，再对半分
	const float cellW = (kCardW - 2.f * kRowInsetX - kCellGap) * 0.5f;
	const float textW = cellW - 2.f * kRowPadX - kThumb - kCellGap;

	rows_.clear();
	rows_.reserve(colorValues_.size());
	hoverRow_ = -1;
	for (size_t i = 0; i < colorValues_.size(); ) {
		// 一行两格
		auto* line = list->makeChild<Ling::Node>();
		line->setHeight(kRowH);
		line->setFlexShrink(0.f);
		line->setMarginLeft(kRowInsetX);
		line->setMarginRight(kRowInsetX);
		line->setMarginBottom(kRowGap);
		line->setFlexDirection(Ling::FlexDirection::Row);
		line->setAlignItems(Ling::Align::Center);

		for (int col = 0; col < 2 && i < colorValues_.size(); ++col, ++i) {
			const uint32_t rgb = colorValues_[i].second;

			auto* cell = line->makeChild<Ling::Node>();
			cell->setWidth(cellW);
			cell->setHeight(kRowH);
			cell->setFlexShrink(0.f);
			if (col == 0) cell->setMarginRight(kCellGap);
			cell->setPadding(kRowPadX, 0.f, kRowPadX, 0.f);
			cell->setFlexDirection(Ling::FlexDirection::Row);
			cell->setAlignItems(Ling::Align::Center);
			cell->setBorderRadius(8.f);
			cell->setBg(0);

			// 左：色块（带一圈细描边，白色 / 浅色也能看出边界）；单击复制、按住可拖出去
			auto* swatch = cell->makeChild<Ling::Node>();
			swatch->setSize(kThumb, kThumb);
			swatch->setFlexShrink(0.f);
			swatch->setMarginRight(kCellGap);
			swatch->setBorderRadius(6.f);
			swatch->setBg((rgb << 8) | 0xFFu);
			swatch->setBorder(1.f, SettingTheme::isDark() ? 0xFFFFFF33u : 0x00000026u);

			// 右：两行 —— 第一行是"复制时那种格式"，第二行是另一种格式。
			// 所以 rgb(…) 复制的就把 RGB 放上面（点哪行复制哪种格式）
			const std::wstring& token = colorValues_[i].first;
			const bool rgbFirst = !token.empty() && (token[0] == L'r' || token[0] == L'R');

			auto* info = cell->makeChild<Ling::Node>();
			info->setWidth(textW);
			info->setFlexShrink(1.f);
			info->setFlexDirection(Ling::FlexDirection::Column);
			info->setJustifyContent(Ling::Justify::Center);

			auto* top = info->makeChild<Ling::Label>();
			top->setFontSize(SettingTheme::fontBase);
			top->setColor(SettingTheme::textPrimary);
			top->setFlexShrink(0.f);

			auto* bottom = info->makeChild<Ling::Label>();
			bottom->setFontSize(SettingTheme::fontXs);
			bottom->setColor(SettingTheme::textTertiary);
			bottom->setFlexShrink(0.f);

			Ling::Label* hexLabel = nullptr;
			Ling::Label* rgbLabel = nullptr;
			if (rgbFirst) {
				top->setText(formatRgbColor(rgb));
				bottom->setText(formatHexColor(rgb));
				rgbLabel = top;
				hexLabel = bottom;
			}
			else {
				top->setText(formatHexColor(rgb));
				bottom->setText(formatRgbColor(rgb));
				hexLabel = top;
				rgbLabel = bottom;
			}

			rows_.push_back(Row{ cell, nullptr, swatch, hexLabel, rgbLabel, (int)i });
		}
	}
}

void ClipPin::showRowMenu(size_t index, POINT screenPt)
{
	if (index >= files_.size()) return;
	const std::wstring path = files_[index];
	const bool dir = isDirPath(path);
	const bool image = !dir && isImagePath(path);

	std::vector<PinMenu::Item> items;
	auto add = [&](const std::wstring& text, std::function<void()> action,
		bool checkable = false, bool checked = false) {
		PinMenu::Item it;
		it.text = text;
		it.action = std::move(action);
		it.checkable = checkable;
		it.checked = checked;
		items.push_back(std::move(it));
	};
	auto sep = [&]() {
		PinMenu::Item it;
		it.separator = true;
		items.push_back(it);
	};

	add(Lang::get(L"pin.menuOpen"), [this, index]() { this->openFileAt(index); });
	add(image ? Lang::get(L"clip.copyImage")
		: (dir ? Lang::get(L"clip.copyFolder") : Lang::get(L"clip.copyFile")),
		[this, index]() { this->copyRowToClipboard(index); });
	// 图片：图片另存为（可换格式重新编码）；普通文件：文件另存为（原样复制）；文件夹没有这个语义
	if (!dir) {
		add(image ? Lang::get(L"clip.saveImageAs") : Lang::get(L"clip.saveFileAs"),
			[this, index, image]() {
				if (image) this->saveImageAs(index);
				else this->saveFileAsAt(index);
			});
	}
	sep();
	appendWindowMenu(items);
	PinMenu::popup(std::move(items), screenPt);
}

void ClipPin::showColorRowMenu(int rowIndex, POINT screenPt)
{
	if (rowIndex < 0 || (size_t)rowIndex >= rows_.size()) return;
	const int ci = rows_[(size_t)rowIndex].colorIndex;
	if (ci < 0 || (size_t)ci >= colorValues_.size()) return;

	std::vector<PinMenu::Item> items;
	auto add = [&](const std::wstring& text, std::function<void()> action,
		bool checkable = false, bool checked = false) {
		PinMenu::Item it;
		it.text = text;
		it.action = std::move(action);
		it.checkable = checkable;
		it.checked = checked;
		items.push_back(std::move(it));
	};
	auto sep = [&]() {
		PinMenu::Item it;
		it.separator = true;
		items.push_back(it);
	};

	add(Lang::get(L"clip.copyColor"), [this, ci]() {
		this->copyPlainText(colorValues_[(size_t)ci].first);
		this->flashCopiedColor((int)ci, true, true);
		});
	sep();
	appendWindowMenu(items);
	PinMenu::popup(std::move(items), screenPt);
}

// 从一个条目拖出去：文件走 CF_HDROP（等同从资源管理器拖那个文件），色值走文字。
// 用系统的 DoDragDrop，落到任何接受拖放的软件里即可（和晾衣架拖出去是一个意思）。
// 把第 rowIndex 条放进剪贴板。单击复制和拖动共用这一段，区别只在图片：
// 单击给位图（与右键「复制图片」一致），拖动给文件（等同从资源管理器拖这个文件）。
// 所有写入都标成"自己写的"，不会进剪切板历史。
bool ClipPin::putRowOnClipboard(int rowIndex, bool imageAsFile)
{
	if (rowIndex < 0 || (size_t)rowIndex >= rows_.size()) return false;
	const int ci = rows_[(size_t)rowIndex].colorIndex;
	if (ci >= 0) {
		if ((size_t)ci >= colorValues_.size()) return false;
		copyPlainText(colorValues_[(size_t)ci].first);
		return true;
	}
	if ((size_t)rowIndex >= files_.size()) return false;
	const std::wstring& path = files_[(size_t)rowIndex];
	if (!imageAsFile && !isDirPath(path) && isImagePath(path)) {
		auto [w, h, px] = Util::loadImagePixels(path);
		if (w <= 0 || h <= 0 || px.empty()) return false;
		Util::saveToClipboard(w, h, px.data());   // 内部已标记为自己写入
		return true;
	}
	Util::addFilesToClipboard({ path });
	Setting::get()->markOwnClipboardWrite();
	return true;
}

void ClipPin::beginItemDrag(int rowIndex)
{
	if (locked_ || !this->putRowOnClipboard(rowIndex, true)) return;
	ComPtr<IDataObject> data;
	if (FAILED(OleGetClipboard(data.GetAddressOf())) || !data) return;

	// DoDragDrop 期间由它接管鼠标；先把自己这边的拖动状态复位
	dragging_ = false;
	if (GetCapture() == hwnd) ReleaseCapture();
	ComPtr<PinDropSource> src;
	src.Attach(new PinDropSource());
	DWORD effect = DROPEFFECT_NONE;
	DoDragDrop(data.Get(), src.Get(), DROPEFFECT_COPY, &effect);
	refresh();
}

// 单击缩略图 / 色块：把这一条复制到剪贴板（和晾衣架"单击复制"一致）
void ClipPin::copyRowItem(int rowIndex)
{
	if (!this->putRowOnClipboard(rowIndex, false)) return;
	if ((size_t)rowIndex < rows_.size() && rows_[(size_t)rowIndex].colorIndex >= 0)
		this->flashCopiedColor(rowIndex, true, true);
}

void ClipPin::appendWindowMenu(std::vector<PinMenu::Item>& items)
{
	auto add = [&](const std::wstring& text, std::function<void()> action,
		bool checkable = false, bool checked = false) {
		PinMenu::Item it;
		it.text = text;
		it.action = std::move(action);
		it.checkable = checkable;
		it.checked = checked;
		items.push_back(std::move(it));
	};
	auto sep = [&]() {
		PinMenu::Item it;
		it.separator = true;
		items.push_back(it);
	};

	add(Lang::get(L"pin.menuTop"), [this]() { this->setTopMost(!topMost_); }, true, topMost_);
	add(Lang::get(L"pin.showBorder"), [this]() { this->setBorderVisible(!borderVisible_); }, true, borderVisible_);
	sep();
	add(Lang::get(L"pin.menuLock"), [this]() { this->toggleLocked(); }, true, locked_);
	add(Lang::get(L"pin.menuClose"), [this]() { this->closeSelf(); });
}

void ClipPin::toggleLocked()
{
	locked_ = !locked_;
	// 锁上时把行内按钮与悬停底色一起收起，和点左上角锁按钮完全等价
	refreshHoverChrome();
}

void ClipPin::showContextMenu(POINT screenPt)
{
	// 空白区域右键：
	//   · 单个文件 / 文件夹：内容项（打开 / 复制 / 另存为）+ 窗口级项
	//   · 文字卡片：同样先给内容项（复制文字 / 打开链接 / 保存为文件）
	//   · 多文件卡片：只给窗口级项（复制 / 另存为属于某一行，到行上右键去）
	if (isFiles_ && files_.size() == 1) {
		this->showRowMenu(0, screenPt);
		return;
	}
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

	if (!isFiles_) {
		add(copyLabel(), [this]() { this->copyToClipboard(); });
		// 「打开」只在有唯一目标（链接）时出现
		if (const std::wstring target = openTarget(); !target.empty()) {
			add(Lang::get(L"pin.menuOpen"), [this]() { this->openContent(); });
		}
		add(Lang::get(L"pin.menuSaveAs"), [this]() { this->saveAs(); });
		sep();
	}
	appendWindowMenu(items);
	PinMenu::popup(std::move(items), screenPt);
}

void ClipPin::setTopMost(bool on)
{
	topMost_ = on;
	SetWindowPos(hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
		SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void ClipPin::applyBorder()
{
	if (!body) return;
	auto* s = Setting::get();
	int width = s ? s->getPinBorderWidth() : 0;
	const int radius = s ? s->getPinBorderRadius() : 6;
	if (!borderVisible_) {
		// 贴屏卡片默认无边框：圆角回到卡片自己的主题圆角，不描边
		body->setBorder(0.f, 0);
		body->setBorderRadius(SettingTheme::radiusLg);
	}
	else {
		// 走「软件外观 > 贴图边框」：粗细 0 表示不画线，但圆角仍按外观页的设置
		if (width > 0) body->setBorder((float)width, s->getPinBorderColor());
		else body->setBorder(0.f, 0);
		body->setBorderRadius((float)radius);
	}
	refresh();
}

void ClipPin::refreshBorderAppearance()
{
	for (auto& p : g_clipPins) {
		if (p && p->hwnd) p->applyBorder();
	}
}

void ClipPin::setBorderVisible(bool on)
{
	borderVisible_ = on;
	applyBorder();
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
	// 锁定后行内按钮一并收起（悬停底色也撤掉，避免看起来还能点）
	if (locked_) updateRowHover(POINT{ INT_MAX, INT_MAX });
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
		// 贴屏也全关了（且没有贴图在）：把这类窗口专用的资源回收掉
		if (g_clipPins.empty() && !WinPin::hasWindow()) App::scheduleIdleRelease();
		});
}

