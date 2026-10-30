#include "pch.h"
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <thread>
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Filters.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.System.Threading.h>

#include "../Lang.h"
#include "../Setting.h"
#include "../Tool/IconCodes.h"
#include "../Update.h"
#include "../Util.h"
#include "WinSetting.h"
#include "WinSettingAbout.h"
#include "SettingTheme.h"
#include "SettingWidgets.h"

// 关于页（逐项对齐 design/about-page.html 已确认的版本）：
//   ① 配图 520×176，水平居中，顶部被卡片裁掉 14px，下沿压住标题 39px（图在字上面）
//   ② SnowAir 大字 56px / 600
//   ③ 版本号 + Lite beta 胶囊（高 22 / 字号 11.5 / 间距 9）
//   ④ 口号 13.5px 实色
//   ⑤ 按钮组 margin-top:auto → 顶到卡片底部；按钮高 36 / 圆角 8 / 间距 10
//   ⑥ 分隔线 + 配置目录（含打开目录按钮）+ © Liuyiran · 更新日志
// 页面高度恒等于卡片高度（WinSetting::loadPage 里直接挂 cardNode），
// 这样 ⑤ 的 auto 边距才有可分配空间，底部信息才会贴卡片底。
// 配图压字的实现：图片行高只占 176-39，图本身 176 向下溢出；再 bringToFront 提到标题之上。

namespace {

// 配图展示尺寸（逻辑像素）：宽 524，高度按原图 1045:384 的比例算 → ≈192.5。
// 宽高比跟原图一致，ImageBox 就会正好按宽度铺满、不再缩到 479。
constexpr float kArtW{ 524.f };
constexpr float kArtH{ kArtW * 384.f / 1045.f };
// 设计稿里配图的 margin-top 是 -44（内容区上内边距 30），即顶部被卡片裁掉 14px
constexpr float kArtTopClip{ 14.f };
// 标题被配图压住的深度：设计稿里配图下沿 162、标题起点 123 → 39。
// 实机里图片底部带一圈透明留白，所以取值要更大，标题上沿才会真正落到图纸上。
// 每个 +1 = 标题上移 1px（压得更深）。
constexpr float kArtOverlap{ 62.f };

// 内嵌的配图是 RCDATA，而 Ling::Image 只认文件路径（内部会整块读进内存再解码）。
// 所以先落到 %TEMP%，加载完立刻删掉 —— 不在用户配置目录里留垃圾。
// 返回路径；调用方用完负责删除（loadImg 是同步的，返回后即可删）。
std::wstring writeArtToTemp()
{
	auto [data, size] = Ling::Util::getRes(L"about");
	if (data == nullptr || size == 0) return {};

	wchar_t tmp[MAX_PATH]{};
	if (GetTempPathW(MAX_PATH, tmp) == 0) return {};

	auto file = std::filesystem::path{ tmp } / L"SnowAir-about-art.png";
	std::ofstream out(file, std::ios::binary | std::ios::trunc);
	if (!out) return {};
	out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
	out.close();
	if (!out) return {};
	return file.wstring();
}

// 旧版本会把图留在配置目录里，这里顺手清掉
void removeLegacyArtFile()
{
	const auto legacy = Setting::get()->getDataPath() / L"about-art.png";
	std::error_code ec;
	std::filesystem::remove(legacy, ec);
}

// 整行居中的容器（水平居中 + 交叉轴居中）
Ling::Node* centerRow(Ling::Node* parent)
{
	auto* row = parent->makeChild<Ling::Node>();
	row->setWidthPercent(100.f);
	row->setFlexDirection(Ling::FlexDirection::Row);
	row->setJustifyContent(Ling::Justify::Center);
	row->setAlignItems(Ling::Align::Center);
	return row;
}

// 只读胶囊（版本号 / Lite beta）。故意不用 Button：不能点，就不该出现手型光标。
Ling::Node* badge(Ling::Node* parent, const std::wstring& text, bool accent)
{
	auto* box = parent->makeChild<Ling::Node>();
	box->setHeight(22.f);
	box->setBorderRadius(SettingTheme::radiusFull);
	box->setPadding(10.f, 0.f, 10.f, 0.f);
	box->setBorder(1.f, accent ? SettingTheme::successBorder : SettingTheme::border);
	box->setBg(accent ? SettingTheme::successBg : SettingTheme::card);
	box->setAlignItems(Ling::Align::Center);
	box->setJustifyContent(Ling::Justify::Center);

	auto* label = box->makeChild<Ling::Label>();
	label->setText(text);
	label->setFontSize(11.5f);
	label->setColor(accent ? SettingTheme::successFg : SettingTheme::textSecondary);
	return box;
}

// 会自转的图标 Label：检查更新时让刷新图标转起来。
// 走 Composition 标量动画（合成线程跑，不占 UI 线程、不用定时器）。
class SpinLabel : public Ling::Label
{
public:
	explicit SpinLabel(Ling::WinBase* w) : Ling::Label(w) {}

