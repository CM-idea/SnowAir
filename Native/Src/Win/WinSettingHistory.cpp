#include "pch.h"
#include <filesystem>
#include <algorithm>
#include <chrono>
#include <cwctype>
#include "../Lang.h"
#include "../Setting.h"
#include "../History.h"
#include "../Util.h"
#include "../Tip.h"
#include "../Tool/IconCodes.h"
#include "../Tool/ToolbarChrome.h"
#include "WinSettingHistory.h"
#include "SettingTheme.h"
#include "SettingWidgets.h"
#include "WinPin.h"
#include "ClipPin.h"

// 停悬预览：独立置顶小窗（不抢焦点）。
//   · 图片：只显示图片本身（无底色 / 边框 / 内边距），按宽高比定尺寸贴在卡片旁边；
//   · 文字：用设置页「文字气泡」的底色，按内容长度长高，最低 350×100、最高 350×650，
//     且不超过工作区；不显示贴屏卡片的锁 / 另存为 / 关闭三个停悬控件。
class ShotPreviewWindow : public Ling::WinBase
{
public:
	explicit ShotPreviewWindow(float ownerDpi)
	{
		dpi = ownerDpi > 0.f ? ownerDpi : 1.f;
	}
	~ShotPreviewWindow()
	{
		// WinBase 析构不销毁 HWND：不关就留一个"内容已拆、窗口还在"的幽灵窗
		if (hwnd) { close(); hwnd = nullptr; }
	}

	void showImage(const std::wstring& path, int imgW, int imgH, const RECT& cellScreen)
	{
		if (path.empty() || imgW <= 0 || imgH <= 0) return;
		MONITORINFO mi{};
		if (!workAreaAt(cellScreen, mi)) return;
		// 1:1 显示（1 图片像素 = 1 物理像素）；长图允许超出屏幕，只在摆放时把左上角夹进工作区
		const int pxW = imgW;
		const int pxH = imgH;
		if (!ensureWindow(pxW, pxH)) return;
		applyImageChrome();
		setSize((float)pxW / dpi, (float)pxH / dpi);
		if (img_ && text_ && filesBox_) { text_->hide(); filesBox_->hide(); img_->show(); }
		if (img_ && path_ != path) {
			img_->loadImg(path, (float)pxW, (float)pxH);
			path_ = path;
		}
		place(pxW, pxH, cellScreen, mi);
		showNoActivate();
		refresh();
	}

	void showText(const std::wstring& text, const RECT& cellScreen)
	{
		if (text.empty()) return;
		MONITORINFO mi{};
		if (!workAreaAt(cellScreen, mi)) return;
		constexpr float kWidth = 350.f;    // 文字卡宽度
		constexpr float kPadX = 12.f;      // 与设置页文字气泡一致的内边距
		constexpr float kPadY = 8.f;
		// 先按内容量算高度（与卡片同一套字体 / 宽度）
		float contentH = 0.f;
		if (auto* d2d = Ling::D2D::get()) {
			Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
			if (SUCCEEDED(d2d->dwriteFactory->CreateTextLayout(text.c_str(), (UINT32)text.size(),
				d2d->getTextFormat(L""), (kWidth - kPadX * 2.f) * dpi, 20000.f * dpi,
				layout.GetAddressOf())) && layout) {
				layout->SetFontSize(SettingTheme::fontBase * dpi, { 0, UINT32_MAX });
				layout->SetWordWrapping(DWRITE_WORD_WRAPPING_EMERGENCY_BREAK);
				DWRITE_TEXT_METRICS m{};
				if (SUCCEEDED(layout->GetMetrics(&m))) contentH = m.height;
			}
		}
		const float areaHLogical = (float)(mi.rcWork.bottom - mi.rcWork.top) / dpi;
		const float maxHLogical = (std::min)(650.f, areaHLogical - 16.f);
		float hLogical = contentH / dpi + kPadY * 2.f;
		hLogical = std::clamp(hLogical, 100.f, (std::max)(100.f, maxHLogical));
		const int pxW = (int)std::lround(kWidth * dpi);
		const int pxH = (int)std::lround(hLogical * dpi);
		if (!ensureWindow(pxW, pxH)) return;
		applyTextChrome();
		setSize(kWidth, hLogical);
		if (img_ && text_ && filesBox_) { img_->hide(); filesBox_->hide(); text_->show(); }
		if (text_) text_->setText(text);
		place(pxW, pxH, cellScreen, mi);
		showNoActivate();
		refresh();
	}

	// 文件 / 文件夹：和「贴屏」的文件卡片一致——图标 + 文件名 + 来源目录（多条时逐行列出）
	void showFiles(const std::vector<std::wstring>& files, const RECT& cellScreen)
	{
		if (files.empty()) return;
		MONITORINFO mi{};
		if (!workAreaAt(cellScreen, mi)) return;
		constexpr float kWidth = 350.f;
		constexpr float kPadY = 8.f;
		const bool single = files.size() == 1;
		// 单条：图标 + 名字 / 来源两行（约 46 高）；多条：每行 26 + 4 间距
		const float rowsH = single ? 46.f : (float)files.size() * 30.f - 4.f;
		const float areaHLogical = (float)(mi.rcWork.bottom - mi.rcWork.top) / dpi;
		const float maxHLogical = (std::min)(650.f, areaHLogical - 16.f);
		float hLogical = rowsH + kPadY * 2.f;
		hLogical = std::clamp(hLogical, 100.f, (std::max)(100.f, maxHLogical));
		const int pxW = (int)std::lround(kWidth * dpi);
		const int pxH = (int)std::lround(hLogical * dpi);
		if (!ensureWindow(pxW, pxH)) return;
		applyTextChrome();
		applyFiles(files);
		setSize(kWidth, hLogical);
		if (img_ && text_ && filesBox_) { img_->hide(); text_->hide(); filesBox_->show(); }
		place(pxW, pxH, cellScreen, mi);
		showNoActivate();
		refresh();
	}

	void hideNow()
	{
		if (hwnd && IsWindowVisible(hwnd)) hide();
	}

protected:
	// 预览窗对鼠标完全透明：光标"穿过"它仍算停悬在原卡片上。
	// 否则预览一旦盖住光标，主窗就会收到 MOUSELEAVE → 收起大图 → 又停悬 → 反复闪烁。
	LRESULT onHitTest(const POINT) override { return HTTRANSPARENT; }
	void onCreated() override
	{
		body->setBg(0);
		card_ = body->makeChild<Ling::Node>();
		card_->setWidthPercent(100.f);
		card_->setHeightPercent(100.f);

		img_ = card_->makeChild<Ling::ImageBox>();
		img_->setWidthPercent(100.f);
		img_->setHeightPercent(100.f);

		text_ = card_->makeChild<Ling::TextBox>();
		text_->setWidthPercent(100.f);
		text_->setHeightPercent(100.f);
		text_->setBg(0);
		text_->setBorder(0.f, 0);
		text_->setFontSize(SettingTheme::fontBase);
		text_->setReadOnly(true);
		text_->setMouseSelectionEnabled(false);
		text_->setTextHoverCursor(false);
		text_->setScrollBarVisible(false);   // 超出 650 上限时用滚轮看剩余内容
		text_->hide();

		// 文件列表：图标 + 名字（+ 来源），和贴屏的文件卡片同款
		filesBox_ = card_->makeChild<Ling::Node>();
		filesBox_->setWidthPercent(100.f);
		filesBox_->setHeightPercent(100.f);
		filesBox_->setFlexDirection(Ling::FlexDirection::Column);
		filesBox_->hide();
	}

private:
	// 浅色主题：浅底黑字；深色主题：深底白字。
	static uint32_t bubbleBg() { return SettingTheme::isDark() ? 0x262626FF : 0xE5E5E5FF; }
	static uint32_t bubbleFg() { return SettingTheme::isDark() ? 0xFFFFFFFF : 0x262626FF; }
	// 气泡内的次要文字（来源目录）：跟着气泡底色走，不能用主题的 textTertiary
	static uint32_t bubbleFgDim() { return SettingTheme::isDark() ? 0xA3A3A3FF : 0x6B6B6BFF; }

	// 图片：只显示图片本身
	void applyImageChrome()
	{
		if (!card_) return;
		card_->setBg(0);
		card_->setBorder(0.f, 0);
		card_->setBorderRadius(0.f);
		card_->setPadding(0.f);
	}

	// 文字：铺文字气泡底色
	void applyTextChrome()
	{
		if (!card_) return;
		card_->setBg(bubbleBg());
		card_->setBorder(0.f, 0);
		card_->setBorderRadius(SettingTheme::radiusLg);
		card_->setPadding(12.f, 8.f, 12.f, 8.f);
		if (text_) text_->setColor(bubbleFg());
	}

	// 按文件类型给图标与颜色（与贴屏文件卡片一致：单文件统一用文件夹图标，视频用蓝色图标）
	static const wchar_t* fileGlyph(const std::wstring& f, uint32_t* color, bool single = false)
	{
		std::error_code ec;
		if (std::filesystem::is_directory(f, ec)) { *color = 0xFFB020FFu; return Icon::Folder; }
		if (hasExt(f, { L"mp4", L"mov", L"mkv", L"avi", L"wmv", L"webm", L"flv", L"m4v" })) {
			*color = 0x1890FFFFu;
			return Icon::Video;
		}
		// 单个文件与贴屏保持一致：除视频外都显示文件夹图标
		if (single) { *color = 0xFFB020FFu; return Icon::Folder; }
		if (hasExt(f, { L"png", L"jpg", L"jpeg", L"webp", L"bmp", L"gif", L"avif", L"jxl" })) {
			*color = SettingTheme::textMuted;
			return Icon::ImageBackground;
		}
		*color = SettingTheme::textMuted;
		return Icon::Plugin;
	}

	static bool hasExt(const std::wstring& path, std::initializer_list<const wchar_t*> exts)
	{
		const auto dot = path.find_last_of(L'.');
		if (dot == std::wstring::npos) return false;
		std::wstring e = path.substr(dot + 1);
		for (auto& c : e) c = (wchar_t)towlower(c);
		for (auto x : exts) if (e == x) return true;
		return false;
	}

	void applyFiles(const std::vector<std::wstring>& files)
	{
		if (!filesBox_) return;
		filesBox_->removeAllChildren();
		const bool single = files.size() == 1;
		filesBox_->setJustifyContent(single ? Ling::Justify::Center : Ling::Justify::Start);

		if (single) {
			// 单条：大图标 + 名字 + 来源目录（同贴屏的单文件卡片）
			const auto& f = files.front();
			uint32_t iconColor{};
			const wchar_t* glyph = fileGlyph(f, &iconColor, true);
			auto* row = filesBox_->makeChild<Ling::Node>();
			row->setWidthPercent(100.f);
			row->setFlexDirection(Ling::FlexDirection::Row);
			row->setAlignItems(Ling::Align::Center);

			auto* icon = row->makeChild<Ling::Label>();
			icon->setText(glyph);
			icon->setFontFamily(Icon::Family);
			icon->setFontSize(24.f);
			icon->setColor(iconColor);
			icon->setMarginRight(10.f);
			icon->setFlexShrink(0.f);

			auto* info = row->makeChild<Ling::Node>();
			info->setFlexDirection(Ling::FlexDirection::Column);
			info->setFlexGrow(1.f);
			info->setFlexShrink(1.f);

			auto* title = info->makeChild<Ling::Label>();
			title->setText(std::filesystem::path(f).filename().wstring());
			title->setFontSize(SettingTheme::fontBase);
			title->setColor(bubbleFg());   // 跟着气泡底色，深色模式下是黑字
			title->setFlexShrink(1.f);

			auto* dir = info->makeChild<Ling::Label>();
			dir->setText(std::filesystem::path(f).parent_path().wstring());
			dir->setFontSize(SettingTheme::fontXs);
			dir->setColor(bubbleFgDim());
			dir->setFlexShrink(1.f);
			return;
		}

		// 多条：逐行「图标 + 文件名」
		for (const auto& f : files) {
			uint32_t iconColor{};
			const wchar_t* glyph = fileGlyph(f, &iconColor);
			auto* row = filesBox_->makeChild<Ling::Node>();
			row->setWidthPercent(100.f);
			row->setHeight(26.f);
			row->setFlexShrink(0.f);
			row->setMarginBottom(4.f);
			row->setFlexDirection(Ling::FlexDirection::Row);
			row->setAlignItems(Ling::Align::Center);

			auto* icon = row->makeChild<Ling::Label>();
			icon->setText(glyph);
			icon->setFontFamily(Icon::Family);
			icon->setFontSize(14.f);
			icon->setColor(iconColor);
			icon->setMarginRight(6.f);
			icon->setFlexShrink(0.f);

			auto* name = row->makeChild<Ling::Label>();
			name->setText(std::filesystem::path(f).filename().wstring());
			name->setFontSize(SettingTheme::fontBase);
			name->setColor(bubbleFg());   // 跟着气泡底色，深色模式下是黑字
			name->setFlexShrink(1.f);
		}
	}

	bool workAreaAt(const RECT& cellScreen, MONITORINFO& mi) const
	{
		mi = MONITORINFO{ sizeof(MONITORINFO) };
		return GetMonitorInfoW(MonitorFromRect(&cellScreen, MONITOR_DEFAULTTONEAREST), &mi) != FALSE;
	}

	// 必须「不激活」地显示：激活会抢走设置窗焦点，触发 onBlur / onFocus（重扫重建网格），看起来就是闪
	void showNoActivate()
	{
		if (hwnd) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
	}

	bool ensureWindow(int pxW, int pxH)
	{
		if (hwnd) return true;
		setSize((float)pxW / dpi, (float)pxH / dpi);
		createNativeWindow(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, WS_POPUP);
		return hwnd != nullptr;
	}

	void place(int pxW, int pxH, const RECT& cellScreen, const MONITORINFO& mi)
	{
		// 卡片右侧；放不下翻到左侧；纵向与卡片居中对齐，最后夹进工作区
		const int gap = (int)std::lround(8 * dpi);
		int x = cellScreen.right + gap;
		if (x + pxW > mi.rcWork.right) x = cellScreen.left - gap - pxW;
		const int maxX = (std::max)((int)mi.rcWork.left, (int)mi.rcWork.right - pxW);
		const int maxY = (std::max)((int)mi.rcWork.top, (int)mi.rcWork.bottom - pxH);
		x = std::clamp(x, (int)mi.rcWork.left, maxX);
		int y = (cellScreen.top + cellScreen.bottom) / 2 - pxH / 2;
		y = std::clamp(y, (int)mi.rcWork.top, maxY);
		setPosition(x, y);
	}

	Ling::Node* card_{ nullptr };
	Ling::ImageBox* img_{ nullptr };
	Ling::TextBox* text_{ nullptr };
	Ling::Node* filesBox_{ nullptr };
	std::wstring path_;
};

namespace {
	// 网格布局常量（3 列、每批 3 行 = 9 个）
	constexpr int kCols = 3;
	constexpr int kBatch = 9;
	constexpr float kGap = 10.f;
	constexpr float kPad = 8.f;
	constexpr float kThumbH = 104.f;
	constexpr float kCaptionH = 24.f;
	// 卡片顶部一行：左图标 / 右「复制」
	constexpr float kHeaderH = 24.f;
	constexpr float kHeaderGap = 6.f;
	// 「复制成功」提示：用窗口定时器收起
	constexpr UINT kToastTimerId = 0x5A1A;
	constexpr UINT kToastMs = 1200;
	// 复制后延迟置顶：先让提示亮一会儿，再挪到最前面
	constexpr UINT kReorderTimerId = 0x5A1B;
	constexpr UINT kReorderDelayMs = 700;

