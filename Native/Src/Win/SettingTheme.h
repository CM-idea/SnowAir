#pragma once
#include <cstdint>
#include <windows.h>

// ============================================================
//  shadcn/ui · Default (Zinc) 设计令牌 —— 浅色 / 深色两套，运行时切换
//  颜色存储：AABBGGRR？不是 —— 本工程统一 RGBA（0xRRGGBBAA，AA=FF 为不透明）
//  设计原则：
//    1. 严格对齐 shadcn 的 CSS 变量语义。
//    2. 暴露完整 Zinc 50-950 色阶，方便组件按语义取用（色阶不随主题变）。
//    3. 字号不使用粗体，通过字号阶梯 + 颜色对比建立层级。
//    4. 所有圆角、间距、控件高度均走 4px 网格。
//  切换方式：SettingTheme::apply(mode)（0 跟随系统 / 1 浅色 / 2 深色）。
//  注意：语义色是运行时变量（默认值 = 浅色），所有界面都必须在构建前先 apply 一次。
// ============================================================
namespace SettingTheme {

// ─── 0. Zinc 完整色阶（50 ~ 950）—— 原始调色板，不随主题变 ────
inline constexpr uint32_t zinc50  { 0xFAFAFAFF };  // hsl(0  0%  98%)
inline constexpr uint32_t zinc100 { 0xF5F4F4FF };  // hsl(240 4.8% 95.9%)
inline constexpr uint32_t zinc200 { 0xE7E4E4FF };  // hsl(240 5.9% 90%)
inline constexpr uint32_t zinc300 { 0xD4D4D8FF };  // hsl(240 4.9% 83.9%)
inline constexpr uint32_t zinc400 { 0xA1A1AAFF };  // hsl(240 5% 64.9%)
inline constexpr uint32_t zinc500 { 0x71717AFF };  // hsl(240 3.8% 46.1%)
inline constexpr uint32_t zinc600 { 0x52525BFF };  // hsl(240 5.2% 33.9%)
inline constexpr uint32_t zinc700 { 0x3F3F46FF };  // hsl(240 5.3% 26.1%)
inline constexpr uint32_t zinc800 { 0x27272AFF };  // hsl(240 3.7% 16.1%)
inline constexpr uint32_t zinc900 { 0x18181BFF };  // hsl(240 5.9% 10%)
inline constexpr uint32_t zinc950 { 0x09090BFF };  // hsl(240 10% 3.9%)

// ─── 1. 语义色（浅色默认值；apply() 会按主题重写）────────────
inline uint32_t background       { 0xFFFFFFFF };       // 内容区底
inline uint32_t foreground       { zinc950 };          // 主文字
inline uint32_t card             { 0xFFFFFFFF };       // 卡片底
inline uint32_t cardForeground   { zinc950 };
inline uint32_t popover          { 0xFFFFFFFF };       // 浮层（下拉/气泡）底
inline uint32_t popoverForeground{ zinc950 };
inline uint32_t popupSelBg       { 0xF5F4F4FF };       // 下拉里当前项 / 悬停项底
inline uint32_t popupFg          { zinc950 };          // 下拉文字（深色下恒为白，悬停也不变）
inline uint32_t primary          { zinc900 };          // 主按钮底
inline uint32_t primaryForeground{ zinc50 };
inline uint32_t primaryHover     { zinc800 };          // 主按钮悬停底
inline uint32_t secondary        { zinc100 };
inline uint32_t secondaryForeground{ zinc950 };
inline uint32_t muted            { zinc100 };
inline uint32_t mutedForeground  { zinc500 };          // 次级文字
inline uint32_t accent           { zinc100 };          // hover / focus 底色
inline uint32_t accentForeground { zinc950 };
inline uint32_t destructive      { 0xEF4444FF };       // red-500（注意：Color 按 RRGGBBAA 解析，勿写反成 AABBGGRR）
inline uint32_t destructiveForeground{ zinc50 };
inline uint32_t border           { zinc200 };
inline uint32_t input            { zinc200 };
inline uint32_t ring             { zinc950 };          // focus ring

// ─── 2. 侧栏（菜单）专用 ────────────────────────────────────
inline uint32_t sidebar          { 0xFFFFFFFF };
inline uint32_t sidebarForeground{ zinc950 };
inline uint32_t sidebarPrimary   { zinc950 };
inline uint32_t sidebarAccent    { 0xE5E5E5FF };       // 菜单行 hover 底
inline uint32_t sidebarSelected  { 0xE5E5E5FF };       // 菜单行选中底
inline uint32_t sidebarSelectedFg{ zinc950 };          // 菜单行选中文字（与选中底成对，勿拆开改）
inline uint32_t sidebarBorder    { zinc200 };
inline uint32_t sidebarRing      { zinc950 };

// ─── 3. 状态与语义扩展 ──────────────────────────────────────
inline uint32_t faintForeground  { zinc400 };          // 占位/版本号/三级文字
inline uint32_t disabledForeground{ zinc400 };
inline uint32_t placeholder      { zinc500 };
inline uint32_t success          { 0x5EC322FF };       // green-500
inline uint32_t successBg        { 0xF0FDF4FF };       // green-50
inline uint32_t successBorder    { 0x86EFACFF };       // green-300
inline uint32_t successFg        { 0x14532DFF };       // green-900
inline uint32_t warningBg        { 0xFFFEF0FF };       // amber-50
inline uint32_t warningBorder    { 0xFCD34DFF };       // amber-300
inline uint32_t warningFg        { 0x713F12FF };       // amber-900
inline uint32_t infoBg           { 0xEFF6FFFF };       // blue-50
inline uint32_t infoBorder       { 0x93C5FDFF };       // blue-300
inline uint32_t infoFg           { 0x1E3A8AFF };       // blue-900
inline uint32_t destructiveBg    { 0xFEF2F2FF };       // red-50
inline uint32_t destructiveBorder{ 0xFECACAFF };       // red-200
inline uint32_t destructiveFg    { 0x7F1D1DFF };       // red-900

// ─── 3b. 内容画布与卡片 ─────────────────────────────────────
inline uint32_t canvas           { 0xF3F3F3FF };       // 窗口底（卡片悬浮其上）
inline uint32_t contentBorder    { 0xEEEEEEFF };       // 内容卡片描边
inline constexpr float contentCardRadius   { 16.f };   // 内容卡片大圆角
inline constexpr float cardOutset          { 14.f };   // 卡片四周悬浮留白（露出画布）
inline uint32_t toggleTrackOff   { 0xE5E5E5FF };       // 开关：关（比画布略深）
inline uint32_t toggleTrackOn    { 0x0FDC78FF };       // 开关：开

// ─── 4. 兼容旧代码的别名（apply() 里一并同步）──────────────
inline uint32_t windowBg         { background };
inline uint32_t sideBg           { sidebar };
inline uint32_t sideHover        { sidebarAccent };
inline uint32_t sideSelected     { sidebarSelected };
inline uint32_t sideBorder       { sidebarBorder };
inline uint32_t accentFg         { primaryForeground };
inline uint32_t brand            { success };
inline uint32_t textPrimary      { foreground };
inline uint32_t textSecondary    { mutedForeground };
inline uint32_t textMuted        { mutedForeground };
inline uint32_t textTertiary     { faintForeground };
inline uint32_t rowBg            { secondary };
inline uint32_t cardBg           { card };
inline uint32_t panel            { card };
inline uint32_t inputBg          { card };
inline uint32_t rowDivider       { border };

// ─── 5. 圆角 shadcn --radius：sm / md / lg / xl ────────────
inline constexpr float radiusXs  { 10.f };  // 设置窗内圆角统一 10（胶囊/圆形另走 radiusFull）
inline constexpr float radiusSm  { 10.f };  // 同上
inline constexpr float radiusMd  { 10.f };  // 同上
inline constexpr float radiusLg  { 10.f };  // 同上
inline constexpr float radiusXl  { 10.f };  // 同上
inline constexpr float radiusFull{ 999.f }; // 胶囊 / 圆形（pill、徽章、圆形按钮）
inline constexpr float radiusInner{ 6.f };  // 大框套小框：内层小框的圆角 = 外层圆角(10) − 内边距(4)
inline constexpr float radiusCtl  { 8.f };  // 选项行内控件（下拉/输入框/按钮）的圆角
inline constexpr float radiusRow  { radiusCtl + 6.f };  // 选项行外框：= 控件圆角 + 行内边距(6)，与控件同心（14）
inline constexpr float radius    { radiusMd };

// ─── 6. 间距 · 4px 阶梯（shadcn p-1=4 ... p-8=32）──────────
inline constexpr float sp1  {  4.f };
inline constexpr float sp2  {  8.f };
inline constexpr float sp3  { 12.f };
inline constexpr float sp4  { 16.f };
inline constexpr float sp5  { 20.f };
inline constexpr float sp6  { 24.f };
inline constexpr float sp7  { 28.f };
inline constexpr float sp8  { 32.f };
inline constexpr float sp10 { 40.f };
inline constexpr float sp12 { 48.f };

// ─── 7. 字号 · shadcn typography（无粗体，靠阶梯建立层级）──
inline constexpr float fontXs   { 11.f };
inline constexpr float fontSm   { 12.f };
inline constexpr float fontBase { 13.f };
inline constexpr float fontLg   { 14.f };
inline constexpr float fontXl   { 16.f };
inline constexpr float font2Xl  { 18.f };
inline constexpr float font3Xl  { 20.f };

// ─── 8. 布局 · 设置窗内部尺寸 ──────────────────────────────
inline constexpr float sideWidth       { 200.f };  // 侧栏宽
inline constexpr float sidePadX        {  18.f };  // 侧栏左右 padding
inline constexpr float sidePadY        {  18.f };  // 侧栏上下 padding
inline constexpr float menuItemH       {  36.f };  // 侧栏菜单项高
inline constexpr float menuGap         {   4.f };  // 菜单项间距
inline constexpr float contentPadX     {  20.f };  // 内容区左右 padding
inline constexpr float contentPadY     {  20.f };  // 内容区上 padding
inline constexpr float contentPadYBottom{ 20.f };  // 内容区下 padding
inline constexpr float rowHeight       {  52.f };  // 含描述的选项行高
inline constexpr float rowHeightCompact{  44.f };  // 单行标签 + 控件
inline constexpr float hotkeyRowH      {  50.f };  // 热键设置卡片行高
inline constexpr float gridItemH       { rowHeightCompact }; // 双栏选项卡片高
inline constexpr float dropdownWidth   { 140.f };  // 下拉栏(触发按钮)与下拉浮层的统一宽度
inline constexpr float cardPadX        {  20.f };  // Card 左右内边距
inline constexpr float cardPadY        {  16.f };  // Card 上下内边距
inline constexpr float cardHeaderPadY  {  12.f };  // Card header 上下内边距

// gap：section 之间 / block 之间 / option 之间
inline constexpr float sectionGap      {  24.f };
inline constexpr float blockGap        {  16.f };
inline constexpr float optionGap       {  12.f };  // 行与行间距（紧凑）
inline constexpr float sectionHeaderBottom { 16.f };
inline constexpr float titleDescGap    {   4.f };

// ─── 9. 控件 · 通用高度 ────────────────────────────────────
inline constexpr float ctrlH  { 32.f };   // 默认（button / input / select）
inline constexpr float ctrlSm { 28.f };   // 小（pill / 次级按钮）
inline constexpr float ctrlLg { 40.f };   // 大

// ─── 10. 阴影与深度（用于弹层、卡片 hover）──────────────────
inline constexpr uint32_t shadowSmColor  { 0x0000001A }; // 10% black
inline constexpr uint32_t shadowMdColor  { 0x00000026 }; // 15% black
inline constexpr float shadowOffsetY     { 4.f };
inline constexpr float shadowBlur        { 12.f };

// ─── 11. 焦点环（Focus ring）────────────────────────────────
inline constexpr float ringWidth         { 2.f };
inline uint32_t ringColor         { zinc950 };

// ============================================================
//  主题切换
// ============================================================
inline bool g_dark{ false };
inline bool isDark() { return g_dark; }

// 系统是否为浅色（AppsUseLightTheme != 0）
inline bool sysIsLight()
{
	DWORD light = 1;
	HKEY hKey = nullptr;
	if (RegOpenKeyExW(HKEY_CURRENT_USER,
		L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
		0, KEY_READ, &hKey) == ERROR_SUCCESS) {
		DWORD size = sizeof(DWORD);
		RegQueryValueExW(hKey, L"AppsUseLightTheme", nullptr, nullptr,
			reinterpret_cast<LPBYTE>(&light), &size);
		RegCloseKey(hKey);
	}
	return light != 0;
}

// 浅色：与既有观感完全一致
inline void applyLight()
{
	g_dark = false;
	background = 0xFFFFFFFF; foreground = zinc950;
	card = 0xFFFFFFFF; cardForeground = zinc950;
	popover = 0xFFFFFFFF; popoverForeground = zinc950;
	popupSelBg = 0xF5F4F4FF; popupFg = zinc950;
	primary = zinc900; primaryForeground = zinc50; primaryHover = zinc800;
	secondary = zinc100; secondaryForeground = zinc950;
	muted = zinc100; mutedForeground = zinc500;
	accent = zinc100; accentForeground = zinc950;
	destructive = 0xEF4444FF; destructiveForeground = zinc50;
	border = zinc200; input = zinc200; ring = zinc950;
	sidebar = 0xFFFFFFFF; sidebarForeground = zinc950; sidebarPrimary = zinc950;
	sidebarAccent = 0xE5E5E5FF; sidebarSelected = 0xE5E5E5FF; sidebarSelectedFg = zinc950;
	sidebarBorder = zinc200; sidebarRing = zinc950;
	faintForeground = zinc400; disabledForeground = zinc400; placeholder = zinc500;
	success = 0x5EC322FF; successBg = 0xF0FDF4FF; successBorder = 0x86EFACFF; successFg = 0x14532DFF;
	warningBg = 0xFFFEF0FF; warningBorder = 0xFCD34DFF; warningFg = 0x713F12FF;
	infoBg = 0xEFF6FFFF; infoBorder = 0x93C5FDFF; infoFg = 0x1E3A8AFF;
	destructiveBg = 0xFEF2F2FF; destructiveBorder = 0xFECACAFF; destructiveFg = 0x7F1D1DFF;
	canvas = 0xF3F3F3FF; contentBorder = 0xEEEEEEFF;
	toggleTrackOff = 0xE5E5E5FF; toggleTrackOn = 0x0FDC78FF;
	ringColor = zinc950;
	// 别名
	windowBg = background; sideBg = sidebar; sideHover = sidebarAccent; sideSelected = sidebarSelected;
	sideBorder = sidebarBorder; accentFg = primaryForeground; brand = success;
	textPrimary = foreground; textSecondary = mutedForeground; textMuted = mutedForeground;
	textTertiary = faintForeground; rowBg = secondary; cardBg = card; panel = card;
	inputBg = card; rowDivider = border;
}

// 深色：以「内容底 #171717 / 菜单底 #262626 / 菜单停悬·选中 #424242」三个锚点铺开，
// 其余语义色按深色场景配套（描边提亮、文字反相、状态色换成深底浅字）。
inline void applyDark()
{
	g_dark = true;
	canvas = 0x262626FF;                 // 窗口最外层底（含左侧菜单那条）；内容卡片悬浮其上
	background = 0x171717FF;             // 内容区底（锚点）
	card = 0x171717FF;                   // 卡片与内容同底，靠描边区分（与浅色白上白一致）
	cardForeground = 0xFAFAFAFF;
	popover = 0x262626FF;                // 浮层（下拉/气泡）底（锚点）
	popoverForeground = 0xFAFAFAFF;
	popupSelBg = 0x424242FF;             // 下拉当前项 / 悬停项（锚点）
	popupFg = 0xFFFFFFFF;                // 下拉文字：恒为纯白
	foreground = 0xFAFAFAFF;
	primary = 0xFAFAFAFF;                // 主按钮：浅底深字（与浅色反相）
	primaryForeground = 0x171717FF;
	primaryHover = 0xE5E5E5FF;
	secondary = 0x262626FF; secondaryForeground = 0xFAFAFAFF;
	muted = 0x262626FF; mutedForeground = 0xA3A3A3FF;
	accent = 0x262626FF; accentForeground = 0xFAFAFAFF;
	destructive = 0xEF4444FF; destructiveForeground = 0xFAFAFAFF;
	border = 0x222222FF; input = 0x222222FF; ring = 0xA3A3A3FF;
	sidebar = 0x262626FF;                // 菜单底（锚点）
	sidebarForeground = 0xFAFAFAFF; sidebarPrimary = 0xFAFAFAFF;
	sidebarAccent = 0x424242FF;          // 菜单停悬（锚点）
	sidebarSelected = 0x424242FF;        // 菜单选中：与停悬同色（锚点）
	sidebarSelectedFg = 0xFAFAFAFF;      // 深底上配浅字
	sidebarBorder = 0x333333FF; sidebarRing = 0xA3A3A3FF;
	faintForeground = 0x737373FF; disabledForeground = 0x737373FF; placeholder = 0x737373FF;
	success = 0x4ADE80FF; successBg = 0x14311FFF; successBorder = 0x1F6B3AFF; successFg = 0x86EFACFF;
	warningBg = 0x3A2E12FF; warningBorder = 0x8A6D1FFF; warningFg = 0xFDE68AFF;
	infoBg = 0x16233AFF; infoBorder = 0x2E4A73FF; infoFg = 0x93C5FDFF;
	destructiveBg = 0x3A1A1AFF; destructiveBorder = 0x7F2A2AFF; destructiveFg = 0xFCA5A5FF;
	contentBorder = 0x262626FF;
	toggleTrackOff = 0x424242FF; toggleTrackOn = 0x0FDC78FF;
	ringColor = 0xD4D4D8FF;
	// 别名
	windowBg = background; sideBg = sidebar; sideHover = sidebarAccent; sideSelected = sidebarSelected;
	sideBorder = sidebarBorder; accentFg = primaryForeground; brand = success;
	textPrimary = foreground; textSecondary = mutedForeground; textMuted = mutedForeground;
	textTertiary = faintForeground; rowBg = secondary; cardBg = card; panel = card;
	inputBg = card; rowDivider = border;
}

// mode：0 跟随系统 / 1 浅色 / 2 深色
inline void apply(int mode)
{
	const bool dark = (mode == 2) ? true : (mode == 0 ? !sysIsLight() : false);
	if (dark) applyDark(); else applyLight();
}

} // namespace SettingTheme