	void startSpin()
	{
		auto compositor = visual.Compositor();
		auto anim = compositor.CreateScalarKeyFrameAnimation();
		anim.InsertKeyFrame(1.0f, 360.f);
		anim.Duration(std::chrono::milliseconds(900));
		anim.IterationBehavior(winrt::Windows::UI::Composition::AnimationIterationBehavior::Forever);
		visual.StartAnimation(L"RotationAngleInDegrees", anim);
	}
	void stopSpin()
	{
		visual.StopAnimation(L"RotationAngleInDegrees");
		visual.RotationAngleInDegrees(0.f);
	}
protected:
	void layout() override
	{
		Ling::Label::layout();
		// 绕自身中心转：CenterPoint 是视觉局部坐标，必须等 layout 拿到真实 w/h 再设
		visual.CenterPoint({ w * 0.5f, h * 0.5f, 0.f });
	}
};

// 图标 + 文字的按钮：Button 只负责背景/圆角/命中与悬停，图标和文字用两个 Label 手动摆。
// （Button 内置的 Text 置空；这也是侧栏菜单项一直在用的做法）
Ling::Button* iconButton(Ling::Node* parent, const wchar_t* glyph, const std::wstring& text,
	bool primary, std::function<void()> onClick,
	Ling::Label** outLabel = nullptr, SpinLabel** outIcon = nullptr)
{
	auto* btn = parent->makeChild<Ling::Button>();
	btn->setText(L"");
	btn->setHeight(36.f);
	btn->setBorderRadius(8.f);
	btn->setPadding(16.f, 0.f, 16.f, 0.f);
	btn->setFlexDirection(Ling::FlexDirection::Row);
	btn->setJustifyContent(Ling::Justify::Center);
	btn->setAlignItems(Ling::Align::Center);
	if (primary) {
		btn->setBorder(1.f, SettingTheme::primary);
		btn->setBg(SettingTheme::primary);
		btn->setHoverBg(SettingTheme::primaryHover);
	}
	else {
		btn->setBorder(1.f, SettingTheme::border);
		btn->setBg(SettingTheme::card);
		btn->setHoverBg(SettingTheme::accent);
	}

	const uint32_t fg = primary ? SettingTheme::primaryForeground : SettingTheme::foreground;

	auto* icon = btn->makeChild<SpinLabel>();
	icon->setText(glyph);
	icon->setFontFamily(Icon::Family);
	icon->setFontSize(15.f);
	icon->setColor(fg);
	icon->setMarginRight(7.f);

	auto* label = btn->makeChild<Ling::Label>();
	label->setText(text);
	label->setFontSize(SettingTheme::fontBase);
	label->setColor(fg);

	if (onClick) {
		btn->onClick.add([onClick](Ling::Button*) { onClick(); });
	}
	if (outLabel) *outLabel = label;
	if (outIcon) *outIcon = icon;
	return btn;
}

// "v0.1.12" / "0.1.12" / "v0.1.12-beta" → {0,1,12}；认不出来就全 0（等于「不比本地新」）
std::array<int, 3> parseVersionTag(const std::wstring& tag)
{
	std::array<int, 3> ver{ 0, 0, 0 };
	int idx = 0;
	std::wstring digits;
	auto flush = [&]() {
		if (digits.empty() || idx >= 3) { digits.clear(); return; }
		try { ver[idx++] = std::stoi(digits); }
		catch (...) {}
		digits.clear();
	};
	for (wchar_t ch : tag) {
		if (ch >= L'0' && ch <= L'9') digits.push_back(ch);
		else flush();
	}
	flush();
	return ver;
}

// 拉 GitHub 最新 release 的 tag。网络/解析失败一律 ok=false，不抛出去。
// 8 秒没响应就取消（避免请求挂住导致按钮看起来「卡死」）。
// 「检查中」至少显示 700ms：命中 HTTP 缓存时请求几毫秒就回来了，不兜底的话
// 图标还没转起来就停了，看起来像「再按没反应」。
// 结果经 app 调度队列回到 UI 线程，并用 weak 守卫页面是否还活着；seq 用于丢弃过期请求。
winrt::fire_and_forget fetchLatestTag(std::weak_ptr<bool> weak, WinSettingAbout* page, int seq)
{
	using namespace winrt::Windows::Web::Http;
	using namespace winrt::Windows::Web::Http::Filters;
	using namespace winrt::Windows::Data::Json;

	constexpr auto kMinSpin = std::chrono::milliseconds(700);
	const auto started = std::chrono::steady_clock::now();

	std::wstring tag;
	bool ok{ false };
	try {
		co_await winrt::resume_background();
		// 每次都真正问一次服务器：版本检查不该吃到上一次的缓存
		HttpBaseProtocolFilter filter;
		filter.CacheControl().ReadBehavior(HttpCacheReadBehavior::NoCache);
		HttpClient client{ filter };
		// GitHub API 必须带 User-Agent，否则直接 403
		client.DefaultRequestHeaders().UserAgent().TryParseAdd(L"SnowAir");
		auto op = client.GetStringAsync(
			winrt::Windows::Foundation::Uri{ L"https://github.com/CM-idea/SnowAir/releases.atom" });
		auto guard = winrt::Windows::System::Threading::ThreadPoolTimer::CreateTimer(
			[op](const winrt::Windows::System::Threading::ThreadPoolTimer&) { op.Cancel(); },
			std::chrono::seconds(8));
		std::wstring body{ co_await op };
		guard.Cancel();
		// Releases Atom 不受未认证 GitHub API 的 60 次/小时限流影响
		tag = Util::parseGithubReleaseTag(body);
		ok = !tag.empty();
	}
	catch (...) {
		ok = false;
	}

	// 已经在线程池线程上，直接睡够剩余时间，保证动画可见
	const auto spent = std::chrono::steady_clock::now() - started;
	if (spent < kMinSpin) {
		std::this_thread::sleep_for(kMinSpin - spent);
	}

	if (auto app = Ling::App::get()) {
		app->dq.TryEnqueue([weak, page, seq, ok, tag]() {
			if (!weak.lock()) return;   // 页面已经切走/销毁
			page->onCheckFinished(seq, ok, tag);
		});
	}
}

} // namespace