	std::wstring replaceOnce(std::wstring s, const std::wstring& from, const std::wstring& to)
	{
		auto pos = s.find(from);
		if (pos != std::wstring::npos) s.replace(pos, from.size(), to);
		return s;
	}
	// 搜索命中：关键词的每个字按顺序出现即可，中间允许隔着别的字。
	// 例：正文 "你好呀我的世界" 命中 "你好" / "呀我" / "你好世界"。
	bool searchHit(const std::wstring& hay, const std::wstring& kw)
	{
		if (kw.empty()) return true;
		size_t k = 0;
		for (const wchar_t c : hay) {
			if (c != kw[k]) continue;
			if (++k == kw.size()) return true;
		}
		return false;
	}
	// 搜索归一化：小写 + 去掉空格/标点/控制符（用 Unicode 字符分类，中文标点也覆盖）。
	// 例："你好呀，我的世界" → "你好呀我的世界"，所以 "呀我" / "的世" 都能命中。
	std::wstring searchNormalize(const std::wstring& s)
	{
		if (s.empty()) return {};
		std::wstring out;
		out.reserve(s.size());
		std::vector<WORD> types(s.size());
		if (GetStringTypeW(CT_CTYPE1, s.c_str(), (int)s.size(), types.data())) {
			for (size_t i = 0; i < s.size(); ++i) {
				if (types[i] & (C1_SPACE | C1_BLANK | C1_PUNCT | C1_CNTRL)) continue;
				out.push_back((wchar_t)towlower(s[i]));
			}
		}
		else {
			for (const wchar_t c : s) {
				if (iswspace(c) || iswpunct(c) || iswcntrl(c)) continue;
				out.push_back((wchar_t)towlower(c));
			}
		}
		return out;
	}
	long long nowMs()
	{
		return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
	}
	// "YYYY-MM-DD" → 当天 0 点 / 23:59:59.999 的毫秒时间戳（本地时区）
	bool parseDateRange(const std::wstring& startText, const std::wstring& endText,
		long long& startMs, long long& endMs)
	{
		int y1 = 0, m1 = 0, d1 = 0, y2 = 0, m2 = 0, d2 = 0;
		if (swscanf_s(startText.c_str(), L"%d-%d-%d", &y1, &m1, &d1) != 3) return false;
		if (swscanf_s(endText.c_str(), L"%d-%d-%d", &y2, &m2, &d2) != 3) return false;
		struct tm a{};
		a.tm_year = y1 - 1900; a.tm_mon = m1 - 1; a.tm_mday = d1;
		struct tm b{};
		b.tm_year = y2 - 1900; b.tm_mon = m2 - 1; b.tm_mday = d2;
		b.tm_hour = 23; b.tm_min = 59; b.tm_sec = 59;
		startMs = (long long)mktime(&a) * 1000;
		endMs = (long long)mktime(&b) * 1000 + 999;
		return startMs > 0 && endMs > 0 && startMs <= endMs;
	}
	std::wstring dateText(long long ms)
	{
		time_t tt = (time_t)(ms / 1000);
		struct tm t{};
		localtime_s(&t, &tt);
		wchar_t buf[16]{};
		wcsftime(buf, 16, L"%Y-%m-%d", &t);
		return buf;
	}
	// 气泡外壳：圆角 + 指向筛选按钮的小箭头（复用工具栏属性栏那套绘制）
	class FilterBubble : public Ling::Canvas
	{
	public:
		FilterBubble(Ling::WinBase* w) : Ling::Canvas(w)
		{
			// 每次刷新都会重绘：用双缓冲后端，避免「清屏 → 重画」的中间态被合成器采样到而闪烁
			enableSwapChain();
		}
		void configure(bool tipDown, float arrowX)
		{
			tipDown_ = tipDown;
			arrowX_ = arrowX;
			if (win) win->refresh();
		}
	protected:
		void layout() override
		{
			Ling::Canvas::layout();
			auto ctx = startPaint();
			if (!ctx) return;
			ctx->Clear(0);
			const float d = (win && win->dpi > 0.f) ? win->dpi : 1.f;
			Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> bg;
			// 气泡底与描边跟随软件主题（与设置页浮层同色），不沿用截图工具栏配色
			ctx->CreateSolidColorBrush(Ling::Color(SettingTheme::popover).getD2DColor(), bg.GetAddressOf());
			ToolbarChrome::paintPropBubble(ctx, w, h, d, bg.Get(), arrowX_, tipDown_,
				ToolbarTheme::borderWidth, SettingTheme::border);
			finishPaint();
		}
	private:
		bool tipDown_{ false };
		float arrowX_{ 0.f };
	};
	// 日历切月箭头：复用「下拉」字形旋转 90°，与其它图标字重一致
	// （Composition 的 CenterPoint 是视觉局部坐标，得等 layout 拿到真实 w/h 再设）
	struct ChevronNavBtn : Ling::Button
	{
		explicit ChevronNavBtn(Ling::WinBase* w) : Ling::Button(w) {}
		void setAngle(float a) { angle_ = a; }
	protected:
		void layout() override
		{
			Ling::Button::layout();
			visual.CenterPoint({ w * 0.5f, h * 0.5f, 0.f });
			visual.RotationAngleInDegrees(angle_);
		}
	private:
		float angle_{ 0.f };
	};
	// 历史条目 → 来源文案
	std::wstring sourceLabel(const std::wstring& src)
	{
		if (src == L"fullscreen") return Lang::get(L"setting.srcFullscreen");
		if (src == L"focused")    return Lang::get(L"setting.srcFocused");
		if (src == L"long")       return Lang::get(L"setting.srcLong");
		if (src == L"record")     return Lang::get(L"setting.srcRecord");
		if (src == L"autosave")   return Lang::get(L"setting.srcAutoSave");
		if (src == L"clipboard")  return Lang::get(L"setting.historyTabClipboard");
		return Lang::get(L"setting.srcCapture");
	}
	// 生成一个 26×26 的圆形浮层图标按钮（叠在缩略图右下角）
	Ling::Button* mkIconBtn(Ling::Node* parent, const wchar_t* glyph,
		float rightOffset, bool danger, std::function<void()> onClick)
	{
		auto* b = parent->makeChild<Ling::Button>();
		b->setPositionType(Ling::Position::Absolute);
		b->setPosition(Ling::Edge::Right, rightOffset);
		b->setPosition(Ling::Edge::Bottom, 6.f);
		b->setSize(26.f, 26.f);
		b->setPadding(0.f, 0.f, 0.f, 0.f);
		b->setBorderRadius(SettingTheme::radiusFull);
		b->setBorder(0.f, 0);
		b->setText(glyph);
		b->setFontFamily(Icon::Family);
		b->setFontSize(14.f);
		b->setColor(0xFFFFFFE1);
		b->setBg(0x00000078);
		b->setHoverBg(danger ? 0xFF383CD2u : 0x000000AAu);
		b->setHoverColor(0xFFFFFFFF);
		b->setFlexDirection(Ling::FlexDirection::Row);
		b->setJustifyContent(Ling::Justify::Center);
		b->setAlignItems(Ling::Align::Center);
		b->onClick.add([cb = std::move(onClick)](Ling::Button*) { cb(); });
		return b;
	}
	// 在资源管理器里定位文件（录屏条目用：视频不复制到历史目录，定位的是用户自己保存的那份）
	void revealInExplorer(const std::wstring& path)
	{
		if (path.empty()) return;
		const std::wstring args = L"/select,\"" + path + L"\"";
		ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
	}
	// 底部「清空…」文案的语言键：随导航切换
	const wchar_t* clearLabelKeyFor(int tab)
	{
		switch (tab) {
		case 1: return L"setting.clearShots";
		case 2: return L"setting.clearRecords";
		case 3: return L"setting.clearOther";
		case 4: return L"setting.clearClipboard";
		default: return L"setting.clearAll";
		}
	}
	// —— 剪切板类型标签 ——
	bool clipExtIs(const std::wstring& path, std::initializer_list<const wchar_t*> exts)
	{
		const auto dot = path.find_last_of(L'.');
		if (dot == std::wstring::npos) return false;
		std::wstring e = path.substr(dot + 1);
		for (auto& c : e) c = (wchar_t)towlower(c);
		for (auto x : exts) if (e == x) return true;
		return false;
	}
	bool clipIsImageExt(const std::wstring& p)
	{
		return clipExtIs(p, { L"png", L"jpg", L"jpeg", L"webp", L"bmp", L"gif", L"avif", L"jxl" });
	}
	bool clipIsVideoExt(const std::wstring& p)
	{
		return clipExtIs(p, { L"mp4", L"mov", L"mkv", L"avi", L"wmv", L"webm", L"flv", L"m4v", L"gif", L"apng" });
	}
	bool clipIsAudioExt(const std::wstring& p)
	{
		return clipExtIs(p, { L"mp3", L"wav", L"flac", L"aac", L"m4a", L"ogg", L"oga", L"wma",
			L"ape", L"opus", L"aiff", L"aif", L"mid", L"midi", L"amr", L"mka", L"ac3", L"dts", L"caf", L"wv" });
	}
	bool clipAllFiles(const std::vector<std::wstring>& files, bool image)
	{
		if (files.empty()) return false;
		for (const auto& f : files) {
			if (image ? !clipIsImageExt(f) : !clipIsVideoExt(f)) return false;
		}
		return true;
	}
	bool clipAllAudio(const std::vector<std::wstring>& files)
	{
		if (files.empty()) return false;
		for (const auto& f : files) {
			if (!clipIsAudioExt(f)) return false;
		}
		return true;
	}
	// 复制的是文件夹（可能多个）：标签显示「文件夹」
	bool clipAllDirs(const std::vector<std::wstring>& files)
	{
		if (files.empty()) return false;
		for (const auto& f : files) {
			std::error_code ec;
			if (!std::filesystem::is_directory(f, ec)) return false;
		}
		return true;
	}
	// —— 文字型剪切板内容的细分：纯文字 / 链接 / 色值 ——
	enum class ClipTextKind { Plain, Link, Color };
	std::wstring trimCopy(std::wstring s)
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
	bool hexNibble(wchar_t c, int& v)
	{
		if (c >= L'0' && c <= L'9') { v = c - L'0'; return true; }
		if (c >= L'a' && c <= L'f') { v = c - L'a' + 10; return true; }
		if (c >= L'A' && c <= L'F') { v = c - L'A' + 10; return true; }
		return false;
	}
	// 解析常见色值：#RGB / #RRGGBB / #RRGGBBAA / rgb(r,g,b) / rgba(r,g,b,a)
	bool parseColorText(const std::wstring& raw, uint32_t& rgb)
	{
		const std::wstring s = trimCopy(raw);
		if (s.empty()) return false;
		if (s[0] == L'#') {
			const size_t n = s.size() - 1;
			if (n == 3 || n == 6 || n == 8) {
				std::vector<int> d(n);
				for (size_t i = 0; i < n; ++i)
					if (!hexNibble(s[i + 1], d[i])) return false;
				int r = 0, g = 0, b = 0;
				if (n == 3) {
					r = d[0] * 17; g = d[1] * 17; b = d[2] * 17;
				}
				else {
					r = d[0] * 16 + d[1];
					g = d[2] * 16 + d[3];
					b = d[4] * 16 + d[5];
				}
				rgb = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
				return true;
			}
			return false;
		}
		std::wstring l = s;
		for (auto& c : l) c = (wchar_t)towlower(c);
		const bool isRgb = l.rfind(L"rgb(", 0) == 0;
		const bool isRgba = l.rfind(L"rgba(", 0) == 0;
		if (!isRgb && !isRgba) return false;
		if (s.back() != L')') return false;
		const size_t open = s.find(L'(');
		std::wstring inside = s.substr(open + 1, s.size() - open - 2);
		std::vector<int> parts;
		size_t start = 0;
		while (start <= inside.size()) {
			const size_t comma = inside.find(L',', start);
			const std::wstring piece = trimCopy(inside.substr(start,
				comma == std::wstring::npos ? std::wstring::npos : comma - start));
			if (piece.empty()) break;
			wchar_t* end = nullptr;
			const long v = wcstol(piece.c_str(), &end, 10);
			if (!end || *end != L'\0' || v < 0 || v > 255) return false;
			parts.push_back((int)v);
			if (comma == std::wstring::npos) break;
			start = comma + 1;
		}
		if ((isRgb && parts.size() != 3) || (isRgba && parts.size() != 4)) return false;
		rgb = ((uint32_t)parts[0] << 16) | ((uint32_t)parts[1] << 8) | (uint32_t)parts[2];
		return true;
	}
	ClipTextKind clipTextKind(const std::wstring& text, uint32_t* rgb = nullptr)
	{
		const std::wstring s = trimCopy(text);
		uint32_t c = 0;
		if (parseColorText(s, c)) {
			if (rgb) *rgb = c;
			return ClipTextKind::Color;
		}
		if (looksLikeLink(s)) return ClipTextKind::Link;
		return ClipTextKind::Plain;
	}
	// 剪切板条目的类型标签：图片 / 视频 / 文字 / SVG / 文件
	std::wstring clipTypeTag(const Setting::HistoryItem& it)
	{
		if (!it.files.empty()) {
			if (clipAllDirs(it.files)) return Lang::get(L"setting.tagFolder");
			if (clipAllFiles(it.files, true)) return Lang::get(L"setting.tagImage");
			if (clipAllFiles(it.files, false)) return Lang::get(L"setting.tagVideo");
			if (clipAllAudio(it.files)) return Lang::get(L"setting.tagMusic");
			return Lang::get(L"setting.tagFile");
		}
		if (!it.filePath.empty()) return Lang::get(L"setting.tagImage");
		if (Util::isSvgText(it.text)) return L"SVG";
		const auto kind = clipTextKind(it.text);
		if (kind == ClipTextKind::Link) return Lang::get(L"setting.tagLink");
		if (kind == ClipTextKind::Color) return Lang::get(L"setting.tagColor");
		return Lang::get(L"setting.tagText");
	}
	// 文件大小 → "12.3 MB" 这类显示文本
	std::wstring formatFileSize(uintmax_t bytes)
	{
		if (bytes < 1024) return std::to_wstring(bytes) + L" B";
		const wchar_t* units[] = { L"KB", L"MB", L"GB", L"TB" };
		double v = (double)bytes;
		int unit = -1;
		do {
			v /= 1024.0;
			++unit;
		} while (v >= 1024.0 && unit < 3);
		wchar_t buf[32]{};
		swprintf_s(buf, L"%.1f %s", v, units[unit]);
		return buf;
	}
	// 把图片文件贴到屏幕中央（普通贴图）
	void pinImageFile(const std::wstring& path)
	{
		auto [w, h, px] = Util::loadImagePixels(path);
		if (w <= 0 || h <= 0 || px.empty()) return;
		RECT rc{};
		SystemParametersInfo(SPI_GETWORKAREA, 0, &rc, 0);
		const int areaW = (int)rc.right - (int)rc.left;
		const int areaH = (int)rc.bottom - (int)rc.top;
		const int x = (int)rc.left + (std::max)(0, (areaW - w) / 2);
		const int y = (int)rc.top + (std::max)(0, (areaH - h) / 2);
		WinPin::initFromData(x, y, w, h, px);
	}
	// 剪切板里的 SVG 代码：渲染成图片后贴到屏幕中央
	void pinSvgText(const std::wstring& svg)
	{
		int w = 0, h = 0;
		std::vector<BYTE> px;
		if (!Util::renderSvgText(svg, 1024, w, h, px)) return;
		RECT rc{};
		SystemParametersInfo(SPI_GETWORKAREA, 0, &rc, 0);
		const int areaW = (int)rc.right - (int)rc.left;
		const int areaH = (int)rc.bottom - (int)rc.top;
		const int x = (int)rc.left + (std::max)(0, (areaW - w) / 2);
		const int y = (int)rc.top + (std::max)(0, (areaH - h) / 2);
		WinPin::initFromData(x, y, w, h, px);
	}
	// 卡片缩略图：SVG 代码渲染成 PNG 缓存（同一 id 只渲染一次；空图 / 坏图会重渲）
	std::wstring svgPreviewPath(const Setting::HistoryItem& it)
	{
		if (it.id.empty()) return {};
		auto dir = Setting::get()->getClipboardDir();
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		// 缓存版本号跟着渲染逻辑走：裁掉透明留白后旧缓存不再适用，bump 一版让老记录重渲
		const auto path = dir / (it.id + L".svg3.png");
		if (std::filesystem::exists(path, ec)) {
			const auto [pw, ph] = Util::imageSize(path.wstring());
			if (pw > 0 && ph > 0) return path.wstring();
			std::filesystem::remove(path, ec);
		}
		int w = 0, h = 0;
		std::vector<BYTE> px;
		if (!Util::renderSvgText(it.text, 512, w, h, px)) return {};
		if (!Util::saveToFile(path.wstring(), w, h, px.data())) return {};
		return path.wstring();
	}
}

