#pragma once
#include <include/Ling.h>
#include "SettingTheme.h"
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// ============================================================================
//  SettingUi 构件库 · shadcn/ui 风格（严格对齐 shadcn 组件文档）
//
//  涵盖组件：
//    sectionHeader / groupCard        → 页面级标题 + 分组卡片
//    optionRow / descRow / toggleRow / selectRow / textRow
//                                    → shadcn 通用 Settings Row（label + 可选 desc + 控件）
//    inputField / textareaField       → Input / Textarea
//    subTabs                           → Tabs（shadcn TabsList + TabsTrigger）
//    card / separator                  → Card / 水平分割线
//    pill                              → Badge（variant: default / outline / secondary）
//    dashedAdd                         → dashed 幽灵按钮 "＋ 添加项"
//    btnPrimary / btnOutline / btnGhost / btnDestructive → shadcn Button 四变体
//    callout                           → shadcn Callout 提示卡
//    closePopup / hasPopup             → 全局下拉浮层管理
// ============================================================================

namespace SettingUi {

// 版本标记：免费版在版本号后面带「Lite」，与将来的专业版区分。改这一处，全局生效。
// 只影响「显示用」的版本串（设置页左下角 / 关于页）；exe 的数字版本(0.1.0.0)不带它，更新器要靠它比对。
inline constexpr const wchar_t* kEditionTag{ L" - Lite beta" };

// ─── 浮层管理（所有下拉/弹层共享同一个 gPopup，互斥显示） ──────────────
void closePopup(Ling::WinBase* win);
bool hasPopup();

// ─── 可见坐标换算：返回 node 当前在窗口中的可见原点（逻辑像素）────────
//   ScrollerBox 滚动只平移 content 视觉偏移、不改子节点布局坐标，故 node->x/y 是
//   "未滚动"的窗口坐标。本函数沿 parent 链累加祖先 ScrollerBox 的 scrollY，
//   换算回按钮当前显示位置，供 popup 等浮层绝对定位（滚动后仍对齐按钮）。
std::pair<float, float> visiblePos(Ling::Node* node);

// ─── 滚动跟随：让挂在 win->body 上的浮层随内容滚动自动重新锚定 ──────────
//   ScrollerBox 滚动只平移 content 视觉、不触发子节点重排，因此浮层一旦按
//   "打开时"的可见位置定位，滚动内容后就会停留原地（下拉项漂移缺陷）。
//   本类在浮层存活期间监听滚轮/拖动滚动条，每次位置变化时调用 compute()
//   （须基于 visiblePos 的当前值）重算期望坐标并重定位；位置未变则跳过，
//   避免无谓重排。浮层关闭时析构本对象即可移除监听。
class ScrollFollower {
public:
    // compute(): 返回浮层期望的窗口逻辑坐标 {left, top}，随滚动应调用 visiblePos 实时计算。
    ScrollFollower(Ling::WinBase* win,
        Ling::ScrollerBox* popup,
        std::function<std::pair<float, float>()> compute);
    ~ScrollFollower();
    ScrollFollower(const ScrollFollower&) = delete;
    ScrollFollower& operator=(const ScrollFollower&) = delete;
    // 立即按 compute() 定位（打开浮层时调用一次）。
    void place();
    float left{ 0.f }, top{ 0.f };   // 最近一次定位坐标（便于外部读取）
private:
    Ling::WinBase* win;
    Ling::ScrollerBox* popup;
    std::function<std::pair<float, float>()> compute;
    winrt::event_token wheelTok{}, moveTok{};
    float lastLeft{ -1.f }, lastTop{ -1.f };
};

// ─── 独立浮层窗口：下拉/弹层用独立 WS_POPUP 顶层窗口承载 ──────────────
//   原实现把 ScrollerBox 挂到宿主 win->body 下，会被窗口客户区裁剪（下拉项靠近
//   底部时被迫上拉、且内容被截断）。改用独立顶层窗口后不再受宿主客户区裁剪，
//   可正常超出界面显示、完整展示所有选项。生命周期由持有者负责（全局 gPopup 或
//   页面成员 unique_ptr），关闭需先 close()（销毁 HWND）再 delete；析构内自动完成。
class Popup : public Ling::WinBase {
public:
    // host：宿主设置窗口；pw/totalH：浮层逻辑尺寸（body 物理尺寸响应窗口缩放）。
    Popup(Ling::WinBase* host, float pw, float totalH);
    ~Popup();
    // 点击宿主窗口（弹层之外）→ 关闭自身。持有者应把它设为销毁本弹层的回调
    // （全局 gPopup 设为 destroyPopup，页面成员弹层设为 reset 对应 unique_ptr）。
    std::function<void()> onDismiss;
    // 内容容器：调用方往里面挂子节点（选项列表 / 色板 / 自定义面板）。
    Ling::ScrollerBox* box() const { return listBox; }
    // 宿主窗口（供 closePopup(host) 判断归属）。
    Ling::WinBase* hostWindow() const { return host; }
    // 指定锚定按钮：place() 会把浮层定到按钮正下方（不因靠近底部而上拉）。
    void setAnchor(Ling::Button* a) { anchor = a; }
    // 顶部吸附模式：浮层顶到锚定按钮上缘（与按钮同宽、同高起算）把按钮盖住，与按钮连成一整块面板
    // （下拉"一体展开"效果）；不再做靠近屏幕底部的向上翻转。
    void setAttachTop(bool on) { attachTop = on; }
    // 吸附模式下把浮层顶部 px（物理像素，= 被盖住的触发框高度）对命中测试设为透明：
    //  该区域的点击直接落到宿主窗口的触发框上 → "再点一次触发框收回"的行为与未展开时完全一致
    //  （否则点击落在浮层窗口自身，交互/激活路径跟点选项时不同）。
    void setHitTransparentTop(float px) { hitTransparentTop = px; }
    // 当前锚定按钮（供绑定方做"点击同一触发按钮即开关"的切换判断）。
    Ling::Button* anchorButton() const { return anchor; }
    // 指定触发按钮：默认即 anchor。当"触发下拉的按钮"与"视觉锚定按钮"不一致时
    // （如 Tab ▾ 触发、菜单却左对齐胶囊整体），须分别设置：anchor 决定浮层位置，
    // trigger 让全局钩子识别"点击触发按钮不算点击外部"，从而把开关切换留给按钮 onClick。
    void setTrigger(Ling::Button* t) { trigger = t; }
    // 按 anchor 当前可见位置重定位（滚动宿主内容时调用，屏幕坐标）。
    void place();
    // 完成 setSize + place + show + refresh 的打开流程。
    void open();
protected:
    void onCreated() override;
    LRESULT onHitTest(const POINT pos) override;
private:
    void removeHostHooks();
    void installHook();
    void uninstallHook();
    // 全局低级鼠标钩子：点击浮层窗口之外的任何位置（其它软件/宿主空白/桌面）都关闭浮层。
    // 不依赖宿主是否持有焦点/激活，是"点击外部即关闭"的确定性实现（下拉互斥，同一时刻仅一个）。
    static Popup* sOpen;
    static LRESULT CALLBACK lowLevelProc(int code, WPARAM wParam, LPARAM lParam);
    HHOOK mouseHook{ nullptr };
    Ling::WinBase* host;
    Ling::Button*  anchor{ nullptr };
    Ling::Button*  trigger{ nullptr };
    bool attachTop{ false };   // true：顶到锚定按钮上缘（盖住按钮，合为一整块面板）
    float hitTransparentTop{ 0.f };   // >0：该高度（物理像素）内对命中测试返回 HTTRANSPARENT
    Ling::ScrollerBox* listBox{ nullptr };
    float pw, totalH;
    winrt::event_token mouseTok{}, wheelTok{};
};

// ─── HoverTip：悬停气泡（复用工具栏停悬气泡外壳：圆角矩形 + 底部小三角）───
//   独立 WS_POPUP 顶层窗口承载，不走系统 tooltip（没有等待延迟/淡入淡出）：
//   鼠标进入控件立刻显示、离开立刻消失；文本过长时自动折行，气泡随内容自适应。
//   小三角指向锚定控件上缘中点；上方放不下时自动翻到控件下方（三角朝上）。
class HoverTip : public Ling::WinBase
{
public:
    // host：锚定控件所在的宿主窗口（坐标换算与滚动跟随都基于它）。
    explicit HoverTip(Ling::WinBase* host);
    ~HoverTip();
    // 绑定到按钮：进入即弹、离开即收。气泡在按钮上方，小三角朝下指向按钮。
    // 一个实例可绑定任意多个按钮（当前锚点/文本在每次悬停时确定）。
    void bind(Ling::Button* btn, const std::wstring& text);
    // 手动指定锚点与文本显示（供自定义悬停场景，如文件输出预览）；owner 可为任意 Node。
    void showFor(Ling::Node* owner, const std::wstring& text);
    // 仅当当前锚点就是 owner 时收起（避免误关别人的气泡）。
    void hideFor(Ling::Node* owner);
    // 立刻收起（未显示时是空操作）。
    void hideTip();
private:
    void onCreated() override;
    void layout() override;
    // 量出文本占位 → 定窗口逻辑尺寸 → 定位 → 显示。
    void showNow();
    // 按锚定控件当前可见位置重定位（屏幕物理坐标；含上下翻转判断）。
    void place();
private:
    Ling::WinBase* host{ nullptr };
    Ling::Node* anchor{ nullptr };
    Ling::Node* content{ nullptr };
    Ling::Label* label{ nullptr };
    Ling::Canvas* canvas{ nullptr };
    std::wstring text;
    float arrowX{ 0.f };     // 小三角在窗口内的物理横坐标
    bool tipDown{ true };    // true：气泡在锚定控件上方、三角朝下
    bool isVisible{ false };
    winrt::event_token moveTok{};
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushBg;
};

// ─── ConfirmDialog：确认弹窗（压暗底 + 居中卡片 +
//   「取消 / 确定」；配色走本工程主题）────────────────────────────
//   独立顶层窗口：几何 == 宿主窗口客户区（不含边框），因此天然盖住宿主、也把点击挡在弹窗外
//   （宿主上的按钮不会跟着被点到）。异步接口：确定/取消后回调（true=确定）。
//   同一时刻只允许一个；Esc / 点卡片外的压暗区域 = 取消。
class ConfirmDialog : public Ling::WinBase {
public:
    static void ask(Ling::WinBase* host, const std::wstring& message,
        std::function<void(bool)> onResult,
        std::wstring confirmText = L"", std::wstring cancelText = L"");
    static bool isOpen();
    ~ConfirmDialog();
protected:
    void onCreated() override;
    void layout() override;
private:
    ConfirmDialog(Ling::WinBase* host, std::wstring message,
        std::wstring confirmText, std::wstring cancelText, std::function<void(bool)> onResult);
    void placeOverHost();
    // 抓宿主客户区 → 降采样再放大 = 廉价模糊，作为压暗底的「毛玻璃」背景
    void prepareBackdrop();
    void finish(bool ok);
private:
    Ling::WinBase* host{ nullptr };
    std::wstring message, confirmText, cancelText;
    std::function<void(bool)> onResult;
    Ling::Node* card{ nullptr };
    Ling::Canvas* blurCanvas{ nullptr };                       // 自绘模糊背景
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> blurBmp;
    bool done{ false };
    winrt::event_token downTok{}, keyTok{}, hostDestroyTok{};
    static ConfirmDialog* s_open;
};

// ─── 挂接裸节点（等价于 makeChild<T> 的后半段）。
//   用途：当外部 `new T(win)` 得到一个 Node*，但因模板约束无法走 Node::makeChild<T>()
//   时，调用 adoptChild(parent, child) 完成父-子挂接 + 所有权转移。
void adoptChild(Ling::Node* parent, Ling::Node* child);

// ─── Section Header：页面标题 + 描述 ────────────────────────────────────
//   shadcn: PageHeader → h1(22/28 bold) + p(muted)
Ling::Node* sectionHeader(Ling::Node* parent,
    const std::wstring& title,
    const std::wstring& desc = L"",
    const std::wstring& tip = L"");

// ─── Group Card：shadcn Card 分组容器（带可选 cardHeader / cardContent）──
//   白底 + zinc-200 描边 + radiusLg(10px) + 16px pad
Ling::Node* groupCard(Ling::Node* parent,
    const std::wstring& cardTitle = L"",
    const std::wstring& cardDesc  = L"");

// ─── Group Label：card 内分组小标题（粗 13 zinc-900 + marginTop）───────
Ling::Label* groupLabel(Ling::Node* parent, const std::wstring& text);
//   带说明的版本：标题右侧多一个「?」，悬停显示 tip
Ling::Label* groupLabel(Ling::Node* parent, const std::wstring& text, const std::wstring& tip);

// ─── 功能提示（问号说明）───────────────────────────────────────────────
//   设置项旁的「?」：悬停出工具栏那套停悬气泡（HoverTip）。由设置窗口在 onCreated 里
//   把它的 HoverTip 实例注册进来（HoverTip 需要宿主 hwnd，只能在窗口创建后构造）。
void attachHelpTip(HoverTip* tip);
//   造一个「?」按钮：parent 通常是选项标题所在的行容器。文本为空 / 总开关关着时返回 nullptr。
Ling::Button* helpTipButton(Ling::Node* parent, const std::wstring& text);
//   取当前注册的说明气泡（未注册返回 nullptr）；供子页做自定义悬停提示（如文件输出预览）。
HoverTip* helpTip();

// ─── Separator：水平 1px zinc-200 分割线 ────────────────────────────────
Ling::Node* separator(Ling::Node* parent, float marginTop = 12.f, float marginBottom = 12.f);

// ─── 末尾留白修剪：沿"视觉最底边"逐层清空最后一个子节点的底部 margin ──
//   各构件（card/groupCard/optionRow/grid2Cells/dashedAdd 等）用 marginBottom 做元素
//   之间的间距，但页面最后一个元素会额外把内容区底部垫高（20+12/16），与左右间距(20)不一致。
//   页面构建完成后调用本函数，把最底边链上的每个末尾 margin 归零，让底部留白只由外层
//   content 的 contentPadYBottom(20) 提供。对带 Tab 的页面需逐 tab 调用。
void trimTrailingGap(Ling::Node* content);

// ─── Option Row：通用行容器（左 label/desc 区 + 右控件区） ──────────────
//   chrome=false → 纯行（无卡片背景/描边），放在 groupCard 里与 separator 搭配
//   chrome=true  → 独立行（白底 + 描边 + radiusMd）
//   结构：Row[ LabelBox[ Label / Desc(可选) ], ControlBox[ 控件 ] ]
struct OptionRowRef {
    Ling::Node* row;        // 整行
    Ling::Node* labelBox;   // 左 label 区（column）
    Ling::Node* ctrlBox;    // 右控件区（row, right align）
    Ling::Label* label;     // 主标题
    Ling::Label* desc;      // 副标题（可能为nullptr）
    // 便捷访问：允许 auto r = optionRow(p); r->makeChild<T>() 直接把自定义子节点挂到 row 上（与 labelBox/ctrlBox 同级，flex=row）。
    Ling::Node* operator->() const noexcept { return row; }
};
OptionRowRef optionRow(Ling::Node* parent,
    const std::wstring& label = L"",
    const std::wstring& desc  = L"",
    bool chrome = false,
    float ctrlH = SettingTheme::ctrlH);   // 行内控件实际高度：决定右侧内边距（与控件上下留白取齐）
// 简易版：只返回 row Node*，不关心 label/desc。适用于「row 内部自由布局」场景（子页自行 makeChild）。
inline Ling::Node* optionRowSimple(Ling::Node* parent, bool chrome = false)
{
    auto r = optionRow(parent, L"", L"", chrome);
    return r.row;
}

// ─── 双栏网格容器：把多个 optionRow/toggleRow/selectRow 以 2 列排布 ───
//   视觉：每个选项 = 独立白底描边圆角卡片，两两一行（shadcn grid-cols-2）。
//   用法：auto* g = SettingUi::grid2(p); 然后往 g 里连续 add toggleRow/selectRow(..., chrome=true)，
//   最后调用 SettingUi::grid2Cells(g)。grid 为 Column，grid2Cells 把子行按两两一组塞进
//   独立 Row，行内两列用 flexGrow(1)+widthPercent(0) 严格等分剩余空间，右列右缘恰好贴
//   行右缘，因此双栏总占宽（两列 + 列间距）与单栏 100% 宽度精确一致。
Ling::Node* grid2(Ling::Node* parent);
void grid2Cells(Ling::Node* grid);

// ─── Toggle Row：Switch 行 ──────────────────────────────────────────────
//   重载 1（详尽）: toggleRow(parent, label, desc, on, onChange [, chrome])
Ling::Button* toggleRow(Ling::Node* parent,
    const std::wstring& label,
    const std::wstring& desc,
    bool on,
    std::function<void(bool)> onChange,
    bool chrome = false);
//   重载 2（简明，无 desc）: toggleRow(parent, label, on, onChange)
inline Ling::Button* toggleRow(Ling::Node* parent,
    const std::wstring& label,
    bool on,
    std::function<void(bool)> onChange)
{
    return toggleRow(parent, label, L"", on, std::move(onChange), false);
}
//   重载 3（带 chrome，无 desc）: toggleRow(parent, label, on, onChange, chrome)
inline Ling::Button* toggleRow(Ling::Node* parent,
    const std::wstring& label,
    bool on,
    std::function<void(bool)> onChange,
    bool chrome)
{
    return toggleRow(parent, label, L"", on, std::move(onChange), chrome);
}
void styleToggle(Ling::Button* btn, bool on);

// ─── Select Row：Dropdown Select 行 ─────────────────────────────────────
//   重载 1（详尽）: selectRow(parent, label, desc, options, current, onChange [, chrome])
Ling::Button* selectRow(Ling::Node* parent,
    const std::wstring& label,
    const std::wstring& desc,
    const std::vector<std::wstring>& options,
    int current,
    std::function<void(int, const std::wstring&)> onChange,
    bool chrome = false);
//   重载 2（简明，无 desc）: selectRow(parent, label, options, current, onChange)
inline Ling::Button* selectRow(Ling::Node* parent,
    const std::wstring& label,
    const std::vector<std::wstring>& options,
    int current,
    std::function<void(int, const std::wstring&)> onChange)
{
    return selectRow(parent, label, L"", options, current, std::move(onChange), false);
}
//   重载 3（简明 + chrome）: selectRow(parent, label, options, current, onChange, chrome)
inline Ling::Button* selectRow(Ling::Node* parent,
    const std::wstring& label,
    const std::vector<std::wstring>& options,
    int current,
    std::function<void(int, const std::wstring&)> onChange,
    bool chrome)
{
    return selectRow(parent, label, L"", options, current, std::move(onChange), chrome);
}
// 下拉按钮本体（不包含行容器/标签，用于自定义布局）
Ling::Button* selectDdl(Ling::Node* parent,
    const std::vector<std::wstring>& options,
    int current,
    std::function<void(int, const std::wstring&)> onChange);

// ─── Text Row：单行 Input 行 ────────────────────────────────────────────
//   重载 1（详尽）: textRow(parent, label, desc, value, placeholder, onChange [, chrome])
Ling::TextBox* textRow(Ling::Node* parent,
    const std::wstring& label,
    const std::wstring& desc,
    const std::wstring& value,
    const std::wstring& placeholder,
    std::function<void(const std::wstring&)> onCommit,
    bool chrome = false);
//   重载 2（简明，placeholder 作为 value 后的描述字段）: textRow(parent, label, hint, value, onChange)
inline Ling::TextBox* textRow(Ling::Node* parent,
    const std::wstring& label,
    const std::wstring& hint,
    const std::wstring& value,
    std::function<void(const std::wstring&)> onCommit)
{
    return textRow(parent, label, hint, value, L"", std::move(onCommit), false);
}

// ─── Textarea Field（多行 Prompt 大文本框，独立 block）─────────────────
Ling::TextBox* textareaField(Ling::Node* parent,
    const std::wstring& label,
    const std::wstring& hint,
    const std::wstring& value,
    std::function<void(const std::wstring&)> onCommit);

// ─── Input Field（独立文本输入框，不带行容器，带 label 和 hint）────────
Ling::TextBox* inputField(Ling::Node* parent,
    const std::wstring& label,
    const std::wstring& hint,
    const std::wstring& value,
    const std::wstring& placeholder,
    std::function<void(const std::wstring&)> onCommit,
    float height = 0.f);

// ─── Sub Tabs：shadcn Tabs 样式 ────────────────────────────────────────
//   variant="solid"（默认）: active = primary(黑底白字) / inactive = white+border
//   支持 onRight 回调（行尾自定义操作按钮区，右侧对齐）
std::vector<Ling::Button*> subTabs(Ling::Node* parent,
    const std::vector<std::wstring>& labels,
    int current,
    std::function<void(int)> onChange,
    std::function<void(Ling::Node* row)> onRight = nullptr);
void styleSubTab(Ling::Button* btn, bool on);

// ─── Segmented Pills：分段胶囊（选中段高亮卡片，自适应宽度）──────────────
//   视觉：轨道 secondary(zinc100) + 常规圆角(radiusLg)；每段按内容自适应宽度。
//   选中段：primary(黑) 背景 + primaryForeground 文字；未选中：透明 + textPrimary。
//   交互：点击段落 → 即时切换选中段（无定时器/绝对定位滑动 → 无卡死与滑块丢失问题）。
//   注意：返回的 ref 需由调用方持有以维持状态存活；select() 仅供外部程序化同步。
struct SegmentedPillsRef {
    Ling::Node* row{ nullptr };               // 轨道容器
    std::vector<Ling::Button*> btns;          // 分段按钮
    std::shared_ptr<void> state;              // 内部状态（保持存活）
    std::function<void(int)> select;          // 程序化切换选中段（不触发 onChange，用于外部同步）
    Ling::Node* operator->() const noexcept { return row; }
};
SegmentedPillsRef segmentedPills(Ling::Node* parent,
    const std::vector<std::wstring>& labels,
    int current,
    std::function<void(int)> onChange);

// ─── Card：通用白底卡片容器（不带 header，纯 content box）───────────────
Ling::Node* card(Ling::Node* parent);

// ─── Pill / Badge：标签胶囊（selected=default / unselected=outline）─────
Ling::Button* pill(Ling::Node* row, const std::wstring& label, bool selected);
void stylePill(Ling::Button* btn, bool selected);

// ─── Dashed Add：虚线添加按钮 ────────────────────────────────────────────
Ling::Button* dashedAdd(Ling::Node* parent, const std::wstring& label,
    std::function<void()> onClick);

// ─── shadcn Button 四大变体（直接创建按钮） ─────────────────────────────
//   Primary: 黑底白字（zinc-900 / zinc-50）
//   Outline: 白底 + border(zinc-200) + 黑字，hover→zinc-100
//   Ghost:   无背无边，黑字，hover→zinc-100
//   Destructive: red-500底白字，hover→red-600
Ling::Button* btnPrimary(Ling::Node* parent, const std::wstring& text,
    std::function<void()> onClick, float w = 0.f);
Ling::Button* btnOutline(Ling::Node* parent, const std::wstring& text,
    std::function<void()> onClick, float w = 0.f);
Ling::Button* btnGhost(Ling::Node* parent, const std::wstring& text,
    std::function<void()> onClick, float w = 0.f);
Ling::Button* btnDestructive(Ling::Node* parent, const std::wstring& text,
    std::function<void()> onClick, float w = 0.f);

// ─── Callout：提示卡（shadcn Callout variant=default/green/red/yellow）─
enum class CalloutVariant { Info, Success, Warning, Destructive };
Ling::Node* callout(Ling::Node* parent,
    const std::wstring& message,
    CalloutVariant variant = CalloutVariant::Info,
    const std::wstring& title = L"");

} // namespace SettingUi