WinSettingAbout::WinSettingAbout(Ling::WinBase* parent) : Ling::Node(parent)
{
	setFlexDirection(Ling::FlexDirection::Column);
	setWidthPercent(100.f);
	YGNodeStyleSetMinWidth(node, 0.f);

	// ① 配图不参与排版：绝对定位、最后创建（后加的子节点画在上面），位置在 layout() 里算。
	//    标题用 marginTop 让出图的高度，上沿落进图纸里 → 「图压字」。

	// ② SnowAir 大字
	auto* titleRow = centerRow(this);
	titleRow->setMarginTop(kArtH - kArtTopClip - kArtOverlap - SettingTheme::contentPadY);
	auto* title = titleRow->makeChild<Ling::Label>();
	title->setText(L"SnowAir");
	title->setFontFamily(L"Segoe UI");
	title->setFontSize(56.f);
	title->setFontWeight(600);
	title->setColor(SettingTheme::textPrimary);

	// ③ 版本号 + 版本标记
	auto* metaRow = centerRow(this);
	metaRow->setMarginTop(18.f);
	const auto ver = Util::getAppVersion();
	badge(metaRow, wfmt(L"v%d.%d.%d", ver[0], ver[1], ver[2]), false);
	auto* edition = badge(metaRow, L"Lite beta", true);
	edition->setMarginLeft(9.f);

	// ④ 口号
	auto* sloganRow = centerRow(this);
	sloganRow->setMarginTop(16.f);
	auto* slogan = sloganRow->makeChild<Ling::Label>();
	slogan->setText(Lang::get(L"about.slogan"));
	slogan->setFontSize(13.5f);
	slogan->setColor(SettingTheme::textPrimary);

	// ⑤ 按钮组：图标 + 文字；margin-top:auto 让整组沉到卡片底部
	auto* actions = centerRow(this);
	YGNodeStyleSetMarginAuto(actions->node, YGEdgeTop);

	SpinLabel* checkIcon = nullptr;
	checkBtn_ = iconButton(actions, Icon::Refresh, Lang::get(L"about.checkUpdate"), true,
		[this]() { startCheck(); }, &checkLabel_, &checkIcon);
	checkBtn_->setMarginRight(10.f);
	if (checkIcon) {
		spinIcon_ = [checkIcon](bool on) {
			if (on) checkIcon->startSpin();
			else    checkIcon->stopSpin();
		};
	}

	auto* feedback = iconButton(actions, Icon::Bubble, Lang::get(L"about.feedback"), false, []() {
		ShellExecuteW(nullptr, L"open", L"https://github.com/CM-idea/SnowAir/issues",
			nullptr, nullptr, SW_SHOWNORMAL);
	});
	feedback->setMarginRight(10.f);

	iconButton(actions, Icon::Github, Lang::get(L"about.repo"), false, []() {
		ShellExecuteW(nullptr, L"open", L"https://github.com/CM-idea/SnowAir",
			nullptr, nullptr, SW_SHOWNORMAL);
	});

	// ⑤-2 检查结果状态行：● 已是最新版本 · 刚刚检查
	auto* statusRow = centerRow(this);
	statusRow->setMarginTop(18.f);

	statusDot_ = statusRow->makeChild<Ling::Node>();
	statusDot_->setSize(6.f, 6.f);
	statusDot_->setBorderRadius(3.f);
	statusDot_->setBg(SettingTheme::textTertiary);
	statusDot_->setMarginRight(8.f);

	statusText_ = statusRow->makeChild<Ling::Label>();
	statusText_->setText(Lang::get(L"about.notChecked"));
	statusText_->setFontSize(12.f);
	statusText_->setColor(SettingTheme::textTertiary);

	statusTime_ = statusRow->makeChild<Ling::Label>();
	statusTime_->setFontSize(12.f);
	statusTime_->setColor(SettingTheme::textTertiary);
	statusTime_->setMarginLeft(4.f);

	// 状态行尾：自动检查频率下拉（每天 / 每周 / 每月 / 从不）
	const std::vector<std::wstring> freqOptions{
		Lang::get(L"about.checkFreqDay"),
		Lang::get(L"about.checkFreqWeek"),
		Lang::get(L"about.checkFreqMonth"),
		Lang::get(L"about.checkFreqNever"),
	};
	auto* freqDdl = SettingUi::selectDdl(statusRow, freqOptions,
		Setting::get()->getUpdateCheckFreq(),
		[](int idx, const std::wstring&) {
			Setting::get()->setUpdateCheckFreq(idx);
			if (idx != 3) Update::checkLater();   // 改频率后若已到期就立刻补查一次
		}, true);
	freqDdl->setWidth(64.f);
	freqDdl->setHeight(20.f);
	freqDdl->setMarginLeft(10.f);
	freqDdl->setFlexShrink(0.f);
	// 常态做成和「尚未检查」同一档文字：12px、三级灰、无边框/无底色；停悬才浮出浅底
	freqDdl->setBorder(0.f, 0);
	freqDdl->setBg(0);
	freqDdl->setHoverBg(SettingTheme::accent);
	freqDdl->setColor(SettingTheme::textTertiary);
	freqDdl->setPadding(4.f, 0.f, 4.f, 0.f);
	freqDdl->setBorderRadius(4.f);
	for (auto& c : freqDdl->children) {
		if (auto* lab = dynamic_cast<Ling::Label*>(c.get())) {
			lab->setFontSize(12.f);
			lab->setColor(SettingTheme::textTertiary);
		}
	}

	// 每日后台检查如果已经发现新版本，进来就直接显示出来
	if (const auto pending = Update::pendingVersion(); !pending.empty()) {
		setStatus(Lang::get(L"about.newVersion") + L" " + pending, L"", SettingTheme::success);
	}

	// ⑥ 分隔线 + 底部信息（打开配置目录 / 版权 / 更新日志）
	// 上下各 18：状态行上方 18、下方到分隔线也是 18，视觉上对称
	SettingUi::separator(this, 18.f, 16.f);

	// 「打开配置目录」：无边框的轻按钮，文字 + 图标（图标在右）。
	// 常态与「更新日志」同色（textTertiary）；悬停加浅底、文字转深色。
	auto* dirRow = makeChild<Ling::Node>();
	dirRow->setWidthPercent(100.f);
	dirRow->setFlexDirection(Ling::FlexDirection::Row);
	dirRow->setAlignItems(Ling::Align::Center);

	auto* openDir = dirRow->makeChild<Ling::Button>();
	openDir->setText(L"");                                   // 内置 Text 置空，图标和文字用手动 Label
	openDir->setHeight(22.f);
	openDir->setBorderRadius(SettingTheme::radiusInner);      // 6：轻量圆角，贴合小字号
	openDir->setPadding(6.f, 0.f, 6.f, 0.f);
	openDir->setMarginLeft(-6.f);                             // 文字左缘与下面「© Liuyiran」对齐
	openDir->setFlexDirection(Ling::FlexDirection::Row);
	openDir->setJustifyContent(Ling::Justify::Center);
	openDir->setAlignItems(Ling::Align::Center);
	openDir->setBorder(0.f, 0);
	openDir->setBg(0);
	openDir->setHoverBg(SettingTheme::accent);

	auto* dirText = openDir->makeChild<Ling::Label>();
	dirText->setText(Lang::get(L"about.openDataDir"));
	dirText->setFontSize(11.5f);
	dirText->setColor(SettingTheme::textTertiary);

	auto* dirIcon = openDir->makeChild<Ling::Label>();
	dirIcon->setText(Icon::Folder);
	dirIcon->setFontFamily(Icon::Family);
	// 图标字形净高只有 em 的 ~0.75：11.5px 正文配 14px 图标才视觉等高
	dirIcon->setFontSize(14.f);
	dirIcon->setColor(SettingTheme::textTertiary);
	dirIcon->setMarginLeft(5.f);

	// 两个 Label 不在 Button 的 hover 变色链上，手动跟随：悬停转深色
	openDir->onEnter.add([dirText, dirIcon](Ling::Button*) {
		dirText->setColor(SettingTheme::textPrimary);
		dirIcon->setColor(SettingTheme::textPrimary);
	});
	openDir->onLeave.add([dirText, dirIcon](Ling::Button*) {
		dirText->setColor(SettingTheme::textTertiary);
		dirIcon->setColor(SettingTheme::textTertiary);
	});

	openDir->onClick.add([](Ling::Button*) {
		const auto dir = Setting::get()->getDataPath();
		ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	});

	auto* crRow = makeChild<Ling::Node>();
	crRow->setWidthPercent(100.f);
	crRow->setFlexDirection(Ling::FlexDirection::Row);
	crRow->setAlignItems(Ling::Align::Center);
	crRow->setMarginTop(10.f);

	auto* author = crRow->makeChild<Ling::Label>();
	author->setText(L"城迷 · © Liuyiran");
	author->setFontSize(11.5f);
	author->setColor(SettingTheme::textTertiary);

	auto* dot = crRow->makeChild<Ling::Label>();
	dot->setText(L"·");
	dot->setFontSize(11.5f);
	dot->setColor(SettingTheme::textTertiary);
	dot->setMarginLeft(8.f);
	dot->setMarginRight(8.f);

	auto* changelog = crRow->makeChild<Ling::Button>();
	changelog->setText(L"");
	changelog->setBg(0);
	changelog->setBorder(0.f, 0);
	changelog->setHoverBg(0);
	changelog->setPadding(0.f, 0.f, 0.f, 0.f);
	changelog->setFlexDirection(Ling::FlexDirection::Row);
	changelog->setAlignItems(Ling::Align::Center);

	auto* changelogText = changelog->makeChild<Ling::Label>();
	changelogText->setText(Lang::get(L"about.changelog"));
	changelogText->setFontSize(11.5f);
	changelogText->setColor(SettingTheme::textTertiary);

	// 外链提示：文字右侧一个小箭头（字库里 e637「箭头」）
	auto* changelogArrow = changelog->makeChild<Ling::Label>();
	changelogArrow->setText(Icon::Arrow);
	changelogArrow->setFontFamily(Icon::Family);
	changelogArrow->setFontSize(11.f);
	changelogArrow->setColor(SettingTheme::textTertiary);
	changelogArrow->setMarginLeft(4.f);

	// 两个 Label 不在 Button 的 hover 变色链上，手动跟随
	changelog->onEnter.add([changelogText, changelogArrow](Ling::Button*) {
		changelogText->setColor(SettingTheme::textPrimary);
		changelogArrow->setColor(SettingTheme::textPrimary);
	});
	changelog->onLeave.add([changelogText, changelogArrow](Ling::Button*) {
		changelogText->setColor(SettingTheme::textTertiary);
		changelogArrow->setColor(SettingTheme::textTertiary);
	});

	changelog->onClick.add([](Ling::Button*) {
		ShellExecuteW(nullptr, L"open", L"https://github.com/CM-idea/SnowAir/releases",
			nullptr, nullptr, SW_SHOWNORMAL);
	});

	// ⑦ 配图：最后创建 → z 序最上层（后加的子节点叠在前面的上面），因此压住标题而不是被压。
	//    位置（居中 + 顶到 -14px）在 layout() 里算，绝对定位不占排版空间。
	artHost_ = makeChild<Ling::Node>();
	artHost_->setPositionType(Ling::Position::Absolute);
	artHost_->setSize(kArtW, kArtH);
	auto* art = artHost_->makeChild<Ling::ImageBox>();
	art->setWidth(kArtW);
	art->setHeight(kArtH);
	if (const auto path = writeArtToTemp(); !path.empty()) {
		// maxW/maxH 是物理像素：按当前 DPI 取解码上限，避免把整张原图解码进内存
		art->loadImg(path, kArtW * win->dpi, kArtH * win->dpi);
		// loadImg 是同步的（整块读进内存后解码），加载完就能把临时文件删掉
		std::error_code ec;
		std::filesystem::remove(path, ec);
	}
	removeLegacyArtFile();
}