WinSettingHistory::WinSettingHistory(Ling::WinBase* parent) : Ling::Node(parent)
{
	// 本页整页不滚动（由 WinSetting 直接挂到视口容器，高度恒等于视口高度），
	// 只让中部网格自建滚动：底行因此始终贴住视口底部。
	setFlexDirection(Ling::FlexDirection::Column);

	// ---- 顶部一行：导航（全部/截图/录屏） + 时间筛选（贴最右） ----
	auto* topRow = makeChild<Ling::Node>();
	topRow->setFlexDirection(Ling::FlexDirection::Row);
	topRow->setWidthPercent(100.f);
	topRow->setAlignItems(Ling::Align::Center);
	topRow->setMarginBottom(SettingTheme::blockGap);

	navPills = SettingUi::segmentedPills(topRow,
		{ Lang::get(L"setting.historyTabAll"), Lang::get(L"setting.historyTabShots"),
		  Lang::get(L"setting.historyTabRecord"), Lang::get(L"setting.historyTabOther"),
		  Lang::get(L"setting.historyTabClipboard") },
		sourceTab,
		[this](int idx) {
			sourceTab = idx;
			shownCount = kBatch;
			this->closeFilterPanel();
			this->closeModal();
			// 筛选对所有分类通用（剪切板也按时间 / 关键词 / 标签筛）
			if (filterHost) filterHost->show();
			if (clearBtn) clearBtn->setText(Lang::get(clearLabelKeyFor(idx)));
			// 剪切板只记录路径，没有本地历史目录，不显示「打开历史目录」
			if (openDirBtn) idx == 4 ? openDirBtn->hide() : openDirBtn->show();
			this->refreshGrid();
		});
	if (navPills.row) navPills.row->setFlexShrink(0.f);
	// 三段等宽：以侧栏「功能设置」四个字的文本宽度为准
	{
		float textW = 0.f;
		if (auto layout = Ling::D2D::get()->makeTextLayout(
			Lang::get(L"setting.function"), SettingTheme::fontBase * win->dpi)) {
			DWRITE_TEXT_METRICS m{};
			layout->GetMetrics(&m);
			textW = m.width / (win->dpi > 0.f ? win->dpi : 1.f);
		}
		if (textW <= 0.f) textW = SettingTheme::fontBase * 4.f;
		const float pillW = std::ceil(textW) + SettingTheme::sp4 * 2.f;
		for (auto* b : navPills.btns) {
			b->setWidth(pillW);
			b->setFlexShrink(0.f);
		}
	}

	// 中间弹性空白：把时间筛选推到最右
	auto* spacer = topRow->makeChild<Ling::Node>();
	spacer->setFlexGrow(1.f);
	spacer->setFlexShrink(1.f);

	// 顶行压暗层：盖住导航等，但排在筛选按钮之前 → 按钮始终在它上面
	topRowDim = topRow->makeChild<Ling::Node>();
	topRowDim->setPositionType(Ling::Position::Absolute);
	topRowDim->setPosition(Ling::Edge::Left, 0.f);
	topRowDim->setPosition(Ling::Edge::Top, 0.f);
	topRowDim->setPosition(Ling::Edge::Right, 0.f);
	topRowDim->setPosition(Ling::Edge::Bottom, 0.f);
	topRowDim->setBg(0x00000059);
	topRowDim->hide();

	filterHost = topRow->makeChild<Ling::Node>();
	filterHost->setFlexDirection(Ling::FlexDirection::Row);
	filterHost->setAlignItems(Ling::Align::Center);
	filterHost->setFlexShrink(0.f);

	// 筛选按钮：点击展开页内气泡面板（不再使用下拉）
	filterBtn = SettingUi::btnOutline(filterHost, Icon::Filter, [this]() { this->toggleFilterPanel(); });
	filterBtn->setFontFamily(Icon::Family);
	filterBtn->setFontSize(16.f);
	filterBtn->setWidth(SettingTheme::ctrlH);
	filterBtn->setHeight(SettingTheme::ctrlH);
	filterBtn->setPadding(0.f, 0.f, 0.f, 0.f);
	filterBtn->setFlexShrink(0.f);
	filterBtn->setColor(SettingTheme::textMuted);
	filterBtn->setHoverColor(SettingTheme::textPrimary);

	// ---- 中部：空态 / 缩略图网格（互斥显示，同占顶行与底行之间的剩余空间）----
	// 空态直接作为中部区域的 flex 兄弟（flexGrow 占满），文字在其中上下居中。
	emptyLabel = makeChild<Ling::Label>();
	emptyLabel->setText(Lang::get(L"setting.noHistory"));
	emptyLabel->setFontSize(SettingTheme::fontBase);
	emptyLabel->setColor(SettingTheme::textMuted);
	emptyLabel->setWidthPercent(100.f);
	emptyLabel->setFlexGrow(1.f);
	emptyLabel->setFlexShrink(1.f);
	emptyLabel->setJustifyContent(Ling::Justify::Center);
	emptyLabel->setAlignItems(Ling::Align::Center);

	// 网格滚动区（内容溢出时只滚它）
	gridScroll = makeChild<Ling::ScrollerBox>();
	gridScroll->setWidthPercent(100.f);
	gridScroll->setFlexGrow(1.f);
	gridScroll->setFlexShrink(1.f);
	gridScroll->setScrollBarVisible(false);   // 不显示滚动条（滚轮仍可滚）
	// ScrollerBox::setChild 会把子节点挂到它的 content 上（滚动内容），下面直接往 gridScroll 里加。
	gridScroll->content->setFlexDirection(Ling::FlexDirection::Column);
	gridScroll->content->setWidthPercent(100.f);

	gridHost = gridScroll->makeChild<Ling::Node>();
	gridHost->setFlexDirection(Ling::FlexDirection::Column);
	gridHost->setWidthPercent(100.f);

	// ---- 底部一行：左「打开目录 / 清空历史」，右「当前条目」 ----
	auto* footer = makeChild<Ling::Node>();
	footer->setFlexDirection(Ling::FlexDirection::Row);
	footer->setWidthPercent(100.f);
	footer->setAlignItems(Ling::Align::Center);
	footer->setMarginTop(SettingTheme::blockGap);

	// 剪切板变动时，如果正停在「剪切板」页（或「全部」正用标签筛剪切板），即时刷新列表
	{
		auto weak = getWeakThis();
		Setting::get()->clipboardChanged = [this, weak]() {
			auto run = [this, weak]() {
				if (!weak.lock()) return;
				if (this->sourceTab == 4 || (this->sourceTab == 0 && !this->filter.tag.empty()))
					this->refreshGrid();
			};
			if (auto* app = Ling::App::get()) app->dq.TryEnqueue(run);
			else run();
		};
	}

	openDirBtn = SettingUi::btnOutline(footer, Lang::get(L"setting.openHistoryDir"), [this]() {
		const auto dir = Util::outputDir();
		if (dir.empty()) return;
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		ShellExecute(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		});
	openDirBtn->setMarginRight(SettingTheme::sp2);

	// 清空按钮：文案随导航切换（全部/截图/录屏/其他/剪切板）
	clearBtn = SettingUi::btnDestructive(footer, Lang::get(clearLabelKeyFor(sourceTab)), [this]() {
		// 确认文案与动作都跟随当前导航分类
		const wchar_t* msgKey = L"setting.clearHistoryConfirmAll";
		switch (this->sourceTab) {
		case 1: msgKey = L"setting.clearHistoryConfirmShots"; break;
		case 2: msgKey = L"setting.clearHistoryConfirmRecord"; break;
		case 3: msgKey = L"setting.clearHistoryConfirmOther"; break;
		case 4: msgKey = L"setting.clearHistoryConfirmClipboard"; break;
		default: break;
		}
		SettingUi::ConfirmDialog::ask(win, Lang::get(msgKey),
			[this](bool ok) {
				if (!ok) return;
				switch (this->sourceTab) {
				case 3:
					// 「其他」：只清用户自己放进目录的图片/视频（移入回收站）
					Setting::get()->clearOtherFiles();
					break;
				case 4:
					Setting::get()->clearClipboardHistory();
					break;
				default:
					// 全部 / 截图 / 录屏：只删本软件登记过的文件
					Setting::get()->clearAllHistory(this->sourceTab);
					if (this->sourceTab == 0) Setting::get()->clearClipboardHistory();
					break;
				}
				this->refreshGrid();
			});
	});

	auto* footerSpacer = footer->makeChild<Ling::Node>();
	footerSpacer->setFlexGrow(1.f);
	footerSpacer->setFlexShrink(1.f);

	countLabel = footer->makeChild<Ling::Label>();
	countLabel->setFontSize(SettingTheme::fontSm);
	countLabel->setColor(SettingTheme::textMuted);

	// 三个图标按钮的停悬提示：与截图工具栏同一套系统 tooltip（comctl32 TOOLTIPS_CLASS）
	tip = std::make_unique<Tip>(win);
	// 气泡 / 弹窗盖住卡片时不冒停悬提示
	tip->suppressed = [this]() { return this->overlayOpen(); };
	if (filterBtn) tip->bind(filterBtn, Lang::get(L"setting.filter"));

	// 滚轮 / 拖滑块 / 拉窗口 → 重判要不要续补下一批
	// ⚠ 必须在本页的 gridScroll 建好之后再订阅：ScrollerBox 自己的滚轮处理先注册、
	//   先执行，这里读到的 getScrollY() 才是滚动后的新值。
	auto weak = getWeakThis();
	wheelTok = win->onMouseWheel.add([this, weak](POINT, float) {
		if (!weak.lock()) return;
		this->hidePreview();        // 滚动后卡片位置变了，先收起大图
		this->maybeScrollGrew();
		});
	moveTok = win->onMouseMove.add([this, weak](POINT pos) {
		if (!weak.lock()) return;
		this->updateHover(pos);     // 停悬才浮出 复制/贴图/删除
		this->maybeScrollGrew();
		});
	sizeTok = win->onSizeChanged.add([this, weak]() {
		if (!weak.lock()) return;
		this->scheduleFillCheck();
		});
	timerTok = win->onTimer.add([this, weak](UINT id) {
		if (!weak.lock()) return;
		if (id == kToastTimerId) this->hideCopyToast();
		else if (id == kReorderTimerId) this->applyReorderToTop();
		});
	// 窗口重新获得焦点：重扫目录。用户可能刚在资源管理器里从回收站还原了截图，
	// 回到设置窗就应该能在历史里看到（历史以目录里的真实文件为准）。
	focusTok = win->onFocus.add([this, weak]() {
		if (!weak.lock()) return;
		auto rescan = [this, weak]() { if (weak.lock()) this->refreshGrid(); };
		if (auto* app = Ling::App::get()) app->dq.TryEnqueue(rescan);
		else rescan();
		});
	// 点击面板/弹窗以外的地方 → 收起；Esc 同样收起
	mouseDownTok = win->onMouseDown.add([this, weak](POINT pos, bool) {
		if (!weak.lock()) return;
		this->dismissingClick = false;
		this->closeTransientUi(pos);
		});
	keyTok = win->onKeyDown.add([this, weak](UINT key) {
		if (!weak.lock()) return;
		if (key == VK_ESCAPE) this->closeTransientUi({ INT_MAX, INT_MAX });
		});
	// 停在可预览的卡片上显示"点按"光标（提示可以点开 / 双击）；功能关闭时不接管光标
	cursorTok = win->onCursor.add([this, weak](bool* handled) {
		if (!weak.lock() || !handled || !win) return;
		if (!Setting::get()->getHoverPreview()) return;
		POINT pt{};
		GetCursorPos(&pt);
		ScreenToClient(win->hwnd, &pt);
		for (const auto& c : cells) {
			if (!c.thumb || (c.previewPath.empty() && c.previewText.empty() && c.previewFiles.empty()))
				continue;
			if (c.thumb->isPosIn(pt)) {
				SetCursor(LoadCursor(nullptr, IDC_HAND));
				*handled = true;
				return;
			}
		}
		});
	// 窗口失焦（切到别的程序）：大图跟着收起，别一直悬在桌面上
	blurTok = win->onBlur.add([this, weak]() {
		if (!weak.lock()) return;
		this->hidePreview();
		});

	refreshGrid();
	// 内容区只有外层 content 提供底部留白：清掉末尾元素的 marginBottom，使底部与左右一致。
	SettingUi::trimTrailingGap(this);
}

WinSettingHistory::~WinSettingHistory()
{
	if (auto* s = Setting::get()) s->clipboardChanged = nullptr;
	if (preview_) preview_->hideNow();
	win->onMouseWheel.remove(wheelTok);
	win->onMouseMove.remove(moveTok);
	win->onSizeChanged.remove(sizeTok);
	win->onTimer.remove(timerTok);
	win->onFocus.remove(focusTok);
	win->onMouseDown.remove(mouseDownTok);
	win->onKeyDown.remove(keyTok);
	win->onCursor.remove(cursorTok);
	win->onBlur.remove(blurTok);
	win->killTimer(kToastTimerId);
	win->killTimer(kReorderTimerId);
}

// 分类归属（不看标签筛选）。
// 「全部」默认只列本软件登记过的截图/录屏；用户文件（其他）与剪切板只在用自定义标签筛选时
// 才被带进「全部」（见 matchSourceTab）。
bool WinSettingHistory::baseTabMatch(const Setting::HistoryItem& it) const
{
	const bool userFile = (it.id == it.filePath);
	const bool clip = (it.source == L"clipboard");
	if (sourceTab == 4) return clip;
	if (clip) return sourceTab == 0;
	if (sourceTab == 3) return userFile;
	if (sourceTab == 0) return true;
	if (userFile) return false;
	const bool isRecord = (it.source == L"record");
	return sourceTab == 2 ? isRecord : !isRecord;
}

// 叠加标签筛选后的实际归属
bool WinSettingHistory::matchSourceTab(const Setting::HistoryItem& it) const
{
	if (!baseTabMatch(it)) return false;
	if (sourceTab != 0) return true;
	// 「全部」：剪切板 / 其他 里的用户文件只在自定义标签筛选下出现
	const bool extra = (it.source == L"clipboard") || (it.id == it.filePath);
	return extra ? !filter.tag.empty() : true;
}

// 导航分类 + 时间范围 + 标签 + 关键词，全部叠加生效
bool WinSettingHistory::matchFilters(const Setting::HistoryItem& it) const
{
	if (!matchSourceTab(it)) return false;
	const long long now = nowMs();
	if (filter.timeMode == 1 && it.createdAt < now - 7LL * 24 * 3600 * 1000) return false;
	if (filter.timeMode == 2 && it.createdAt < now - 30LL * 24 * 3600 * 1000) return false;
	if (filter.timeMode == 4) {
		// 今天：本地时间当天 0 点之后
		time_t tt = (time_t)(now / 1000);
		struct tm t{};
		localtime_s(&t, &tt);
		t.tm_hour = 0;
		t.tm_min = 0;
		t.tm_sec = 0;
		const long long startOfDay = (long long)mktime(&t) * 1000;
		if (it.createdAt < startOfDay) return false;
	}
	if (filter.timeMode == 3) {
		if (filter.rangeStart > 0 && it.createdAt < filter.rangeStart) return false;
		if (filter.rangeEnd > 0 && it.createdAt > filter.rangeEnd) return false;
	}
	if (!filter.tag.empty()) {
		const bool clip = (it.source == L"clipboard");
		bool hit = false;
		const auto tags = clip ? Setting::get()->getClipboardTags(it.id)
			: Setting::get()->getHistoryTags(it.filePath);
		for (auto& t : tags)
			if (_wcsicmp(t.c_str(), filter.tag.c_str()) == 0) { hit = true; break; }
		if (!hit) return false;
	}
	if (!filter.keyword.empty()) {
		const std::wstring kw = searchNormalize(filter.keyword);
		if (!kw.empty()) {
			const bool clip = (it.source == L"clipboard");
			// 正文：剪切板 = 文本本体 + 文件名（文件名只对剪切板参与）；
			// 截图/录屏的历史文件名只是时间戳，不参与关键词命中
			bool hit = false;
			if (clip) {
				// SVG 记录只按类型标签「SVG」/ 自定义标签命中，本体代码不参与搜索
				if (!Util::isSvgText(it.text)) hit = searchHit(searchNormalize(it.text), kw);
				for (const auto& f : it.files) {
					if (hit) break;
					hit = searchHit(searchNormalize(std::filesystem::path(f).filename().wstring()), kw);
				}
			}
			// 卡片上显示的标签：剪切板类型标签（链接 / SVG / 色值…）、历史来源标签、自定义标签
			const auto tags = clip ? Setting::get()->getClipboardTags(it.id)
				: Setting::get()->getHistoryTags(it.filePath);
			if (!hit && clip) hit = searchHit(searchNormalize(clipTypeTag(it)), kw);
			if (!hit && !clip && tags.empty()) {
				const std::wstring chip = (it.id == it.filePath)
					? Lang::get(it.source == L"record" ? L"setting.tagVideo" : L"setting.tagImage")
					: sourceLabel(it.source);
				hit = searchHit(searchNormalize(chip), kw);
			}
			for (auto& t : tags) {
				if (hit) break;
				if (searchHit(searchNormalize(t), kw)) hit = true;
			}
			if (!hit) return false;
		}
	}
	return true;
}

void WinSettingHistory::toggleFilterPanel()
{
	if (filterPanel) { closeFilterPanel(); return; }
	rebuildFilterPanel();
}

// 筛选按钮的选中态：面板展开时用停悬底色 + 绿色图标
void WinSettingHistory::setFilterBtnActive(bool on)
{
	if (!filterBtn) return;
	filterBtn->setColor(on ? SettingTheme::success : SettingTheme::textMuted);
	filterBtn->setHoverColor(on ? SettingTheme::success : SettingTheme::textPrimary);
	filterBtn->setBg(on ? SettingTheme::accent : SettingTheme::card);
	filterBtn->setHoverBg(SettingTheme::accent);
}

void WinSettingHistory::closeFilterPanel(bool keepMode)
{
	const bool hadOverlay = (filterPanel != nullptr) || (filterDim != nullptr);
	setFilterBtnActive(false);
	if (topRowDim) topRowDim->hide();
	if (!keepMode) panelMode = 0;
	if (filterDim) {
		if (filterDim->parent) filterDim->parent->removeChild(filterDim);
		filterDim = nullptr;
	}
	if (!filterPanel) {
		if (hadOverlay && win) win->layout();
		return;
	}
	if (filterPanel->parent) filterPanel->parent->removeChild(filterPanel);
	filterPanel = nullptr;
	// 收面板后同步跑一遍布局：把「覆盖层移除 + 各滚动区偏移重算 + 画布重绘」
	// 收敛到同一帧提交。展开时 applyMode() 也是这么做的 —— 否则下一帧才重排，
	// 合成器会先采到只移除了覆盖层、内容还没重排的中间态，看起来就是内容区闪一下。
	if (hadOverlay && win) win->layout();
}

// 在筛选按钮正下方（放不下就翻到上方）挂一个页内气泡面板
void WinSettingHistory::rebuildFilterPanel()
{
	closeFilterPanel(true);   // 保留 panelMode（区间模式重建时不回落）
	if (!filterBtn) return;
	setFilterBtnActive(true);
	if (tip) tip->hide();     // 已经挂着的停悬提示先收掉
	// 气泡内所有配色跟随软件主题（不使用截图工具栏主题）
	const uint32_t tFg = SettingTheme::textPrimary;
	// 停悬底色：比常态（分隔线色）深一档，两种主题各自取值
	const uint32_t tHover = SettingTheme::isDark() ? 0x3A3A3AFF : 0xE5E5E5FF;
	const uint32_t tActive = SettingTheme::brand;   // 标签选中态
	const uint32_t fgFaint = (tFg & 0xFFFFFF00u) | 0x66u;
	// 日历格：今天 = 次级底 + 主文字；选中 = 主色底 + 主色前景（深/浅主题各自成立）
	// 当前日期（含当前月/当前年）的默认底色：暗色与分隔线同色，浅色用 #F5F4F4
	const uint32_t todayBg = SettingTheme::isDark() ? SettingTheme::border : 0xF5F4F4FF;
	const uint32_t todayFg = SettingTheme::textPrimary;
	const uint32_t selBg = SettingTheme::primary;
	const uint32_t selFg = SettingTheme::primaryForeground;

	const float d = win->dpi > 0.f ? win->dpi : 1.f;
	const float pageW = w / d;
	const float pageH = h / d;
	const float caret = ToolbarTheme::caretSize;
	const float panelW = 320.f;
	const float panelH = 370.f;          // 固定高度（列表 / 日历两种模式一致）
	const float gap = 4.f;

	// Node 的 x/y 已是窗口内绝对坐标（physical）；换算成相对本页的逻辑坐标
	const float bx = (filterBtn->x - x) / d;
	const float by = (filterBtn->y - y) / d;
	const float bw = filterBtn->w / d;
	const float bh = filterBtn->h / d;

	// 面板右缘与筛选按钮右缘对齐（向左展开），箭头仍指向按钮中心
	float px = bx + bw - panelW;
	float py = by + bh + gap;
	bool tipDown = false;                // false：箭头在顶部（面板在按钮下方）
	if (py + panelH > pageH - 2.f) {     // 下方放不下 → 翻到按钮上方
		tipDown = true;
		py = by - gap - panelH;
	}
	px = (std::max)(2.f, (std::min)(px, pageW - panelW - 2.f));
	py = (std::max)(2.f, (std::min)(py, pageH - panelH - 2.f));

	// 压暗层：整页压暗；顶行整条由 topRowDim 压暗（按钮在它上层，保持高亮）
	{
		float rowX = 0.f, rowY = 0.f, rowW = pageW, rowH = 0.f;
		if (filterHost && filterHost->parent) {
			Ling::Node* topRow = filterHost->parent;
			rowX = (topRow->x - x) / d;
			rowY = (topRow->y - y) / d;
			rowW = topRow->w / d;
			rowH = topRow->h / d;
		}
		auto* dim = makeChild<Ling::Node>();
		dim->setPositionType(Ling::Position::Absolute);
		dim->setPosition(Ling::Edge::Left, 0.f);
		dim->setPosition(Ling::Edge::Top, 0.f);
		dim->setSize(pageW, pageH);
		filterDim = dim;

		auto addBand = [&](float bx0, float by0, float bw0, float bh0) {
			if (bw0 <= 0.f || bh0 <= 0.f) return;
			auto* n = dim->makeChild<Ling::Node>();
			n->setPositionType(Ling::Position::Absolute);
			n->setPosition(Ling::Edge::Left, bx0);
			n->setPosition(Ling::Edge::Top, by0);
			n->setSize(bw0, bh0);
			n->setBg(0x00000059);
		};
		addBand(0.f, 0.f, pageW, rowY);                               // 顶行上方留白
		addBand(0.f, rowY + rowH, pageW, pageH - (rowY + rowH));      // 顶行下方
		addBand(0.f, rowY, rowX, rowH);                               // 顶行左侧留白
		addBand(rowX + rowW, rowY, pageW - (rowX + rowW), rowH);      // 顶行右侧留白
		if (topRowDim) topRowDim->show();
	}

	auto* panel = makeChild<Ling::Node>();
	panel->setPositionType(Ling::Position::Absolute);
	panel->setPosition(Ling::Edge::Left, px);
	panel->setPosition(Ling::Edge::Top, py);
	panel->setSize(panelW, panelH);
	filterPanel = panel;

	auto* bubble = panel->makeChild<FilterBubble>();
	bubble->setPositionType(Ling::Position::Absolute);
	bubble->setSize(panelW, panelH);
	const float arrowX = (bx + bw * 0.5f - px) * d;   // 物理像素
	bubble->configure(tipDown, arrowX);

	const float pad = 12.f;             // 气泡内边距（四周统一）
	auto* content = panel->makeChild<Ling::Node>();
	content->setPositionType(Ling::Position::Absolute);
	content->setPosition(Ling::Edge::Left, pad);
	content->setPosition(Ling::Edge::Right, pad);
	content->setPosition(Ling::Edge::Top, 0.f);
	content->setPosition(Ling::Edge::Bottom, 0.f);
	content->setFlexDirection(Ling::FlexDirection::Column);
	const float padTop = tipDown ? pad : caret + pad;
	const float padBottom = tipDown ? caret + pad : pad;
	content->setPadding(0.f, padTop, 0.f, padBottom);

	// 时间快捷：近7天 / 近30天 / 选择时间
	auto* timeRow = content->makeChild<Ling::Node>();
	timeRow->setFlexDirection(Ling::FlexDirection::Row);
	timeRow->setAlignItems(Ling::Align::Center);
	timeRow->setFlexWrap(Ling::Wrap::Wrap);
	timeRow->setWidthPercent(100.f);
	timeRow->setMarginBottom(10.f);

	// 两套内容（默认列表 / 区间输入）都常驻，只做显隐切换，避免整块重建导致闪烁
	struct PanelSections {
		Ling::Node* list{ nullptr };
		Ling::Node* range{ nullptr };
		Ling::Node* rangeBtns{ nullptr };
		Ling::Button* pick{ nullptr };
	};
	auto sec = std::make_shared<PanelSections>();
	auto calOpen = std::make_shared<bool>(false);   // 日历是否展开（展开时日期框与按钮都让位）
	auto applyMode = [this, sec, calOpen, tHover, tActive, tFg]() {
		const bool range = (this->panelMode == 1);
		auto toggle = [](Ling::Node* n, bool show) {
			if (!n) return;
			if (show) n->show(); else n->hide();
		};
		toggle(sec->list, !range);
		toggle(sec->range, range);
		// 日历展开时取消/确认也让位，别又冒出来
		toggle(sec->rangeBtns, range && !*calOpen);
		if (sec->pick) {
			// 选中态：比停悬再深一档的底 + 正常文字色（不再变绿）
			const uint32_t pickedBg = SettingTheme::isDark() ? 0x4A4A4AFF : 0xE5E5E5FF;
			sec->pick->setBg(range ? pickedBg : 0);
			sec->pick->setColor(tFg);
			sec->pick->setHoverColor(tFg);
		}
		// show/hide 只先改可见性，坐标要等下一次 WM_PAINT 的布局才更新；
		// 这里同步跑一遍布局，避免被隐藏节点的旧坐标（0,0）先被合成器采样到而闪一下
		if (this->win) this->win->layout();
	};

	auto mkTimeBtn = [&](const std::wstring& text, int mode) {
		auto* b = timeRow->makeChild<Ling::Button>();
		b->setText(text);
		b->setFontSize(SettingTheme::fontSm);
		b->setHeight(SettingTheme::ctrlH);   // 32，与「恢复默认」按钮同高
		b->setPadding(10.f, 0.f, 10.f, 0.f);
		b->setBorderRadius(ToolbarTheme::hoverRadius);
		b->setBorder(0.f, 0);
		b->setBg(0);
		b->setHoverBg(tHover);
		b->setColor(tFg);
		b->setHoverColor(tFg);
		b->setJustifyContent(Ling::Justify::Center);
		b->setAlignItems(Ling::Align::Center);
		b->setMarginRight(6.f);
		b->setFlexShrink(0.f);
		b->onClick.add([this, mode, applyMode](Ling::Button*) {
			if (mode == 3) {
				// 复用气泡：分隔线以下切成日期区间输入（不弹新窗）；再次点击退回
				this->panelMode = (this->panelMode == 1) ? 0 : 1;
				applyMode();
				return;
			}
			// 面板会连本按钮一起销毁 → 动作延后一拍执行
			auto weak = getWeakThis();
			filter.timeMode = mode;
			shownCount = kBatch;
			auto apply = [this, weak]() {
				if (!weak.lock()) return;
				this->closeFilterPanel();
				this->refreshGrid();
			};
			if (auto* app = Ling::App::get()) app->dq.TryEnqueue(apply);
			else apply();
		});
		if (mode == 3) sec->pick = b;
	};
	mkTimeBtn(Lang::get(L"setting.filterToday"), 4);
	mkTimeBtn(Lang::get(L"setting.filter7d"), 1);
	mkTimeBtn(Lang::get(L"setting.filter30d"), 2);
	mkTimeBtn(Lang::get(L"setting.filterPickTime"), 3);

	// 分隔线
	auto* line1 = content->makeChild<Ling::Node>();
	line1->setHeight(1.f);
	line1->setWidthPercent(100.f);
	line1->setBg(SettingTheme::border);   // 与筛选按钮描边同色
	line1->setMarginBottom(10.f);

	// 区间输入（「选择时间」展开）：并列起止日期 + 日历 + 取消/确认
	auto* rangeSec = content->makeChild<Ling::Node>();
	rangeSec->setFlexDirection(Ling::FlexDirection::Column);
	rangeSec->setWidthPercent(100.f);
	rangeSec->setFlexGrow(1.f);
	rangeSec->setFlexShrink(1.f);
	sec->range = rangeSec;
	{
		// 没选过日期时留空，只显示「开始日期 / 结束日期」占位提示
		const std::wstring startText = (filter.timeMode == 3 && filter.rangeStart > 0)
			? dateText(filter.rangeStart) : L"";
		const std::wstring endText = (filter.timeMode == 3 && filter.rangeEnd > 0)
			? dateText(filter.rangeEnd) : L"";

		// 起止日期：并列两个只读输入框，点按其中一个在下方展开日历
		auto* dateRow = rangeSec->makeChild<Ling::Node>();
		dateRow->setFlexDirection(Ling::FlexDirection::Row);
		dateRow->setWidthPercent(100.f);
		dateRow->setMarginBottom(8.f);

		// 日期选择框：用按钮做外观（文字天然水平居中），点一下展开日历；
		// 暗色与分隔线同色，浅色用更浅一档的输入面
		const uint32_t dateBoxBg = SettingTheme::isDark() ? SettingTheme::border : 0xF5F4F4FF;
		auto mkDateBox = [&](const std::wstring& value, const std::wstring& hint) {
			auto* b = dateRow->makeChild<Ling::Button>();
			b->setHeight(SettingTheme::ctrlH);
			b->setFlexGrow(1.f);
			b->setFlexShrink(1.f);
			b->setBorderRadius(ToolbarTheme::hoverRadius);
			b->setBorder(0.f, 0);
			b->setBg(dateBoxBg);
			b->setHoverBg(dateBoxBg);
			b->setFontSize(SettingTheme::fontSm);
			b->setPadding(10.f, 0.f, 10.f, 0.f);
			b->setJustifyContent(Ling::Justify::Center);
			b->setAlignItems(Ling::Align::Center);
			b->setText(value.empty() ? hint : value);
			b->setColor(value.empty() ? fgFaint : tFg);
			b->setHoverColor(value.empty() ? fgFaint : tFg);
			return b;
		};
		auto* startBox = mkDateBox(startText, Lang::get(L"setting.filterStartDate"));
		startBox->setMarginRight(8.f);
		auto* endBox = mkDateBox(endText, Lang::get(L"setting.filterEndDate"));

		// 日历容器：点输入框时填充并显示
		auto* calHost = rangeSec->makeChild<Ling::Node>();
		calHost->setFlexDirection(Ling::FlexDirection::Column);
		calHost->setWidthPercent(100.f);
		calHost->setFlexGrow(1.f);
		calHost->setFlexShrink(1.f);
		calHost->setMarginTop(2.f);   // 与分隔线留 12（分隔线自带 10 的下边距）
		calHost->hide();

		struct CalState { int field{ 0 }; int year{ 0 }; int month{ 0 }; };
		auto cal = std::make_shared<CalState>();
		// 日历控件只建一次：切月只刷新文字 / 配色，不重建按钮，鼠标停悬状态才不会丢
		struct CalUi {
			Ling::Button* yearBtn{ nullptr };
			Ling::Button* monthBtn{ nullptr };
			std::vector<Ling::Button*> cells;       // 42 个日期格
			std::vector<Ling::Node*> dayRows;       // 6 行日期容器
			Ling::Node* monthHost{ nullptr };       // 月份网格
			Ling::Node* yearHost{ nullptr };        // 年份网格
			std::vector<Ling::Button*> monthCells;
			std::vector<Ling::Button*> yearCells;
			Ling::Button* nextNav{ nullptr };       // 右箭头（到未来时禁用）
			int view{ 0 };                          // 0 天 / 1 月 / 2 年
			int yearPage{ 0 };                      // 年份网格起始年
			int pickedYear{ -1 };                   // 本会话里用户点过的年份（-1 = 还没点）
			int pickedMonth{ -1 };                  // 本会话里用户点过的月份（-1 = 还没点）
		};
		auto ui = std::make_shared<CalUi>();
		auto updateCal = std::make_shared<std::function<void()>>();
		auto buildCal = std::make_shared<std::function<void()>>();
		auto btnRowRef = std::make_shared<Ling::Node*>(nullptr);   // 日历展开时先让位

		// 展开某个字段的日历：初始月份取该字段已有日期，否则今天
		auto openCal = [this, cal, calHost, ui, buildCal, updateCal, btnRowRef, dateRow, startBox, endBox, calOpen](int field) {
			cal->field = field;
			int y = 0, m = 0, d = 0;
			const std::wstring t = (field == 0) ? startBox->getText() : endBox->getText();
			if (swscanf_s(t.c_str(), L"%d-%d-%d", &y, &m, &d) == 3) {
				cal->year = y;
				cal->month = m - 1;
			}
			else {
				time_t tt = (time_t)(nowMs() / 1000);
				struct tm tn{};
				localtime_s(&tn, &tt);
				cal->year = tn.tm_year + 1900;
				cal->month = tn.tm_mon;
			}
			ui->view = 0;   // 每次展开都从日期视图开始
			ui->pickedYear = -1;   // 重新展开：年份回到「默认态」，只有当前年带底色
			ui->pickedMonth = -1;  // 月份同理
			if (!ui->yearBtn) (*buildCal)();
			else (*updateCal)();
			calHost->show();
			// 日历展开期间：日期框整行与取消/确认都收起，整块空间留给日历
			*calOpen = true;
			dateRow->hide();
			if (*btnRowRef) (*btnRowRef)->hide();
			if (this->win) this->win->layout();
		};
		startBox->onClick.add([openCal](Ling::Button*) { openCal(0); });
		endBox->onClick.add([openCal](Ling::Button*) { openCal(1); });

		// 按当前年月刷新标题 / 日期格 / 月份格 / 年份格（只改文字与配色，不动控件）
		*updateCal = [this, cal, ui, startBox, endBox, tFg, tHover, fgFaint,
			todayBg, todayFg, selBg, selFg]() {
			const int y = cal->year, m = cal->month;

			wchar_t yb[32]{};
			swprintf_s(yb, Lang::get(L"setting.calYearFmt").c_str(), y);
			ui->yearBtn->setText(yb);
			wchar_t mb[32]{};
			swprintf_s(mb, Lang::get(L"setting.calMonthFmt").c_str(), m + 1);
			ui->monthBtn->setText(mb);

			// 标题的选中态：打开哪个网格，对应标题就保持底色
			ui->yearBtn->setBg(ui->view == 2 ? tHover : 0);
			ui->monthBtn->setBg(ui->view == 1 ? tHover : 0);

			// 视图显隐：天 / 月 / 年
			auto show = [](Ling::Node* n, bool on) {
				if (!n) return;
				if (on) n->show(); else n->hide();
			};
			for (auto* row : ui->dayRows) show(row, ui->view == 0);
			show(ui->monthHost, ui->view == 1);
			show(ui->yearHost, ui->view == 2);

			time_t nowT = (time_t)(nowMs() / 1000);
			struct tm tn{};
			localtime_s(&tn, &nowT);

			// 月份网格：默认态只有当前月带底色；用户点过的月份才显示选中态
			for (int i = 0; i < (int)ui->monthCells.size(); ++i) {
				auto* b = ui->monthCells[i];
				wchar_t tb[16]{};
				swprintf_s(tb, Lang::get(L"setting.calMonthFmt").c_str(), i + 1);
				b->setText(tb);
				const bool isThisMonth = (y == tn.tm_year + 1900 && i == tn.tm_mon);
				const bool future = (y > tn.tm_year + 1900)
					|| (y == tn.tm_year + 1900 && i > tn.tm_mon);
				const bool sel = (ui->pickedMonth == i);
				b->setBg(sel ? selBg : (isThisMonth ? todayBg : 0));
				b->setHoverBg(sel ? selBg
					: (isThisMonth ? todayBg : (future ? 0 : tHover)));
				if (sel) b->setColor(selFg);
				else if (isThisMonth) b->setColor(todayFg);
				else b->setColor(future ? fgFaint : tFg);
				b->setHoverColor(sel ? selFg
					: (isThisMonth ? todayFg : (future ? fgFaint : tFg)));
			}

			// 年份网格：默认态只有当前年带底色；用户点过的年份才显示选中态
			for (int i = 0; i < (int)ui->yearCells.size(); ++i) {
				const int yy = ui->yearPage + i;
				auto* b = ui->yearCells[i];
				wchar_t tb[16]{};
				swprintf_s(tb, L"%d", yy);
				b->setText(tb);
				const bool future = (yy > tn.tm_year + 1900);
				const bool isThisYear = (yy == tn.tm_year + 1900);
				const bool sel = (ui->pickedYear == yy);
				b->setBg(sel ? selBg : (isThisYear ? todayBg : 0));
				b->setHoverBg(sel ? selBg : (isThisYear ? todayBg : (future ? 0 : tHover)));
				if (sel) b->setColor(selFg);
				else if (isThisYear) b->setColor(todayFg);
				else b->setColor(future ? fgFaint : tFg);
				b->setHoverColor(sel ? selFg
					: (isThisYear ? todayFg : (future ? fgFaint : tFg)));
			}

			// 不能往后翻到未来：右箭头置灰且点击无效
			bool canNext = false;
			const int curY = tn.tm_year + 1900, curM = tn.tm_mon;
			if (ui->view == 2) canNext = (ui->yearPage + 12) <= curY;
			else if (ui->view == 1) canNext = (y < curY);
			else canNext = (y < curY) || (y == curY && m < curM);
			if (ui->nextNav) {
				ui->nextNav->setColor(canNext ? tFg : fgFaint);
				ui->nextNav->setHoverColor(canNext ? tFg : fgFaint);
				ui->nextNav->setHoverBg(canNext ? tHover : 0);
				if (!canNext) ui->nextNav->setBg(0);   // 正停悬时立刻撤掉底色
			}

			if (ui->view != 0) {
				if (this->win) this->win->layout();
				return;
			}

			struct tm base{};
			base.tm_year = y - 1900;
			base.tm_mon = m;
			base.tm_mday = 1;
			base.tm_hour = 12;
			mktime(&base);                          // 归一化，同时拿到 1 号是周几
			const int lead = (base.tm_wday + 6) % 7;

			int selY = 0, selM = 0, selD = 0;
			const std::wstring cur = (cal->field == 0) ? startBox->getText() : endBox->getText();
			swscanf_s(cur.c_str(), L"%d-%d-%d", &selY, &selM, &selD);

			for (int i = 0; i < (int)ui->cells.size(); ++i) {
				struct tm td = base;
				td.tm_mday = i - lead + 1;
				mktime(&td);
				const int day = td.tm_mday;
				const bool inMonth = (td.tm_mon == m);
				const bool isToday = (td.tm_year == tn.tm_year && td.tm_mon == tn.tm_mon && day == tn.tm_mday);
				const bool selected = (selD > 0 && td.tm_year == selY - 1900
					&& td.tm_mon == selM - 1 && day == selD);

				auto* cell = ui->cells[i];
				cell->setText(std::to_wstring(day));
				// 今天之后的日子还没到：弱化且无停悬
				const bool future = (td.tm_year > tn.tm_year)
					|| (td.tm_year == tn.tm_year && td.tm_mon > tn.tm_mon)
					|| (td.tm_year == tn.tm_year && td.tm_mon == tn.tm_mon && day > tn.tm_mday);
				// 选中：主色底 + 主色前景；今天：次级底 + 主文字；其余透明 + 停悬底色
				const uint32_t cellBg = selected ? selBg : (isToday ? todayBg : 0);
				const uint32_t cellFg = selected ? selFg
					: (isToday ? todayFg : ((inMonth && !future) ? tFg : fgFaint));
				cell->setBg(cellBg);
				cell->setHoverBg(selected || isToday || future ? cellBg : tHover);
				cell->setColor(cellFg);
				cell->setHoverColor(cellFg);
			}
			if (this->win) this->win->layout();
		};

		*buildCal = [this, cal, ui, updateCal, calHost, calOpen, btnRowRef, dateRow,
			startBox, endBox, tFg, tHover]() {
			// 头部：‹ 年月 ›
			auto* head = calHost->makeChild<Ling::Node>();
			head->setFlexDirection(Ling::FlexDirection::Row);
			head->setAlignItems(Ling::Align::Center);
			head->setWidthPercent(100.f);
			head->setHeight(32.f);
			head->setMarginBottom(2.f);

			// 切月 / 切年 / 翻年份页（看当前视图）；到未来就不给再往后翻
			auto shift = [this, cal, ui, updateCal](int dir) {
				if (dir > 0) {
					time_t nowT = (time_t)(nowMs() / 1000);
					struct tm tn{};
					localtime_s(&tn, &nowT);
					const int curY = tn.tm_year + 1900, curM = tn.tm_mon;
					bool canNext = false;
					if (ui->view == 2) canNext = (ui->yearPage + 12) <= curY;
					else if (ui->view == 1) canNext = (cal->year < curY);
					else canNext = (cal->year < curY) || (cal->year == curY && cal->month < curM);
					if (!canNext) return;
				}
				if (ui->view == 2) {
					ui->yearPage += 12 * dir;
				}
				else if (ui->view == 1) {
					cal->year += dir;
				}
				else {
					int mm = cal->month + dir;
					while (mm < 0) { mm += 12; cal->year -= 1; }
					while (mm > 11) { mm -= 12; cal->year += 1; }
					cal->month = mm;
				}
				(*updateCal)();
			};
			auto mkNav = [&](float angle, std::function<void()> fn) {
				auto* b = head->makeChild<ChevronNavBtn>();
				b->setAngle(angle);
				b->setText(Icon::Dropdown);
				b->setFontFamily(Icon::Family);
				b->setFontSize(18.f);
				b->setSize(36.f, 36.f);
				b->setPadding(0.f, 0.f, 0.f, 0.f);
				b->setBorderRadius(ToolbarTheme::hoverRadius);
				b->setBorder(0.f, 0);
				b->setBg(0);
				b->setHoverBg(tHover);
				b->setColor(tFg);
				b->setHoverColor(tFg);
				b->setJustifyContent(Ling::Justify::Center);
				b->setAlignItems(Ling::Align::Center);
				b->onClick.add([fn](Ling::Button*) { fn(); });
				return b;
			};
			// 箭头中心与日期网格首列 / 末列的中心对齐（网格 7 列 38 宽、SpaceEvenly 均分，间距约 4）
			auto* prevNav = mkNav(90.f, [shift]() { shift(-1); });
			prevNav->setMarginLeft(5.f);
			prevNav->setMarginRight(4.f);

			auto* titleWrap = head->makeChild<Ling::Node>();
			titleWrap->setFlexGrow(1.f);
			titleWrap->setFlexShrink(1.f);
			titleWrap->setFlexDirection(Ling::FlexDirection::Row);
			titleWrap->setJustifyContent(Ling::Justify::Center);
			titleWrap->setAlignItems(Ling::Align::Center);

			// 年份 / 月份都可点：点开对应的选择网格
			auto mkTitle = [&]() {
				auto* b = titleWrap->makeChild<Ling::Button>();
				b->setFontSize(SettingTheme::fontBase);
				b->setFontWeight(600);
				b->setColor(tFg);
				b->setHoverColor(tFg);
				b->setBg(0);
				b->setHoverBg(tHover);
				b->setHeight(28.f);
				b->setPadding(6.f, 0.f, 6.f, 0.f);
				b->setBorder(0.f, 0);
				b->setBorderRadius(ToolbarTheme::hoverRadius);
				b->setJustifyContent(Ling::Justify::Center);
				b->setAlignItems(Ling::Align::Center);
				return b;
			};
			auto* yearBtn = mkTitle();
			yearBtn->setMarginRight(2.f);
			yearBtn->onClick.add([this, cal, ui, updateCal](Ling::Button*) {
				ui->view = 2;
				ui->yearPage = cal->year - 9;   // 最近 12 年：9 年过去 + 当年 + 2 年未来
				(*updateCal)();
			});
			ui->yearBtn = yearBtn;

			auto* monthBtn = mkTitle();
			monthBtn->onClick.add([this, ui, updateCal](Ling::Button*) {
				ui->view = 1;
				(*updateCal)();
			});
			ui->monthBtn = monthBtn;

			auto* nextNav = mkNav(-90.f, [shift]() { shift(1); });
			nextNav->setMarginRight(5.f);
			ui->nextNav = nextNav;

			// 日期网格：周一起算；固定 6 行（多出来的补下个月），
			// 行数固定行距才稳定，横竖间距才能一致
			for (int r = 0; r < 6; ++r) {
				auto* row = calHost->makeChild<Ling::Node>();
				row->setFlexDirection(Ling::FlexDirection::Row);
				row->setWidthPercent(100.f);
				row->setFlexGrow(1.f);
				row->setFlexShrink(1.f);
				row->setJustifyContent(Ling::Justify::SpaceEvenly);
				row->setAlignItems(Ling::Align::Center);
				ui->dayRows.push_back(row);
				for (int c = 0; c < 7; ++c) {
					auto* cell = row->makeChild<Ling::Button>();
					cell->setSize(38.f, 38.f);                  // 方形底块（列间距约 4）
					cell->setFlexShrink(0.f);
					cell->setFontSize(SettingTheme::fontBase);
					cell->setPadding(0.f, 0.f, 0.f, 0.f);
					cell->setBorderRadius(8.f);                 // 与时间按钮 / 停悬块同一档圆角
					cell->setBorder(0.f, 0);
					cell->setJustifyContent(Ling::Justify::Center);
					cell->setAlignItems(Ling::Align::Center);
					const int idx = r * 7 + c;
					cell->onClick.add([this, cal, calHost, btnRowRef, dateRow, startBox, endBox, calOpen, tFg, idx](Ling::Button*) {
						struct tm b{};
						b.tm_year = cal->year - 1900;
						b.tm_mon = cal->month;
						b.tm_mday = 1;
						b.tm_hour = 12;
						mktime(&b);
						const int lead = (b.tm_wday + 6) % 7;
						struct tm td = b;
						td.tm_mday = idx - lead + 1;
						std::wstring picked = dateText((long long)mktime(&td) * 1000);
						// 未来日期不能选，回落到今天
						const std::wstring todayStr = dateText(nowMs());
						if (picked > todayStr) picked = todayStr;
						if (cal->field == 0) startBox->setText(picked);
						else endBox->setText(picked);
						// 日期用正常色，不再是占位灰
						startBox->setColor(tFg);
						startBox->setHoverColor(tFg);
						endBox->setColor(tFg);
						endBox->setHoverColor(tFg);
						// 收起日历，恢复日期框与取消/确认
						*calOpen = false;
						calHost->hide();
						dateRow->show();
						if (*btnRowRef) (*btnRowRef)->show();
						if (this->win) this->win->layout();
					});
					ui->cells.push_back(cell);
				}
			}

			// 月份 / 年份网格：3 列 × 4 行，和日期区占据同一块空间
			auto buildMini = [&](std::vector<Ling::Button*>& out, Ling::Node*& host,
				std::function<void(int)> onPick) {
				host = calHost->makeChild<Ling::Node>();
				host->setFlexDirection(Ling::FlexDirection::Column);
				host->setWidthPercent(100.f);
				host->setFlexGrow(1.f);
				host->setFlexShrink(1.f);
				host->hide();
				for (int r = 0; r < 4; ++r) {
					auto* row = host->makeChild<Ling::Node>();
					row->setFlexDirection(Ling::FlexDirection::Row);
					row->setWidthPercent(100.f);
					row->setFlexGrow(1.f);
					row->setFlexShrink(1.f);
					row->setAlignItems(Ling::Align::Center);
					for (int c = 0; c < 3; ++c) {
						auto* b = row->makeChild<Ling::Button>();
						b->setFlexGrow(1.f);
						b->setMargin(4.f);
						b->setHeight(38.f);          // 与日期格同高的底块
						b->setPadding(0.f, 0.f, 0.f, 0.f);
						b->setBorder(0.f, 0);
						b->setBorderRadius(8.f);
						b->setFontSize(SettingTheme::fontBase);
						b->setJustifyContent(Ling::Justify::Center);
						b->setAlignItems(Ling::Align::Center);
						const int idx = r * 3 + c;
						b->onClick.add([onPick, idx](Ling::Button*) { onPick(idx); });
						out.push_back(b);
					}
				}
			};
			buildMini(ui->monthCells, ui->monthHost, [this, cal, ui, updateCal](int idx) {
				// 未来的月份不可选
				time_t nowT = (time_t)(nowMs() / 1000);
				struct tm tn{};
				localtime_s(&tn, &nowT);
				if (cal->year > tn.tm_year + 1900
					|| (cal->year == tn.tm_year + 1900 && idx > tn.tm_mon)) {
					ui->view = 0;   // 点未来月份只收起月份选择，不改月份
					(*updateCal)();
					return;
				}
				cal->month = idx;
				ui->pickedMonth = idx;   // 记住用户点过的月份 → 显示选中态
				ui->view = 0;
				(*updateCal)();
			});
			buildMini(ui->yearCells, ui->yearHost, [this, cal, ui, updateCal](int idx) {
				const int yy = ui->yearPage + idx;
				// 未来的年份不可选
				time_t nowT = (time_t)(nowMs() / 1000);
				struct tm tn{};
				localtime_s(&tn, &nowT);
				if (yy > tn.tm_year + 1900) {
					ui->view = 0;   // 点未来年份只收起年份选择，不改年份
					(*updateCal)();
					return;
				}
				cal->year = yy;
				ui->pickedYear = yy;   // 记住用户点过的年份 → 显示选中态
				ui->pickedMonth = -1;  // 换了年份，之前的月份选中态作废
				ui->view = 0;   // 只选年份，直接回日期视图
				(*updateCal)();
			});
			(*updateCal)();
		};

		// 取消 / 确认固定在气泡右下角，不随内容流排版（距底 12）
		auto* btnRow = content->makeChild<Ling::Node>();
		btnRow->setPositionType(Ling::Position::Absolute);
		btnRow->setPosition(Ling::Edge::Right, 0.f);
		btnRow->setPosition(Ling::Edge::Bottom, padBottom);
		btnRow->setFlexDirection(Ling::FlexDirection::Row);
		btnRow->setAlignItems(Ling::Align::Center);
		sec->rangeBtns = btnRow;
		*btnRowRef = btnRow;

		auto weakR = getWeakThis();
		// 取消：在「选择时间」里只退出日期区间选择，回到列表；气泡保持展开。
		// 不在区间模式时（理论上按钮不可见）才收起整个气泡。
		auto deferCancel = [this, weakR, applyMode, calOpen]() {
			auto run = [this, weakR, applyMode, calOpen]() {
				if (!weakR.lock()) return;
				if (this->panelMode == 1) {
					this->panelMode = 0;
					*calOpen = false;
					applyMode();
					return;
				}
				this->closeFilterPanel();
			};
			if (auto* app = Ling::App::get()) app->dq.TryEnqueue(run);
			else run();
		};
		auto deferApplyRange = [this, weakR]() {
			auto run = [this, weakR]() {
				if (!weakR.lock()) return;
				this->closeFilterPanel();
				this->refreshGrid();
			};
			if (auto* app = Ling::App::get()) app->dq.TryEnqueue(run);
			else run();
		};
		SettingUi::btnOutline(btnRow, Lang::get(L"setting.cancel"), deferCancel, 72.f)
			->setMarginRight(8.f);
		SettingUi::btnPrimary(btnRow, Lang::get(L"setting.confirm"),
			[this, startBox, endBox, deferApplyRange]() {
				long long s = 0, e = 0;
				if (!parseDateRange(startBox->getText(), endBox->getText(), s, e)) return;
				filter.timeMode = 3;
				filter.rangeStart = s;
				filter.rangeEnd = e;
				shownCount = kBatch;
				deferApplyRange();
			}, 72.f);
	}

	// 默认：搜索框 + 自定义标签
	auto* listSec = content->makeChild<Ling::Node>();
	listSec->setFlexDirection(Ling::FlexDirection::Column);
	listSec->setWidthPercent(100.f);
	listSec->setFlexGrow(1.f);
	listSec->setFlexShrink(1.f);
	sec->list = listSec;

	// 搜索框（文件名 / 标签）：暗色与分隔线同色，浅色用更浅一档的输入面
	const bool dark = SettingTheme::isDark();
	const uint32_t searchBg = dark ? SettingTheme::border : 0xF5F4F4FF;
	auto* search = listSec->makeChild<Ling::TextBox>();
	search->setHeight(30.f);
	search->setWidthPercent(100.f);
	search->setBorderRadius(ToolbarTheme::hoverRadius);
	search->setBorder(0.f, 0);
	search->setBg(searchBg);
	search->setColor(tFg);
	search->setFontSize(SettingTheme::fontSm);
	search->setPadding(8.f, 0.f, 8.f, 0.f);
	search->setVerticalCenter(true);
	search->setPlaceholder(Lang::get(L"setting.filterSearchHint"));
	search->setPlaceholderColor(SettingTheme::placeholder);
	search->setMarginBottom(10.f);
	search->setText(filter.keyword);
	search->onTextChanged.add([this](Ling::TextBox*, const std::wstring& t) {
		if (filter.keyword == t) return;
		filter.keyword = t;
		shownCount = kBatch;
		this->refreshGrid();
	});

	// 自定义标签：按分类归属列出（「全部」包含「其他」与剪切板的标签，
	// 这样选中一个只存在于剪切板/其他里的标签时，才能把它们筛到「全部」里）
	std::vector<std::wstring> tags;
	auto addTag = [&tags](const std::wstring& t) {
		if (t.empty()) return;
		for (auto& x : tags) if (_wcsicmp(x.c_str(), t.c_str()) == 0) return;
		tags.push_back(t);
		};
	for (auto& it : Setting::get()->getHistoryItemsForUi()) {
		if (!baseTabMatch(it)) continue;
		for (auto& t : Setting::get()->getHistoryTags(it.filePath)) addTag(t);
	}
	if (sourceTab == 0 || sourceTab == 4) {
		for (auto& c : Setting::get()->getClipboardItems())
			for (auto& t : c.tags) addTag(t);
	}
	std::sort(tags.begin(), tags.end());
	// 选中的标签已不存在（文件被删/标签被清空）→ 退回不筛选
	if (!filter.tag.empty()) {
		bool exists = false;
		for (auto& t : tags) if (_wcsicmp(t.c_str(), filter.tag.c_str()) == 0) { exists = true; break; }
		if (!exists) filter.tag.clear();
	}
	if (tags.empty()) {
		auto* none = listSec->makeChild<Ling::Label>();
		none->setText(Lang::get(L"setting.filterNoTags"));
		none->setFontSize(SettingTheme::fontSm);
		none->setColor(fgFaint);
		none->setMarginLeft(8.f);   // 与搜索框内的文字左对齐
		applyMode();
		return;
	}

	// 有标签时直接铺标签列表（无分组标题）
	auto* tagScroll = listSec->makeChild<Ling::ScrollerBox>();
	tagScroll->setWidthPercent(100.f);
	tagScroll->setFlexGrow(1.f);
	tagScroll->setFlexShrink(1.f);
	tagScroll->setScrollBarVisible(false);
	tagScroll->content->setFlexDirection(Ling::FlexDirection::Row);
	tagScroll->content->setFlexWrap(Ling::Wrap::Wrap);
	tagScroll->content->setAlignItems(Ling::Align::FlexStart);
	tagScroll->content->setWidthPercent(100.f);

	auto tagBtns = std::make_shared<std::vector<std::pair<Ling::Button*, std::wstring>>>();
	// 标签底色：暗色与分隔线同色，浅色用 #F5F4F4；停悬统一加深一档
	const uint32_t tagIdleBg = dark ? SettingTheme::border : 0xF5F4F4FF;
	const uint32_t tagHoverBg = dark ? 0x3A3A3AFF : 0xE5E5E5FF;
	// 标签文字：常态用次级色，停悬才变实色；选中用主题绿
	const uint32_t tagIdleFg = SettingTheme::textSecondary;
	auto syncTagBtns = [this, tagBtns, tFg, tActive, tagIdleFg, tagIdleBg, tagHoverBg]() {
			for (auto& [b, tag] : *tagBtns) {
				if (!b) continue;
				const bool active = (filter.tag == tag);
				b->setColor(active ? tActive : tagIdleFg);
				b->setHoverColor(active ? tActive : tFg);
				b->setBg(tagIdleBg);
				b->setHoverBg(tagHoverBg);
			}
		};

	for (auto& tag : tags) {
		auto* b = tagScroll->makeChild<Ling::Button>();
		b->setText(tag);
		b->setFontSize(SettingTheme::fontXs);
		b->setHeight(22.f);
		b->setPadding(6.f, 0.f, 6.f, 0.f);
		b->setBorderRadius(ToolbarTheme::hoverRadius);
		b->setBorder(0.f, 0);
		b->setBg(tagIdleBg);
		b->setHoverBg(tagHoverBg);
		b->setColor(tagIdleFg);
		b->setHoverColor(tFg);
		b->setJustifyContent(Ling::Justify::Center);
		b->setAlignItems(Ling::Align::Center);
		b->setMargin(0.f, 0.f, 6.f, 6.f);
		b->setFlexShrink(0.f);
		const std::wstring t = tag;
		b->onClick.add([this, t, syncTagBtns](Ling::Button*) {
			filter.tag = (filter.tag == t) ? L"" : t;
			shownCount = kBatch;
			syncTagBtns();
			this->refreshGrid();
		});
		tagBtns->push_back({ b, t });
	}
	syncTagBtns();
	applyMode();
}

void WinSettingHistory::closeTransientUi(POINT pos)
{
	const bool esc = (pos.x == INT_MAX);
	if (modal) {
		if (esc || (modalCard && !modalCard->isPosIn(pos))) {
			if (!esc) dismissingClick = true;
			closeModal();
		}
		return;
	}
	if (!filterPanel) return;
	if (esc) { closeFilterPanel(); return; }
	// 点面板内部不算点外；点筛选按钮本身交给按钮自己的开关逻辑
	const bool inPanel = filterPanel->isPosIn(pos);
	const bool onBtn = filterBtn && filterBtn->isPosIn(pos);
	if (!inPanel && !onBtn) {
		dismissingClick = true;
		closeFilterPanel();
	}
}

void WinSettingHistory::closeModal()
{
	if (modalKeyBound) {
		win->onKeyDown.remove(modalKeyTok);
		modalKeyBound = false;
	}
	if (!modal) return;
	if (modal->parent) modal->parent->removeChild(modal);
	modal = nullptr;
	modalCard = nullptr;
}

Ling::Node* WinSettingHistory::makeModalCard(float width, const std::wstring& title)
{
	closeModal();
	auto* layer = makeChild<Ling::Node>();
	layer->setPositionType(Ling::Position::Absolute);
	layer->setPosition(Ling::Edge::Left, 0.f);
	layer->setPosition(Ling::Edge::Top, 0.f);
	layer->setPosition(Ling::Edge::Right, 0.f);
	layer->setPosition(Ling::Edge::Bottom, 0.f);
	layer->setBg(0x00000059);
	layer->setJustifyContent(Ling::Justify::Center);
	layer->setAlignItems(Ling::Align::Center);
	modal = layer;

	auto* card = layer->makeChild<Ling::Node>();
	card->setWidth(width);
	card->setFlexDirection(Ling::FlexDirection::Column);
	card->setBg(SettingTheme::card);
	card->setBorder(1.f, SettingTheme::border);
	card->setBorderRadius(SettingTheme::radiusLg);
	card->setPadding(18.f, 16.f, 18.f, 16.f);
	modalCard = card;

	auto* t = card->makeChild<Ling::Label>();
	t->setText(title);
	t->setFontSize(SettingTheme::fontBase);
	t->setColor(SettingTheme::textPrimary);
	t->setMarginBottom(10.f);
	return card;
}

// 编辑某条历史的自定义标签：标签以 chip 形式显示在输入框内；
// 输入文字后按空格 → 变成 chip；输入框为空时按退格/Delete → 删掉最后一个 chip
void WinSettingHistory::showTagDialog(const Setting::HistoryItem& it)
{
	const std::wstring path = it.filePath;
	showTagDialogFor(path, Setting::get()->getHistoryTags(path),
		[path](const std::vector<std::wstring>& tags) {
			Setting::get()->setHistoryTags(path, tags);
		});
}

void WinSettingHistory::showTagDialogFor(const std::wstring& key, std::vector<std::wstring> initial,
	std::function<void(const std::vector<std::wstring>&)> save)
{
	(void)key;
	if (tip) tip->hide();     // 弹窗盖住卡片，先收掉卡片上的停悬提示
	auto tags = std::make_shared<std::vector<std::wstring>>(std::move(initial));

	auto* card = makeModalCard(320.f, Lang::get(L"setting.filterTags"));

	// 输入框外壳：单行流式——标签 chip 与输入框同一行，按宽度自动换行
	auto* field = card->makeChild<Ling::Node>();
	field->setFlexDirection(Ling::FlexDirection::Row);
	field->setFlexWrap(Ling::Wrap::Wrap);
	field->setAlignItems(Ling::Align::Center);
	field->setWidthPercent(100.f);
	field->setBg(SettingTheme::card);
	field->setBorder(1.f, SettingTheme::input);
	field->setBorderRadius(SettingTheme::radiusInner);
	field->setPadding(6.f, 5.f, 6.f, 5.f);
	field->setMarginBottom(8.f);

	// 光标位置：= 已确认标签的数量（0..size），左侧标签即 Delete 的目标
	auto sel = std::make_shared<int>((int)tags->size());
	auto boxPtr = std::make_shared<Ling::TextBox*>(nullptr);
	auto rebuild = std::make_shared<std::function<void()>>();
	*rebuild = [field, tags, sel, boxPtr]() {
		field->removeAllChildren();
		for (size_t i = 0; i < tags->size(); ++i) {
			auto* chip = field->makeChild<Ling::Label>();
			chip->setText((*tags)[i]);
			chip->setFontSize(SettingTheme::fontXs);
			chip->setColor(SettingTheme::textPrimary);
			// 光标左侧那个标签高亮：Delete 删的就是它
			chip->setBg((*sel == (int)i + 1) ? SettingTheme::accent : SettingTheme::secondary);
			chip->setPadding(5.f, 1.f, 5.f, 1.f);
			chip->setBorderRadius(4.f);
			chip->setMargin(0.f, 0.f, 4.f, 4.f);
			chip->setJustifyContent(Ling::Justify::Center);
			chip->setAlignItems(Ling::Align::Center);
		}
		auto* b = field->makeChild<Ling::TextBox>();
		b->setWidth(96.f);
		b->setHeight(22.f);
		b->setBg(0);
		b->setBorder(0.f, 0);
		b->setColor(SettingTheme::textPrimary);
		b->setFontSize(SettingTheme::fontSm);
		b->setPadding(2.f, 0.f, 2.f, 0.f);
		b->setVerticalCenter(true);
		*boxPtr = b;
		b->focus();
	};
	(*rebuild)();

	// 延后一拍再重建输入框：避免在按键事件派发中途销毁正在处理按键的 TextBox
	auto weakPage0 = getWeakThis();
	auto scheduleRebuild = [rebuild, weakPage0]() {
		auto run = [rebuild, weakPage0]() { if (weakPage0.lock()) (*rebuild)(); };
		if (auto* app = Ling::App::get()) app->dq.TryEnqueue(run);
		else run();
	};
	{	// 打开后聚焦输入框
		auto focusInit = [boxPtr]() { if (boxPtr && *boxPtr) (*boxPtr)->focus(); };
		if (auto* app = Ling::App::get()) app->dq.TryEnqueue(focusInit);
		else focusInit();
	}

	// 空格 → 生成标签；←/→ 在标签间移动光标；退格/Delete（输入框为空）删光标前一个标签
	auto weakPage = getWeakThis();
	Ling::Node* owner = modal;
	modalKeyTok = win->onKeyDown.add([this, weakPage, owner, tags, sel, boxPtr, scheduleRebuild](UINT key) {
		if (!weakPage.lock()) return;
		if (this->modal != owner) return;   // 弹窗已被 Esc 关掉，控件已销毁
		auto* box = *boxPtr;
		if (!box) return;
		const bool textEmpty = box->getText().empty();
		if (key == VK_SPACE) {
			std::wstring t = box->getText();
			while (!t.empty() && iswspace(t.back())) t.pop_back();
			if (!t.empty()) {
				bool dup = false;
				for (auto& e : *tags) if (_wcsicmp(e.c_str(), t.c_str()) == 0) { dup = true; break; }
				if (!dup) {
					const int at = (std::max)(0, (std::min)(*sel, (int)tags->size()));
					tags->insert(tags->begin() + at, t);
					*sel = at + 1;
				}
			}
			scheduleRebuild();
		}
		else if (key == VK_LEFT) {
			if (!textEmpty || *sel <= 0) return;
			--(*sel);
			scheduleRebuild();
		}
		else if (key == VK_RIGHT) {
			if (!textEmpty || *sel >= (int)tags->size()) return;
			++(*sel);
			scheduleRebuild();
		}
		else if (key == VK_BACK || key == VK_DELETE) {
			if (!textEmpty || *sel <= 0) return;   // 还有文字：正常删字符
			tags->erase(tags->begin() + (*sel - 1));
			--(*sel);
			scheduleRebuild();
		}
	});
	modalKeyBound = true;

	auto* row = card->makeChild<Ling::Node>();
	row->setFlexDirection(Ling::FlexDirection::Row);
	row->setWidthPercent(100.f);
	row->setJustifyContent(Ling::Justify::End);
	row->setAlignItems(Ling::Align::Center);
	row->setMarginTop(10.f);
	auto weak = getWeakThis();
	auto deferClose = [this, weak]() {
		auto run = [this, weak]() { if (weak.lock()) this->closeModal(); };
		if (auto* app = Ling::App::get()) app->dq.TryEnqueue(run);
		else run();
	};
	auto deferCloseAndRefresh = [this, weak]() {
		auto run = [this, weak]() {
			if (!weak.lock()) return;
			this->forceGridRebuild = true;   // 标签变了：卡片必须整体重建才看得到
			this->closeModal();
			this->refreshGrid();
		};
		if (auto* app = Ling::App::get()) app->dq.TryEnqueue(run);
		else run();
	};
	SettingUi::btnOutline(row, Lang::get(L"setting.cancel"), deferClose, 72.f)
		->setMarginRight(8.f);
	SettingUi::btnPrimary(row, Lang::get(L"setting.confirm"),
		[boxPtr, tags, save = std::move(save), deferCloseAndRefresh]() {
			// 还没按空格的那段文字也算一个标签
			std::wstring pending = boxPtr && *boxPtr ? (*boxPtr)->getText() : L"";
			while (!pending.empty() && iswspace(pending.back())) pending.pop_back();
			while (!pending.empty() && iswspace(pending.front())) pending.erase(pending.begin());
			std::vector<std::wstring> out = *tags;
			if (!pending.empty()) {
				bool dup = false;
				for (auto& e : out) if (_wcsicmp(e.c_str(), pending.c_str()) == 0) { dup = true; break; }
				if (!dup) out.push_back(pending);
			}
			if (save) save(out);
			deferCloseAndRefresh();
		}, 72.f);
}

std::vector<Setting::HistoryItem> WinSettingHistory::collectItems(bool forceRescan)
{
	auto list = Setting::get()->getHistoryItemsForUi(forceRescan);
	// 建「我们自己产出的文件 → 封面」表：剪贴板里复制本软件录的屏时，卡片直接用媒体历史的封面，
	// 不再显示文件名。媒体历史条目本身在「录屏」页照旧显示，两边各自独立。
	ourMediaCover_.clear();
	for (const auto& m : list) {
		if (m.source != L"record" || m.filePath.empty()) continue;
		const auto cover = (Setting::get()->getHistoryThumbDir() / (m.id + L".png")).wstring();
		std::error_code ec;
		if (!std::filesystem::exists(cover, ec)) continue;   // 封面被保留策略清掉了：回退到文件名
		std::wstring key = m.filePath;
		for (auto& ch : key) ch = (wchar_t)towlower(ch);
		ourMediaCover_[std::move(key)] = cover;
	}
	// 「全部」和「剪切板」页要把剪切板条目并进来（「全部」里是否显示由 matchSourceTab 决定）
	if (sourceTab == 0 || sourceTab == 4) {
		for (auto& c : Setting::get()->getClipboardItems()) {
			Setting::HistoryItem it;
			it.id = c.id;
			it.source = L"clipboard";
			it.filePath = (c.type == L"image") ? c.filePath : L"";
			it.width = c.width;
			it.height = c.height;
			// 剪贴板里的「图片文件」老条目没存宽高：这里补读一次文件头，渲染路径就不必改
			if (it.width <= 0 && c.files.size() == 1 && clipIsImageExt(c.files.front())) {
				const auto wh = Util::imageSize(c.files.front());
				if (wh.first > 0) { it.width = wh.first; it.height = wh.second; }
			}
			it.createdAt = c.createdAt;
			it.text = c.text;
			it.files = c.files;
			list.push_back(std::move(it));
		}
		std::sort(list.begin(), list.end(), [](const Setting::HistoryItem& a, const Setting::HistoryItem& b) {
			return a.createdAt > b.createdAt;
		});
	}
	return list;
}

void WinSettingHistory::refreshGrid()
{
	Setting::get()->pruneExpiredHistory();
	auto all = collectItems(true);

	// 按当前筛选过滤（导航 + 时间 + 标签 + 关键词）
	std::vector<Setting::HistoryItem> items;
	for (const auto& it : all) {
		if (matchFilters(it)) items.push_back(it);
	}
	const int total = (int)items.size();

	// 计数（右下「历史条目：%1」，随导航/筛选变化）
	if (countLabel) {
		std::wstring c = Lang::get(L"setting.currentCount");
		c = replaceOnce(c, L"%1", std::to_wstring(total));
		countLabel->setText(c);
	}

	if (!gridHost) return;

	const bool empty = items.empty();
	// 空态与网格互斥显示：两者都带 flexGrow(1)，必须一并收起另一个，否则会平分中部区域
	// 导致「暂无截图历史」只在中部上半段居中（视觉上偏上）。
	if (emptyLabel) {
		const wchar_t* emptyKey = sourceTab == 0 ? L"setting.noAnyHistory"
			: (sourceTab == 2 ? L"setting.noRecordHistory"
				: (sourceTab == 3 ? L"setting.noOtherHistory"
					: (sourceTab == 4 ? L"setting.noClipboardHistory" : L"setting.noHistory")));
		emptyLabel->setText(Lang::get(emptyKey));
		empty ? emptyLabel->show() : emptyLabel->hide();
	}
	if (gridScroll)  empty ? gridScroll->hide()  : gridScroll->show();
	if (empty) {
		gridHost->removeAllChildren();
		gridHost->hide();
		builtIds.clear();
		cells.clear();
		copyToast = nullptr;
		if (tip) tip->hide();       // 提示挂在将被拆掉的按钮上，一并收掉
		lastRow = nullptr; lastRowCells = 0; lastRowFillers.clear();
		shownCount = kBatch;
		return;
	}
	gridHost->show();

	// 分屏渲染：首屏一批，之后只随滚动增大
	if (shownCount <= 0) shownCount = kBatch;
	if (shownCount > total) shownCount = total;
	const int want = shownCount;

	// 需要整体重建？强制标记（标签改动等）/ 已建格子数超过 want / 前缀条目对不上
	bool rebuild = forceGridRebuild || (int)builtIds.size() > want;
	forceGridRebuild = false;
	if (!rebuild) {
		const size_t n = (std::min)(builtIds.size(), (size_t)want);
		for (size_t i = 0; i < n; ++i) {
			if (builtIds[i] != items[i].id) { rebuild = true; break; }
		}
	}
	if (rebuild) {
		gridHost->removeAllChildren();
		builtIds.clear();
		cells.clear();
		// 提示挂在将被拆掉的格子上，随格子一起销毁，这里一并作废
		copyToast = nullptr;
		if (tip) tip->hide();       // 提示挂在将被拆掉的按钮上，一并收掉
		lastRow = nullptr; lastRowCells = 0; lastRowFillers.clear();
	}

	// 续补前先撤掉末行的占位块
	if (lastRow && !lastRowFillers.empty()) {
		for (auto* f : lastRowFillers) if (f) lastRow->removeChild(f);
		lastRowFillers.clear();
	}

	for (int i = (int)builtIds.size(); i < want; ++i) {
		if (!lastRow || lastRowCells == kCols) {
			if (lastRow) lastRow->setMarginBottom(kGap);   // 上一行不再是末行，补回行距
			lastRow = gridHost->makeChild<Ling::Node>();
			lastRow->setFlexDirection(Ling::FlexDirection::Row);
			lastRow->setWidthPercent(100.f);
			lastRow->setMarginBottom(0.f);                 // 末行先不给间距
			lastRowCells = 0;
		}
		const bool lastCol = (lastRowCells == kCols - 1);
		const auto& it = items[i];

		// 录屏条目（source=record）：视频文件不能直接当缩略图解码，用缓存里的封面小图；
		// 中间那颗按钮也从「贴图」换成「在资源管理器里定位」——贴图对视频没有意义。
		// 剪切板条目：位图/图片文件用图片缩略图，文字铺文字，视频/其它文件显示图标+文件名。
		const bool isMedia = (it.source == L"record");
		const bool clip = (it.source == L"clipboard");
		const bool clipFiles = clip && !it.files.empty();
		const bool clipImageBitmap = clip && !clipFiles && !it.filePath.empty();
		const bool clipText = clip && !clipFiles && !clipImageBitmap;
		// 剪切板里的 SVG 代码：当图片处理（卡片渲染预览、贴图也贴成图）
		const bool clipSvg = clipText && Util::isSvgText(it.text);
		const bool clipImageKind = clip && (clipImageBitmap || (clipFiles && clipAllFiles(it.files, true)));
		// 文字里识别为链接的条目：额外提供「打开」（默认浏览器）
		const bool clipLink = clipText && clipTextKind(it.text) == ClipTextKind::Link;

		// 元信息（顶部行显示）：时间/日期 · 尺寸 · 大小
		wchar_t whenBuf[32]{};
		struct tm t {};
		time_t tt = (time_t)(it.createdAt / 1000);
		localtime_s(&t, &tt);
		struct tm today {};
		time_t nowT = time(nullptr);
		localtime_s(&today, &nowT);
		if (t.tm_year == today.tm_year && t.tm_mon == today.tm_mon && t.tm_mday == today.tm_mday)
			wcsftime(whenBuf, 32, L"%H:%M", &t);
		else if (t.tm_year == today.tm_year)
			wcsftime(whenBuf, 32, L"%m-%d", &t);
		else
			wcsftime(whenBuf, 32, L"%Y-%m-%d", &t);
		std::wstring metaText = std::wstring(whenBuf);
		// 录屏条目补上时长（修复前登记的旧条目没有这个字段，dur=0 时不显示）
		if (isMedia && it.durationSec > 0) {
			const int sec = it.durationSec;
			wchar_t dur[24]{};
			if (sec >= 3600) swprintf_s(dur, L"%d:%02d:%02d", sec / 3600, (sec % 3600) / 60, sec % 60);
			else swprintf_s(dur, L"%02d:%02d", sec / 60, sec % 60);
			metaText += L" · " + std::wstring(dur);
		}
		if (it.width > 0 && it.height > 0)
			metaText += L" · " + std::to_wstring(it.width) + L"×" + std::to_wstring(it.height);
		// 单个视频/其它文件：额外显示文件大小；图片、文件夹、多文件不显示
		if (clip && it.files.size() == 1 && !clipIsImageExt(it.files.front())) {
			std::error_code ec;
			if (!std::filesystem::is_directory(it.files.front(), ec)) {
				const auto bytes = std::filesystem::file_size(it.files.front(), ec);
				if (!ec) metaText += L" · " + formatFileSize(bytes);
			}
		}

		auto* cell = lastRow->makeChild<Ling::Node>();
		cell->setWidthPercent(0.f);
		cell->setFlexGrow(1.f);
		cell->setFlexShrink(1.f);
		if (!lastCol) cell->setMarginRight(kGap);
		cell->setHeight(kThumbH + kPad * 2.f + kCaptionH);
		cell->setFlexDirection(Ling::FlexDirection::Column);
		cell->setBg(SettingTheme::card);
		cell->setBorder(1.f, SettingTheme::border);
		cell->setBorderRadius(SettingTheme::radiusLg);
		cell->setPadding(kPad, kPad, kPad, kPad);

		// 顶部行：左边类型图标 + 元信息（时间/日期 · 尺寸 · 大小）；剪切板条目右侧再放「复制」
		Ling::Node* headerRow = cell->makeChild<Ling::Node>();
		headerRow->setWidthPercent(100.f);
		headerRow->setHeight(kHeaderH);
		headerRow->setFlexShrink(0.f);
		headerRow->setMarginBottom(kHeaderGap);
		headerRow->setFlexDirection(Ling::FlexDirection::Row);
		headerRow->setAlignItems(Ling::Align::Center);

		// 图标只给剪切板卡片；截图/录屏/其他只显示元信息。
		// 类型图标：链接=链接、视频=视频、音乐=音乐、色值=外观设置、单个文件夹=文件夹，
		// 图片/SVG=图片，其余未分类（普通文件 / 混合多选）=插件集成。
		if (clip) {
			const wchar_t* headGlyph = Icon::Plugin;
			if (clipSvg) headGlyph = Icon::ImageBackground;
			else if (clipText) {
				const auto kind = clipTextKind(it.text);
				if (kind == ClipTextKind::Link) headGlyph = Icon::ClipLink;
				else if (kind == ClipTextKind::Color) headGlyph = Icon::Appearance;
				else headGlyph = Icon::Ocr;   // 纯文字
			}
			else if (clipImageKind) headGlyph = Icon::ImageBackground;
			else if (clipFiles) {
				if (it.files.size() == 1 && clipAllDirs(it.files)) headGlyph = Icon::Folder;
				else if (clipAllAudio(it.files)) headGlyph = Icon::ClipMusic;
				else if (clipAllFiles(it.files, false)) headGlyph = Icon::ClipVideo;
			}
			auto* headIcon = headerRow->makeChild<Ling::Label>();
			headIcon->setText(headGlyph);
			headIcon->setFontFamily(Icon::Family);
			headIcon->setFontSize(16.f);
			headIcon->setColor(SettingTheme::textMuted);   // 与「复制」按钮文字同色，避免过亮
			headIcon->setFlexShrink(0.f);
		}

		auto* headMeta = headerRow->makeChild<Ling::Label>();
		headMeta->setText(metaText);
		headMeta->setFontSize(SettingTheme::fontXs);
		headMeta->setColor(SettingTheme::textTertiary);
		headMeta->setMarginLeft(clip ? 6.f : 0.f);
		headMeta->setFlexShrink(1.f);
		headMeta->setJustifyContent(Ling::Justify::Start);
		headMeta->setAlignItems(Ling::Align::Center);

		auto* headSpacer = headerRow->makeChild<Ling::Node>();
		headSpacer->setFlexGrow(1.f);
		headSpacer->setFlexShrink(1.f);

		// 缩略图容器：右下角叠 贴图/打开/删除 图标按钮
		auto* thumbHost = cell->makeChild<Ling::Node>();
		thumbHost->setWidthPercent(100.f);
		// 顶部多一行，内容区相应减高：整张卡高度保持一致
		thumbHost->setHeight(kThumbH - kHeaderH - kHeaderGap);
		thumbHost->setFlexShrink(0.f);
		// 历史与剪切板统一使用截图历史的深色缩略图底。
		thumbHost->setBg(0x111111FFu);
		thumbHost->setBorderRadius(SettingTheme::radiusMd);
		thumbHost->setPositionType(Ling::Position::Relative);

		// 缩略图按缩略图级分辨率解码：历史页会随滚动不断累积格子，若每格都持有整张 4K 原图
		// （约 33MB）解码位图，内存会很快堆起来。上限取显示尺寸的 2 倍（物理像素）足够清晰。
		std::wstring thumbPath;
		if (isMedia) {
			const auto cached = (Setting::get()->getHistoryThumbDir() / (it.id + L".png")).wstring();
			std::error_code ec;
			if (std::filesystem::exists(cached, ec)) thumbPath = cached;
		}
		else if (clipFiles) {
			if (clipImageKind) thumbPath = it.files.front();
			else {
				// 本软件自己产出的文件（录屏/动图）：复用媒体历史的封面，不再显示文件名
				std::wstring key = it.files.front();
				for (auto& ch : key) ch = (wchar_t)towlower(ch);
				auto hit = ourMediaCover_.find(key);
				if (hit != ourMediaCover_.end()) thumbPath = hit->second;
			}
		}
		else if (!clipText) {
			thumbPath = it.filePath;
		}
		Ling::ScrollerBox* imgScroll = nullptr;   // 剪切板图片的滚动容器（无滑条）
		Ling::TextBox* textScroll = nullptr;      // 剪切板文字预览（省略显示，不滚动）
		std::wstring svgPreview;
		if (clipSvg) svgPreview = svgPreviewPath(it);
		if (!svgPreview.empty()) {
			// SVG 代码：渲染成图片，卡片里居中显示（不拉伸，超出就等比收缩）
			auto* img = thumbHost->makeChild<Ling::ImageBox>();
			img->setWidthPercent(100.f);
			img->setHeightPercent(100.f);
			img->setPadding(8.f, 8.f, 8.f, 8.f);
			img->setJustifyContent(Ling::Justify::Center);
			img->setAlignItems(Ling::Align::Center);
			img->loadImg(svgPreview, 800.f * win->dpi, 3200.f * win->dpi);
		}
		else if (clipText) {
			// 换行 / 制表都压成空格，交给 TextBox 按卡片实际高度裁剪并加省略号
			std::wstring preview = it.text;
			for (auto& ch : preview)
				if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
			auto* tb = thumbHost->makeChild<Ling::TextBox>();
			tb->setWidthPercent(100.f);
			tb->setHeightPercent(100.f);
			tb->setPadding(8.f, 6.f, 8.f, 6.f);
			tb->setBg(0);
			tb->setBorder(0.f, 0);
			tb->setText(preview);
			tb->setColor(SettingTheme::textPrimary);
			tb->setFontSize(SettingTheme::fontSm);
			tb->setReadOnly(true);
			tb->setMouseSelectionEnabled(false);
			tb->setTextHoverCursor(false);     // 卡片上停悬保持普通箭头（不是可编辑的暗示）
			tb->setTextEllipsis(true);         // 装不下就截断加省略号，卡片内不再滚动
			tb->setScrollBarVisible(false);   // 预览区不出现内部滑动条
			// 色值条目：缩略图区直接铺这个颜色，并按亮度换文字色
			uint32_t rgb{};
			if (clipTextKind(it.text, &rgb) == ClipTextKind::Color) {
				tb->setBg((rgb << 8) | 0xFFu);
				const int lum = (((rgb >> 16) & 0xFF) * 299 + ((rgb >> 8) & 0xFF) * 587 + (rgb & 0xFF) * 114) / 1000;
				tb->setColor(lum > 150 ? 0x14181CFFu : 0xFFFFFFFFu);
			}
		}
		else if (!thumbPath.empty()) {
			if (clip) {
				// 剪切板图片：整块铺满卡片（与截图卡片一致），不做内部滚动
				auto* img = thumbHost->makeChild<Ling::ImageBox>();
				img->setWidthPercent(100.f);
				img->setHeightPercent(100.f);
				img->loadImg(thumbPath, 800.f * win->dpi, 3200.f * win->dpi);
			}
			else {
				auto* img = thumbHost->makeChild<Ling::ImageBox>();
				img->setWidthPercent(100.f);
				img->setHeightPercent(100.f);
				const float thumbCap = (kThumbH + kPad * 2.f + kCaptionH) * 2.f * win->dpi;
				img->loadImg(thumbPath, thumbCap * 2.f, thumbCap);
			}
		}
		else if (clipFiles) {
			// 视频/其它文件/文件夹：只显示文件名（多个文件 → 「XXX.exe 等多个文件」）
			std::wstring name = std::filesystem::path(it.files.front()).filename().wstring();
			if (it.files.size() > 1) {
				std::wstring fmt = Lang::get(L"clip.multiFiles");
				const auto pos = fmt.find(L"%1");
				if (pos != std::wstring::npos) fmt.replace(pos, 2, name);
				name = fmt;
			}
			// 文件/文件夹只显示名字，不再叠一个大图标（类型已由左上角图标说明）。
			// 文字按卡片实际高度省略（末尾加省略号），不做卡片内滚动
			auto* nameBox = thumbHost->makeChild<Ling::TextBox>();
			nameBox->setWidthPercent(100.f);
			nameBox->setHeightPercent(100.f);
			nameBox->setPadding(8.f, 6.f, 8.f, 6.f);
			nameBox->setBg(0);
			nameBox->setBorder(0.f, 0);
			nameBox->setText(name);
			nameBox->setColor(SettingTheme::textPrimary);
			nameBox->setFontSize(SettingTheme::fontSm);
			nameBox->setReadOnly(true);
			nameBox->setMouseSelectionEnabled(false);
			nameBox->setTextHoverCursor(false);   // 卡片上停悬保持普通箭头
			nameBox->setTextEllipsis(true);
			nameBox->setScrollBarVisible(false);
		}
		else {
			// 目录里扫出来的录屏没有封面：用视频图标占位
			auto* ph = thumbHost->makeChild<Ling::Label>();
			ph->setWidthPercent(100.f);
			ph->setHeightPercent(100.f);
			ph->setText(Icon::Video);
			ph->setFontFamily(Icon::Family);
			ph->setFontSize(34.f);
			ph->setColor(0xFFFFFF66);
			ph->setJustifyContent(Ling::Justify::Center);
			ph->setAlignItems(Ling::Align::Center);
		}

		const std::wstring id = it.id;
		const std::wstring filePath = it.filePath;
		const std::wstring clipTextData = it.text;
		const std::vector<std::wstring> clipFileList = it.files;
		auto* delBtn = mkIconBtn(thumbHost, Icon::Remove, 6.f, true,
			[this, id, clip]() {
				if (this->overlayOpen()) return;
				// 剪切板条目只删索引（源文件/图片缓存由 removeClipboardItem 处理）；
				// 截图 / 录屏 / 其他里的文件删条目时移入回收站，避免误删后找不回
				if (clip) Setting::get()->removeClipboardItem(id);
				else Setting::get()->removeHistoryItem(id, true);
				// 延后一拍重建：这个按钮自己就在要被拆掉的格子里，同步重建会在它自己的
				// 点击回调里把它销毁；
				auto weak = getWeakThis();
				auto reload = [this, weak]() { if (weak.lock()) this->refreshGrid(); };
				if (auto* app = Ling::App::get()) app->dq.TryEnqueue(reload);
				else reload();
			});
		auto* pinBtn = clip
			? mkIconBtn(thumbHost, Icon::Pin, 38.f, false,
				[clipText, clipSvg, clipImageBitmap, clipImageKind, clipTextData, filePath, clipFileList]() {
					if (clipSvg) {
						pinSvgText(clipTextData);
					}
					else if (clipText) {
						ClipPin::showText(clipTextData);
					}
					else if (clipImageBitmap) {
						pinImageFile(filePath);
					}
					else if (clipImageKind && !clipFileList.empty()) {
						pinImageFile(clipFileList.front());
					}
					else if (!clipFileList.empty()) {
						ClipPin::showFiles(clipFileList);
					}
				})
			: (isMedia
				? mkIconBtn(thumbHost, Icon::Folder, 38.f, false, [this, filePath]() {
					if (this->overlayOpen()) return;
					revealInExplorer(filePath);
					})
				: mkIconBtn(thumbHost, Icon::Pin, 38.f, false, [this, id]() {
					if (this->overlayOpen()) return;
					Util::pinHistoryFile(id);
					}));
		// 「打开」：文字没有可打开的内容（链接除外）；图片用系统看图工具，文件用默认程序
		auto* openBtn = (clip && (!clipText || clipLink))
			? mkIconBtn(thumbHost, Icon::Folder, 70.f, false,
				[clipLink, clipTextData, filePath, clipFileList]() {
					if (clipLink) {
						// 链接：交给系统默认浏览器
						if (!clipTextData.empty())
							ShellExecuteW(nullptr, L"open", clipTextData.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
						return;
					}
					const std::wstring target = !clipFileList.empty() ? clipFileList.front() : filePath;
					if (!target.empty())
						ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
				})
			: nullptr;
		// 复制动作：剪切板条目用顶部常驻胶囊，其余分类沿用停悬显示的图标按钮
		auto copyAction = [this, clip, clipText, clipImageBitmap, clipTextData, filePath, clipFileList, id, thumbHost]() {
			if (this->overlayOpen()) return;
			if (!clip) Util::copyHistoryFileToClipboard(id);
			else {
				if (clipText) Ling::Util::setTextToClipboard(clipTextData);
				else if (clipImageBitmap) {
					auto [w, h, px] = Util::loadImagePixels(filePath);
					if (w > 0 && h > 0 && !px.empty()) Util::saveToClipboard(w, h, px.data());
				}
				else if (!clipFileList.empty()) Util::addFilesToClipboard(clipFileList);
				// 这次写入是我们自己触发的：不记新条目
				Setting::get()->markOwnClipboardWrite();
			}
			this->showCopyToast(thumbHost);
			// 先亮「复制成功」，过一会儿再把该条动画式地提到最前面
			if (clip) this->scheduleReorderToTop(id);
		};
		Ling::Button* copyBtn = nullptr;
		if (clip) {
			copyBtn = headerRow->makeChild<Ling::Button>();
			// 与顶部「筛选」按钮同一套外观：描边 + 同色文字 + 同样的停悬底色
			copyBtn->setText(Lang::get(L"setting.copy"));
			copyBtn->setFontSize(SettingTheme::fontXs);
			copyBtn->setColor(SettingTheme::textMuted);
			copyBtn->setHoverColor(SettingTheme::textPrimary);
			copyBtn->setBg(SettingTheme::card);
			copyBtn->setHoverBg(SettingTheme::accent);
			copyBtn->setHeight(22.f);
			copyBtn->setPadding(8.f, 0.f, 8.f, 0.f);
			copyBtn->setBorderRadius(SettingTheme::radiusCtl);
			copyBtn->setBorder(1.f, SettingTheme::border);
			copyBtn->setFlexShrink(0.f);
			copyBtn->setJustifyContent(Ling::Justify::Center);
			copyBtn->setAlignItems(Ling::Align::Center);
			copyBtn->onClick.add([copyAction](Ling::Button*) { copyAction(); });
		}
		else {
			copyBtn = mkIconBtn(thumbHost, Icon::Copy, 70.f, false, copyAction);
		}
		// 其余按钮默认收起，停悬到这张缩略图上才浮出
		pinBtn->hide(); delBtn->hide();
		if (openBtn) openBtn->hide();
		std::vector<Ling::Button*> btnList{ pinBtn, delBtn };
		if (openBtn) btnList.push_back(openBtn);
		if (!clip) { copyBtn->hide(); btnList.push_back(copyBtn); }
		CellSlot slot;
		slot.thumb = thumbHost;
		slot.btns = std::move(btnList);
		slot.imgScroll = imgScroll;
		slot.textScroll = textScroll;
		// 有静态小图的卡片提供停悬大图（录屏用的是缓存封面，不做预览）
		if (!thumbPath.empty() && !isMedia) {
			slot.previewPath = thumbPath;
			slot.previewW = it.width;
			slot.previewH = it.height;
		}
		// 剪切板文字卡：停悬显示全文；剪切板里的文件 / 文件夹停悬显示「图标 + 名字 + 来源」
		if (clipText) {
			slot.previewText = it.text;
		}
		else if (clip && clipFiles && !clipImageKind) {
			slot.previewFiles = it.files;
		}
		cells.push_back(std::move(slot));
		if (tip) {
			// 剪切板条目可能贴出文字 / 图片 / 文件等多种格式，统一叫「贴屏」
			const wchar_t* pinTipKey = (!clip && isMedia) ? L"setting.revealFile"
				: (clip ? L"setting.pinScreen" : L"setting.pin");
			tip->bind(copyBtn, Lang::get(L"setting.copy"));
			tip->bind(pinBtn, Lang::get(pinTipKey));
			tip->bind(delBtn, Lang::get(L"setting.delete"));
			if (openBtn) tip->bind(openBtn, Lang::get(L"setting.open"));
		}

		// 底部一行只放标签（点它可编辑自定义标签）
		auto* metaRow = cell->makeChild<Ling::Node>();
		metaRow->setFlexDirection(Ling::FlexDirection::Row);
		metaRow->setAlignItems(Ling::Align::Center);
		metaRow->setWidthPercent(100.f);
		metaRow->setHeight(kCaptionH - 8.f);
		metaRow->setMarginTop(6.f);

		// 标签位：剪切板固定先显示类型标签（图片/视频/文字/文件）+ 自定义标签；
		// 文件历史有自定义标签就显示标签，否则显示来源标签。
		struct TagChip { std::wstring text; bool editable; };
		const auto tags = clip ? Setting::get()->getClipboardTags(it.id)
			: Setting::get()->getHistoryTags(it.filePath);
		std::vector<TagChip> chips;
		if (clip) chips.push_back({ clipTypeTag(it), false });
		for (auto& t : tags) chips.push_back({ t, true });
		if (chips.empty()) {
			chips.push_back({ it.id == it.filePath
				? Lang::get(isMedia ? L"setting.tagVideo" : L"setting.tagImage")
				: sourceLabel(it.source), false });
		}
		for (auto& chipDef : chips) {
			// 所有分类的标签都可点击：类型标签 / 来源标签点开就是该条目的自定义标签编辑
			auto* chip = metaRow->makeChild<Ling::Button>();
			chip->setText(chipDef.text);
			chip->setFontSize(SettingTheme::fontXs);
			chip->setColor(SettingTheme::textTertiary);
			chip->setHoverColor(SettingTheme::textPrimary);
			chip->setBg(SettingTheme::secondary);
			chip->setHoverBg(SettingTheme::accent);
			chip->setHeight(16.f);
			chip->setPadding(4.f, 0.f, 4.f, 0.f);
			chip->setBorderRadius(4.f);
			chip->setBorder(0.f, 0);
			chip->setMarginLeft(4.f);
			chip->setFlexShrink(0.f);
			chip->setJustifyContent(Ling::Justify::Center);
			chip->setAlignItems(Ling::Align::Center);
			if (clip) {
				chip->onClick.add([this, id = it.id](Ling::Button*) {
					if (this->overlayOpen()) return;
					this->showTagDialogFor(id, Setting::get()->getClipboardTags(id),
						[id](const std::vector<std::wstring>& tags) {
							Setting::get()->setClipboardTags(id, tags);
						});
					});
			}
			else {
				chip->onClick.add([this, item = it](Ling::Button*) {
					if (this->overlayOpen()) return;   // 气泡/弹窗盖住卡片时不再响应
					this->showTagDialog(item);
				});
			}
			if (tip) tip->bind(chip, Lang::get(L"setting.filterTagEdit"));
		}

		lastRowCells++;
		builtIds.push_back(it.id);
	}

	// 末行不足一列：补等宽占位块，保持列宽一致（续补时会先撤掉）
	if (lastRow && lastRowCells > 0 && lastRowCells < kCols) {
		for (int c = lastRowCells; c < kCols; ++c) {
			auto* filler = lastRow->makeChild<Ling::Node>();
			filler->setWidthPercent(0.f);
			filler->setFlexGrow(1.f);
			filler->setFlexShrink(1.f);
			if (c != kCols - 1) filler->setMarginRight(kGap);
			lastRowFillers.push_back(filler);
		}
	}

	scheduleFillCheck();
}


void WinSettingHistory::scheduleFillCheck()
{
	if (fillScheduled) return;
	fillScheduled = true;
	auto weak = getWeakThis();
	auto run = [this, weak]() {
		if (!weak.lock()) return;
		this->fillScheduled = false;
		this->fillCheck();
	};
	// 延后一拍：refreshGrid 可能正由下拉/删除按钮的回调触发，不能在回调里同步重建整片网格
	if (auto* app = Ling::App::get()) app->dq.TryEnqueue(run);
	else run();
}

void WinSettingHistory::fillCheck()
{
	if (!gridScroll || !gridHost) return;

	auto all = collectItems(false);
	int total = 0;
	for (const auto& it : all)
		if (matchFilters(it)) ++total;
	if (total <= shownCount) return;

	// 判据用「自己算得出来的几何」，不依赖刚重建、还没重排的 content 高度：
	// 已渲染内容是否盖住「当前滚动位置 + 一屏 + 一行余量」，盖住就停。
	const int rows = (shownCount + kCols - 1) / kCols;
	const float dpi = win->dpi;
	const float cellH = (kThumbH + kPad * 2.f + kCaptionH) * dpi;
	const float gap = kGap * dpi;
	const float contentH = rows > 0 ? rows * cellH + (rows - 1) * gap : 0.f;
	const float need = gridScroll->getScrollY() + gridScroll->h + cellH;
	if (contentH >= need) return;

	shownCount = (std::min)(total, shownCount + kBatch);
	refreshGrid();
}

void WinSettingHistory::maybeScrollGrew()
{
	if (!gridScroll) return;
	const float y = gridScroll->getScrollY();
	if (y == lastScrollY) return;   // 鼠标动了但没滚动：忽略
	lastScrollY = y;
	scheduleFillCheck();
}

// 停悬到哪张缩略图上，就浮出哪张的 复制/贴图/删除；其余全部收起
void WinSettingHistory::updateHover(POINT pos)
{
	// 气泡 / 弹窗盖住卡片时，卡片不再响应停悬（隐藏浮出的图标按钮）
	const bool blocked = overlayOpen();
	// 停悬预览是可选功能（外观设置 > 其他外观 > 悬停预览），关掉时完全不参与
	const bool previewOn = Setting::get()->getHoverPreview();
	// 指针在可滚动的剪切板图片上时，让图片自己滚：外层网格暂时不响应滚轮
	bool overScrollableImage = false;
	int hoverPreview = -1;
	for (size_t i = 0; i < cells.size(); ++i) {
		auto& c = cells[i];
		if (!c.thumb) continue;
		const bool inside = !blocked && c.thumb->isPosIn(pos);
		for (auto* b : c.btns) {
			if (!b) continue;
			if (inside) b->show();
			else b->hide();
		}
		if (!blocked && c.imgScroll && c.imgScroll->isPosInContent(pos)
			&& c.imgScroll->getMaxScrollY() > 0.f)
			overScrollableImage = true;
		// 剪切板文字预览超出可视高度时，滚轮归文字自己，外层不再跟着滚
		if (!blocked && c.textScroll && c.textScroll->isPosIn(pos) && c.textScroll->canScroll())
			overScrollableImage = true;
		if (previewOn && inside
			&& (!c.previewPath.empty() || !c.previewText.empty() || !c.previewFiles.empty())
			&& hoverPreview < 0)
			hoverPreview = (int)i;
	}
	if (gridScroll) gridScroll->setWheelEnabled(!overScrollableImage);
	// 停悬大图：换到另一张（或移出所有卡片）时更新
	if (hoverPreview != previewIndex_) {
		previewIndex_ = hoverPreview;
		if (hoverPreview >= 0) showPreview(cells[hoverPreview]);
		else hidePreview();
	}
}

void WinSettingHistory::showPreview(const CellSlot& slot)
{
	if (!slot.thumb || !win) { hidePreview(); return; }
	POINT tl{ (LONG)slot.thumb->x, (LONG)slot.thumb->y };
	ClientToScreen(win->hwnd, &tl);
	const RECT cellRect{ tl.x, tl.y, tl.x + (LONG)slot.thumb->w, tl.y + (LONG)slot.thumb->h };
	if (!preview_) preview_ = std::make_unique<ShotPreviewWindow>(win->dpi);
	// 文件 / 文件夹：图标 + 名字 + 来源（与贴屏文件卡片一致）
	if (!slot.previewFiles.empty()) {
		preview_->showFiles(slot.previewFiles, cellRect);
		return;
	}
	// 文字卡：复用文字气泡样式显示全文
	if (!slot.previewText.empty()) {
		preview_->showText(slot.previewText, cellRect);
		return;
	}
	if (slot.previewPath.empty()) { hidePreview(); return; }
	// 优先用条目记录的尺寸；没有就只读图片头拿宽高（不解码像素）
	int w = slot.previewW, h = slot.previewH;
	if (w <= 0 || h <= 0) {
		const auto size = Util::imageSize(slot.previewPath);
		w = size.first;
		h = size.second;
	}
	if (w <= 0 || h <= 0) { hidePreview(); return; }
	preview_->showImage(slot.previewPath, w, h, cellRect);
}

void WinSettingHistory::hidePreview()
{
	if (preview_) preview_->hideNow();
	previewIndex_ = -1;
}

// 「复制成功」提示：在当前被复制的那张缩略图**正中**放一个小胶囊徽章（#0FDC78 底），
// 不全盖住图
void WinSettingHistory::showCopyToast(Ling::Node* thumbHost)
{
	if (!thumbHost) return;
	hideCopyToast();                 // 连点复制不叠加，先撤上一个
	// 透明全铺层只负责把徽章摆到正中；它不拦点击 —— 三个图标按钮都是各自订阅
	// win->onMouseDown 再 isPosIn 判定，不会被这层盖住
	auto* layer = thumbHost->makeChild<Ling::Node>();
	layer->setPositionType(Ling::Position::Absolute);
	layer->setPosition(Ling::Edge::Left, 0.f);
	layer->setPosition(Ling::Edge::Top, 0.f);
	layer->setPosition(Ling::Edge::Right, 0.f);
	layer->setPosition(Ling::Edge::Bottom, 0.f);
	layer->setBg(0);
	layer->setJustifyContent(Ling::Justify::Center);
	layer->setAlignItems(Ling::Align::Center);

	auto* badge = layer->makeChild<Ling::Label>();
	badge->setText(Lang::get(L"about.copySuccess"));
	badge->setFontSize(SettingTheme::fontSm);
	badge->setColor(0xFFFFFFFF);
	badge->setBg(0x0FDC78FF);        // #0FDC78
	badge->setPadding(10.f, 5.f, 10.f, 5.f);
	badge->setBorderRadius(SettingTheme::radiusFull);
	badge->setJustifyContent(Ling::Justify::Center);
	badge->setAlignItems(Ling::Align::Center);

	copyToast = layer;
	win->setTimer(kToastMs, kToastTimerId);
}

void WinSettingHistory::hideCopyToast()
{
	win->killTimer(kToastTimerId);
	// 注意：格子只在「整体重建 / 空态」里被 removeAllChildren 掉，那两条路径都会把
	// copyToast 置空，所以这里指针不会悬空。
	if (copyToast && copyToast->parent)
		copyToast->parent->removeChild(copyToast);
	copyToast = nullptr;
}

void WinSettingHistory::scheduleReorderToTop(const std::wstring& id)
{
	pendingReorderId_ = id;
	win->killTimer(kReorderTimerId);
	win->setTimer(kReorderDelayMs, kReorderTimerId);
}

void WinSettingHistory::applyReorderToTop()
{
	win->killTimer(kReorderTimerId);
	const std::wstring id = pendingReorderId_;
	pendingReorderId_.clear();
	if (id.empty()) return;
	// 只有当前页面真的会显示剪切板条目时才有必要播动画
	const bool visible = (sourceTab == 4 || (sourceTab == 0 && !filter.tag.empty()));
	if (!Setting::get()->touchClipboardItem(id)) return;   // 已在最前：让提示自然收起
	if (!visible) return;
	hideCopyToast();
	// clipboardChanged 已经把网格重建排进队列；我们再排一拍，等新网格建好后再淡入
	auto weak = getWeakThis();
	auto run = [this, weak]() { if (weak.lock()) this->fadeInGrid(); };
	if (auto* app = Ling::App::get()) app->dq.TryEnqueue(run);
	else run();
}

void WinSettingHistory::fadeInGrid()
{
	if (!gridHost || !gridHost->visual) return;
	auto anim = gridHost->visual.Compositor().CreateScalarKeyFrameAnimation();
	anim.InsertKeyFrame(0.f, 0.f);
	anim.InsertKeyFrame(1.f, 1.f);
	anim.Duration(std::chrono::milliseconds{ 200 });
	gridHost->visual.Opacity(0.f);
	gridHost->visual.StartAnimation(L"Opacity", anim);
}
