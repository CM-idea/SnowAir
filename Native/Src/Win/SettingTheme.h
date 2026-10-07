#pragma once
#include <cstdint>

// ============================================================
//  shadcn/ui · Default (Zinc · Light) 完整设计 Token
//  参考：https://ui.shadcn.com/docs/components
//  颜色存储：AABBGGRR（D2D / COLORREF 小端格式，AA=FF 为不透明）
//  设计原则：
//    1. 严格对齐 shadcn 的 CSS 变量语义。
//    2. 暴露完整 Zinc 50-950 色阶，方便组件按语义取用。
//    3. 字号不使用粗体（Ling::Label 暂不支持），通过字号阶梯 + 颜色对比建立层级。
//    4. 所有圆角、间距、控件高度均走 4px 网格。
// ============================================================
namespace SettingTheme {

// ─── 0. Zinc 完整色阶（50 ~ 950）────────────────────────────
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

// ─── 1. 颜色 · shadcn CSS 变量 1:1 对应 ────────────────────
inline constexpr uint32_t background       { 0xFFFFFFFF };       // white
inline constexpr uint32_t foreground       { zinc950 };          // 主文字
inline constexpr uint32_t card             { 0xFFFFFFFF };       // white
inline constexpr uint32_t cardForeground   { zinc950 };
inline constexpr uint32_t popover          { 0xFFFFFFFF };       // white
inline constexpr uint32_t popoverForeground{ zinc950 };
inline constexpr uint32_t primary          { zinc900 };          // zinc-900 按钮/强调
inline constexpr uint32_t primaryForeground{ zinc50 };           // zinc-50
inline constexpr uint32_t secondary        { zinc100 };          // zinc-100
inline constexpr uint32_t secondaryForeground{ zinc950 };
inline constexpr uint32_t muted            { zinc100 };
inline constexpr uint32_t mutedForeground  { zinc500 };          // zinc-500 次级文字
inline constexpr uint32_t accent           { zinc100 };          // hover / focus 底色
inline constexpr uint32_t accentForeground { zinc950 };
inline constexpr uint32_t destructive      { 0xEF4444FF };       // red-500（注意：Color 按 RRGGBBAA 解析，勿写反成 AABBGGRR）
inline constexpr uint32_t destructiveForeground{ zinc50 };
inline constexpr uint32_t border           { zinc200 };          // zinc-200
inline constexpr uint32_t input            { zinc200 };          // zinc-200
inline constexpr uint32_t ring             { zinc950 };          // focus ring

// ─── 2. 颜色 · Sidebar（侧栏专用）───────────────────────────
inline constexpr uint32_t sidebar          { 0xFFFFFFFF };
inline constexpr uint32_t sidebarForeground{ zinc950 };
inline constexpr uint32_t sidebarPrimary   { zinc950 };
inline constexpr uint32_t sidebarAccent    { 0xE5E5E5FF };       // hover + 选中 行底色（E5E5E5）
inline constexpr uint32_t sidebarBorder    { zinc200 };          // 右 1px 分割线
inline constexpr uint32_t sidebarRing      { zinc950 };

// ─── 3. 颜色 · 状态与语义扩展 ───────────────────────────────
inline constexpr uint32_t faintForeground  { zinc400 };          // 占位/版本号/次要 meta
inline constexpr uint32_t disabledForeground{ zinc400 };         // 禁用文字
inline constexpr uint32_t placeholder      { zinc500 };          // 输入框占位符
inline constexpr uint32_t success          { 0x5EC322FF };       // green-500
inline constexpr uint32_t successBg        { 0xF0FDF4FF };       // green-50
inline constexpr uint32_t successBorder    { 0x86EFACFF };       // green-300
inline constexpr uint32_t successFg        { 0x14532DFF };       // green-900
inline constexpr uint32_t warningBg        { 0xFFFEF0FF };       // amber-50
inline constexpr uint32_t warningBorder    { 0xFCD34DFF };       // amber-300
inline constexpr uint32_t warningFg        { 0x713F12FF };       // amber-900
inline constexpr uint32_t infoBg           { 0xEFF6FFFF };       // blue-50
inline constexpr uint32_t infoBorder       { 0x93C5FDFF };       // blue-300
inline constexpr uint32_t infoFg           { 0x1E3A8AFF };       // blue-900
inline constexpr uint32_t destructiveBg    { 0xFEF2F2FF };       // red-50
inline constexpr uint32_t destructiveBorder{ 0xFECACAFF };       // red-200
inline constexpr uint32_t destructiveFg    { 0x7F1D1DFF };       // red-900

// ─── 3b. 画布与浮层内容卡片（参考 TRAE Design 工作区）──────────
inline constexpr uint32_t canvas           { 0xF3F3F3FF };       // 内容画布浅灰底（卡片悬浮其上；整体背景 F3F3F3）
inline constexpr uint32_t contentBorder    { 0xEEEEEEFF };       // 内容卡片描边（EEEEEE）
inline constexpr float contentCardRadius   { 16.f };             // 内容卡片大圆角
inline constexpr float cardOutset          { 14.f };             // 卡片四周悬浮留白（露出画布）

// ─── 4. 兼容旧代码的别名 ────────────────────────────────────
inline constexpr uint32_t windowBg       { background };
inline constexpr uint32_t sideBg         { sidebar };
inline constexpr uint32_t sideHover      { sidebarAccent };
inline constexpr uint32_t sideSelected   { sidebarAccent };
inline constexpr uint32_t sideBorder     { sidebarBorder };
inline constexpr uint32_t accentFg       { primaryForeground };
inline constexpr uint32_t brand          { success };
inline constexpr uint32_t textPrimary    { foreground };
inline constexpr uint32_t textSecondary  { mutedForeground };
inline constexpr uint32_t textMuted      { mutedForeground };
inline constexpr uint32_t textTertiary   { faintForeground };
inline constexpr uint32_t rowBg          { secondary };
inline constexpr uint32_t cardBg         { card };
inline constexpr uint32_t panel          { card };
inline constexpr uint32_t inputBg        { card };
inline constexpr uint32_t rowDivider     { border };

// ─── 5. 圆角 shadcn --radius：sm / md / lg / xl ────────────
inline constexpr float radiusXs  { 10.f };  // 设置窗内圆角统一 10（胶囊/圆形另走 radiusFull，内容区背景见 contentCardRadius）
inline constexpr float radiusSm  { 10.f };  // 同上
inline constexpr float radiusMd  { 10.f };  // 同上
inline constexpr float radiusLg  { 10.f };  // 同上
inline constexpr float radiusXl  { 10.f };  // 同上
inline constexpr float radiusFull{ 999.f }; // 胶囊 / 圆形（pill、徽章、圆形按钮）——保持原样不参与统一
inline constexpr float radiusInner{ 6.f };  // 大框套小框：内层小框的圆角 = 外层圆角(10) − 内边距(4)，与外框同心
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
// 使用说明：
//   fontXs   用于版本号、辅助说明；
//   fontSm   用于选项描述、hint；
//   fontBase 用于选项主标签、正文；
//   fontLg   用于侧栏菜单、卡片小标题；
//   fontXl   用于分组卡片标题；
//   font2Xl  用于页面标题；
//   font3Xl  用于一级页面大标题（少用）。
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
inline constexpr float gridItemH       { rowHeightCompact }; // 双栏选项卡片高（与单栏紧凑行等高，保持双栏/单栏一致）
inline constexpr float dropdownWidth   { 140.f };  // 下拉栏(触发按钮)与下拉浮层的统一宽度，改这里即可全局微调
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
inline constexpr uint32_t ringColor      { zinc950 };

} // namespace SettingTheme