WinSettingAbout::~WinSettingAbout()
{
}

void WinSettingAbout::layout()
{
	Ling::Node::layout();
	if (!artHost_) return;
	// 水平居中；顶部再上移 14px 被卡片圆角裁掉（视觉上像图从卡片顶边伸出来）
	const float left = std::round((w - kArtW) * 0.5f);
	artHost_->visual.Offset({ left, -kArtTopClip, 0.f });
}

void WinSettingAbout::startCheck()
{
	// 不限制点击次数：每点一次就重开一轮，旧请求的结果按 seq 丢弃
	const int seq = ++checkSeq_;
	// 图标转起来、文字切成「检查中…」（和网页预览版一致）
	if (spinIcon_) spinIcon_(true);
	if (checkLabel_) checkLabel_->setText(Lang::get(L"about.checkingBtn"));
	setStatus(Lang::get(L"about.checking"), L"", SettingTheme::textTertiary);
	// 主程序与晾衣架插件同时检查；插件结果由 Update 缓存，插件集成页直接读取
	Update::checkRackUpdate(true);
	// 网络请求在后台线程跑，回来经 dq 落到 onCheckFinished（内部用 weak 守卫页面存活）
	fetchLatestTag(getWeakThis(), this, seq);
}

void WinSettingAbout::setStatus(const std::wstring& text, const std::wstring& time, uint32_t dotColor)
{
	if (statusText_) statusText_->setText(text);
	if (statusTime_) statusTime_->setText(time);
	if (statusDot_)  statusDot_->setBg(dotColor);
}

void WinSettingAbout::onCheckFinished(int seq, bool ok, const std::wstring& remoteTag)
{
	if (seq != checkSeq_) return;   // 又点了一次：这次结果已过期，丢弃
	if (spinIcon_) spinIcon_(false);
	if (checkLabel_) checkLabel_->setText(Lang::get(L"about.checkUpdate"));

	if (!ok) {
		setStatus(Lang::get(L"about.checkFailed"), L"", SettingTheme::destructive);
		return;
	}

	const auto remote = parseVersionTag(remoteTag);
	const auto local = Util::getAppVersion();
	if (remote > local) {
		const auto verStr = wfmt(L"%d.%d.%d", remote[0], remote[1], remote[2]);
		setStatus(Lang::get(L"about.newVersion") + L" " + verStr, L"", SettingTheme::success);
		// 有新版本：直接打开 releases 页面（原本按钮就是这个行为）
		ShellExecuteW(nullptr, L"open", L"https://github.com/CM-idea/SnowAir/releases",
			nullptr, nullptr, SW_SHOWNORMAL);
	}
	else {
		setStatus(Lang::get(L"about.upToDate"), Lang::get(L"about.justNow"), SettingTheme::success);
	}
}
