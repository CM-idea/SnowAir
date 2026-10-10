#include "pch.h"
#include "SettingWidgets.h"
#include "SettingTheme.h"
#include "../Tool/IconCodes.h"
#include "../Tool/ToolbarChrome.h"   // 悬停气泡外壳（圆角 + 小三角）复用工具栏那套
#include "../App.h"
#include "../Lang.h"
#include "../Util.h"
#include "../Setting.h"              // 功能提示总开关
#include "../Tip.h"                  // 系统 tooltip（设置项旁的「?」说明）
#include <algorithm>
#include <map>

namespace SettingUi {

// 与 Ling::Node.h 中 friend struct ::SettingUi::NodeFriendAccess 对应：
// 获得 Ling::Node 的 protected setChild / children 访问权限，
// 用于 SettingUi::adoptChild() 把 new T(win) 节点挂到父节点下。
struct NodeFriendAccess {
    static void Adopt(Ling::Node* parent, Ling::Node* child)
    {
        parent->setChild(child);
        parent->children.push_back(std::unique_ptr<Ling::Node>(child));
    }
};

namespace {

std::unique_ptr<SettingUi::Popup> gPopup;

void destroyPopup()
{
    if (!gPopup) return;
    // 延迟销毁：浮窗经常在宿主 onMouseDown 事件的订阅回调中被销毁（点击浮窗外部关闭、
    // 或点击触发按钮切换当前下拉）。若在这里同步 gPopup.reset()，会在宿主事件迭代中途
    // 触发 Popup 析构 → removeHostHooks() → host->onMouseDown.remove(当前正在执行的 token)，
    // 使 winrt::event 的订阅列表迭代器失效，多次重复后崩溃。把所有权移入 pending 推迟到
    // 调度队列再真正析构，既避开事件迭代期间的自我销毁，又不会影响紧随其后创建的新浮窗。
    auto* app = Ling::App::get();
    if (app) {
        auto pending = std::make_shared<std::unique_ptr<SettingUi::Popup>>();
        *pending = std::move(gPopup);
        app->dq.TryEnqueue([pending]() { pending->reset(); });
    } else {
        gPopup.reset();               // 无调度队列时兜底：同步析构（正常路径不会走到）
    }
}

// 展开态箭头用的标签：同一个字形垂直镜像（不换图标，免得字重/尺寸对不上）。
// 注意 Composition 的 CenterPoint 是"视觉局部坐标里的点"（不是 0~1 比例），必须等 layout
// 拿到真实 w/h 之后再设；写成 (0.5, 0.5, 0) 会把整块翻到框外，图标看起来就是"移位/消失"。
struct FlipVLabel : Ling::Label
{
    explicit FlipVLabel(Ling::WinBase* win) : Ling::Label(win) {}
protected:
    void layout() override
    {
        Ling::Label::layout();
        visual.CenterPoint({ w * 0.5f, h * 0.5f, 0.f });
        visual.Scale({ 1.f, -1.f, 1.f });
    }
};

// 为下拉按钮绑定浮层（选项列表 + 回调），复用全局 gPopup 实现互斥显示
// setValue：回写选中文本到触发按钮（按钮文本由子 Label 控制，不能用 setText 覆盖）
void bindPopup(Ling::Button* btn, const std::vector<std::wstring>& options, int current,
               std::function<void(int, const std::wstring&)> onChange,
               std::function<void(const std::wstring&)> setValue = nullptr)
{
    // 选中值放进共享状态：展开时按「最新选中」渲染首行/排除项，选项被选中后即时更新。
    // 否则 current 是绑定那一刻的值，再次展开看到的还是初始默认项（首行恒为默认选项的 BUG）。
    auto sel = std::make_shared<int>(current);
    btn->onClick.add([btn, options, sel, onChange, setValue](Ling::Button*) mutable {
        auto* w = btn->win;
        // 开关切换：若浮层已展开且正好锚定在该按钮上，再次点击仅关闭（toggle 关），不再重开。
        // 注意顺序：全局低级鼠标钩子在消息派发前已先于按钮 onClick 运行，若它在此处关闭了浮层，
        // 这里 gPopup 已为 null，导致"点击第二下仍展开"。因此钩子对点击锚定按钮本身不干预，
        // 由本回调判断是否已展开来切换。
        if (gPopup && gPopup->anchorButton() == btn) {
            btn->setBorderWidth(1.f);   // 恢复触发框描边
            destroyPopup();
            return;
        }
        destroyPopup();
        // 展开期间把触发框自己的描边藏起来：浮层顶部与触发框重合，
        // 两条描边叠在一起会多出一圈边，看着"不对"。关闭时再恢复（见下方各处 destroyPopup）。
        btn->setBorderWidth(0.f);

        // ── 一体面板：浮层顶到触发框上缘并盖住它，面板首行复刻触发框
        //    （同高度/同左右内边距/同字号与箭头），下面直接接选项列表 → 视觉上就是"触发框 + 列表"
        //    合成的一整块圆角面板。
        const float dpi     = (w && w->dpi > 0.f) ? w->dpi : 1.f;
        const float headerH = (btn->h > 0.f) ? btn->h / dpi : 32.f;   // 首行高度严格等于触发框
        const float itemH   = 32.f;
        const float itemGap = 4.f;   // 选项之间 4px 间距
        const float pad     = 4.f;   // 选项区四周内边距

        // 当前值已经显示在首行，列表里不再重复它；因此列表可能为空 → 补一行「无」。
        const int cur = *sel;   // 每次展开都取最新选中项
        std::vector<int> rows;
        for (int i = 0; i < (int)options.size(); i++)
            if (i != cur) rows.push_back(i);
        const float rowN = rows.empty() ? 1.f : (float)rows.size();

        const float contentH = headerH + pad + itemH * rowN + itemGap * std::max(0.f, rowN - 1.f) + pad;
        // 面板高度按内容来（不再固定砍到 280：选项多时会把内容挤出窗口，列表里就冒出滚动条）。
        // 只在"内容确实放不下"时（顶到显示器工作区下沿）才收缩，这时滚动条才是合理的。
        float maxH = 4000.f;
        if (w && w->hwnd) {
            float sy = 0.f;
            for (Ling::Node* p = btn->parent; p; p = p->parent)
                if (auto* sb = dynamic_cast<Ling::ScrollerBox*>(p)) sy += sb->getScrollY();
            POINT tl{ static_cast<LONG>(btn->x), static_cast<LONG>(btn->y - sy) };
            ClientToScreen(w->hwnd, &tl);
            MONITORINFO mi{ sizeof(MONITORINFO) };
            if (HMONITOR mon = MonitorFromPoint(tl, MONITOR_DEFAULTTONEAREST)) {
                GetMonitorInfo(mon, &mi);
                maxH = std::max(120.f, (float)(mi.rcWork.bottom - tl.y) - 4.f * dpi);
            }
        }
        const float totalH = std::min(contentH, maxH);
        // 下拉宽度跟随触发按钮实际宽度（约定：弹层与触发按钮严格等宽），避免"弹出项比下拉栏宽/窄"的错位。
        // 触发按钮未布局出宽度时才退回 dropdownWidth。btn->w 为物理像素，除以 dpi 换算回逻辑尺寸。
        float pw = SettingTheme::dropdownWidth;
        if (btn->w > 0.f && w && w->dpi > 0.f)
            pw = btn->w / w->dpi;

        // 1px 描边不用圆角节点直接 setBorder：描边外沿与圆角 clip 边缘重合会被二次抗锯齿，
        // 四角糊出一圈灰噪点。改成「外环 + 内底」两层实心圆角块叠出 1px 圆环。
        // 代价：窗口四周各多留 edge 个逻辑像素当外环，面板内容整体内缩同样宽度（place() 里把窗口回退对齐触发框）。
        const float edge = (w && w->dpi > 0.f) ? 1.f / w->dpi : 1.f;   // 1 个物理像素对应的逻辑长度
        gPopup = std::make_unique<SettingUi::Popup>(w, pw + edge * 2.f, totalH + edge * 2.f);
        gPopup->setAttachTop(true);
        gPopup->setHitTransparentTop(headerH * dpi + 1.f);   // 顶部那条=触发框：点击透给宿主触发框
        auto* box = gPopup->box();
        box->setBg(SettingTheme::border);   // 外壳填描边色：内底内缩后正好露出一圈 1px 外环
        box->setBorder(0.f, 0);
        // 浮层与触发框圆角必须一致：展开后首行盖在原触发框位置上，
        // 若两者圆角不同（10 vs 8），同一处的四角会"跳一下"。
        box->setBorderRadius(SettingTheme::radiusCtl);
        box->setPadding(edge, edge, edge, edge);

        // 内底：浮层真正的底色与圆角；首行/选项都挂在它下面。
        auto* panel = box->makeChild<Ling::Node>();
        panel->setWidthPercent(100.f);
        panel->setBg(SettingTheme::popover);
        panel->setBorderRadius(SettingTheme::radiusCtl - edge);
        panel->setFlexDirection(Ling::FlexDirection::Column);
        panel->setFlexShrink(1.f);

        // 首行：复刻触发框（同高/同左右内边距/同字号与箭头），左内边距用触发框自己的值。
        const auto btnPad = btn->getPadding();
        auto* head = panel->makeChild<Ling::Button>();
        head->setText(L"");   // 文本/图标都交给子 Label，字体族才能与触发框一致
        head->setHeight(headerH);
        head->setWidthPercent(100.f);
        head->setFlexDirection(Ling::FlexDirection::Row);
        head->setJustifyContent(Ling::Justify::Start);
        head->setAlignItems(Ling::Align::Center);
        head->setPadding(std::get<0>(btnPad), 0.f, std::get<2>(btnPad), 0.f);
        head->setBorderRadius(0.f);
        // 首行＝原来的触发框位置，显示的就是"当前选中项"：用选中底色标出，
        // 悬停取同值 → 鼠标进出不产生变化。点它只是收起（它不是可选项）。
        head->setBg(SettingTheme::popupSelBg);
        head->setHoverBg(SettingTheme::popupSelBg);
        head->onClick.add([btn](Ling::Button*) {   // 点首行（原触发框位置）＝收起
            btn->setBorderWidth(1.f);
            destroyPopup();
        });
        {
            auto* val = head->makeChild<Ling::Label>();
            val->setFontSize(SettingTheme::fontBase);
            val->setColor(SettingTheme::popupFg);
            val->setFlexShrink(1.f);
            val->setText(cur >= 0 && cur < (int)options.size() ? options[cur] : L"");
            auto* spacer = head->makeChild<Ling::Node>();
            spacer->setFlexGrow(1.f);
            spacer->setFlexShrink(1.f);
            auto* icon = head->makeChild<FlipVLabel>();   // 垂直镜像的箭头
            icon->setText(Icon::Dropdown);
            icon->setFontFamily(Icon::Family);
            icon->setFontSize(Icon::DropSize);
            icon->setColor(SettingTheme::textMuted);
            icon->setFlexShrink(0.f);
            icon->setMarginLeft(6.f);
        }

        // 选项区
        auto* listWrap = panel->makeChild<Ling::Node>();
        listWrap->setWidthPercent(100.f);
        listWrap->setFlexDirection(Ling::FlexDirection::Column);
        listWrap->setFlexShrink(1.f);
        listWrap->setPadding(pad, pad, pad, pad);

        auto addRow = [&](const std::wstring& text, std::function<void()> onPick) {
            auto* item = listWrap->makeChild<Ling::Button>();
            item->setText(text);
            item->setHeight(itemH);
            item->setWidthPercent(100.f);
            item->setFlexDirection(Ling::FlexDirection::Row);
            item->setJustifyContent(Ling::Justify::Start);
            item->setAlignItems(Ling::Align::Center);
            item->setPadding(8.f, 0, 8.f, 0);
            item->setBorderRadius(SettingTheme::radiusInner);   // 与外框(10)同心
            item->setColor(SettingTheme::popupFg);
            // 悬停文字必须与常态同色：Button 悬停时会用 hoverColor 覆盖 text，
            // 不设就退回默认深灰(#333333)，在深色浮层上等于看不见。
            item->setHoverColor(SettingTheme::popupFg);
            item->setBg(0);
            item->setHoverBg(SettingTheme::popupSelBg);
            item->onClick.add([onPick, btn](Ling::Button*) {
                if (onPick) onPick();
                btn->setBorderWidth(1.f);   // 恢复触发框描边
                destroyPopup();
            });
            return item;
        };

        for (size_t r = 0; r < rows.size(); r++) {
            const int i = rows[r];
            auto* item = addRow(options[i], [i, options, onChange, setValue, sel]() {
                *sel = i;   // 记住新选中项：下次展开首行/排除项才对
                if (setValue) setValue(options[i]);
                if (onChange) onChange(i, options[i]);
            });
            if (r + 1 < rows.size()) item->setMarginBottom(itemGap);
        }
        if (rows.empty()) {
            // 唯一选项就是当前值 → 列表空，补一行「无」：点了只收起，不改值
            addRow(Lang::get(L"setting.none"), nullptr);
        }
        gPopup->setAnchor(btn);
        gPopup->onDismiss = [btn]() {
            btn->setBorderWidth(1.f);   // 恢复触发框描边
            destroyPopup();
        };
        // 延迟 open：本回调运行在宿主 onMouseDown 的派发循环内（点触发按钮即触发），
        // 在此把浮窗的 hook add 到宿主事件，会修改正在迭代的订阅列表；与 destroyPopup
        // 的延迟释放同理，推迟到调度队列执行可避免事件循环中途增删订阅导致的崩溃。
        auto* app = Ling::App::get();
        if (app) {
            auto* popup = gPopup.get();
            app->dq.TryEnqueue([popup]() { popup->open(); });
        } else {
            gPopup->open();
        }
    });
}

// ── 通用按钮工厂（4 变体共享骨架）───────────────────────────────────────
enum class BtnVariant { Primary, Outline, Ghost, Destructive };
Ling::Button* makeBtn(Ling::Node* parent, const std::wstring& text,
                      std::function<void()> onClick, float w, BtnVariant v)
{
    auto* b = parent->makeChild<Ling::Button>();
    b->setText(text);
    b->setHeight(SettingTheme::ctrlH);
    if (w > 0.f) b->setWidth(w);
    else         b->setPadding(SettingTheme::sp4, 0, SettingTheme::sp4, 0);
    b->setBorderRadius(SettingTheme::radiusLg);
    b->setFlexDirection(Ling::FlexDirection::Row);
    b->setJustifyContent(Ling::Justify::Center);
    b->setAlignItems(Ling::Align::Center);
    b->setFontSize(SettingTheme::fontBase);
    switch (v) {
    case BtnVariant::Primary:
        b->setBorder(1.f, SettingTheme::primary);
        b->setBg(SettingTheme::primary);
        b->setColor(SettingTheme::primaryForeground);
        b->setHoverBg(SettingTheme::primaryHover);
        b->setHoverColor(SettingTheme::primaryForeground);
        break;
    case BtnVariant::Outline:
        b->setBorder(1.f, SettingTheme::border);
        b->setBg(SettingTheme::card);
        b->setColor(SettingTheme::foreground);
        b->setHoverBg(SettingTheme::accent);
        b->setHoverColor(SettingTheme::foreground);
        break;
    case BtnVariant::Ghost:
        b->setBorder(0.f, 0);
        b->setBg(0);
        b->setColor(SettingTheme::foreground);
        b->setHoverBg(SettingTheme::accent);
        b->setHoverColor(SettingTheme::foreground);
        break;
    case BtnVariant::Destructive:
        b->setBorder(1.f, SettingTheme::destructive);
        b->setBg(SettingTheme::destructive);
        b->setColor(SettingTheme::destructiveForeground);
        b->setHoverBg(0xDC2626FF);         // red-600
        b->setHoverColor(SettingTheme::destructiveForeground);
        break;
    }
    if (onClick) {
        b->onClick.add([onClick](Ling::Button*) { onClick(); });
    }
    return b;
}

} // namespace

// ─── SettingUi::Popup：独立 WS_POPUP 顶层浮层窗口 ─────────────────────
//   原浮层挂宿主 win->body 下会被窗口客户区裁剪；独立窗口不再受宿主裁剪，
//   可正常超出界面显示。生命周期由全局 gPopup(unique_ptr) 持有。
Popup::Popup(Ling::WinBase* host, float pw, float totalH)
    : Ling::WinBase(), host(host), pw(pw), totalH(totalH)
{
    dpi = host->dpi;
    createNativeWindow(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, WS_POPUP);
}

Popup::~Popup()
{
    if (sOpen == this) sOpen = nullptr;
    uninstallHook();
    removeHostHooks();
    // WinBase::~WinBase 不会销毁 HWND，需在此显式关闭，否则 hwnd 泄漏为幽灵窗口。
    if (hwnd) close();
}

// 吸附模式下，浮层顶部（被盖住的触发框那一条）对命中测试透明：
//  该区域的点击不落在浮层窗口上，而是直接落到宿主窗口的触发框上 —— 也就是"再点一次触发框收回"
//  走的是和未展开时点击触发框完全一样的路径（点选项、点外部不受影响）。
//  HTTRANSPARENT 只在同一线程的窗口之间生效，宿主与浮层同线程，符合条件。
LRESULT Popup::onHitTest(const POINT pos)
{
    if (hitTransparentTop > 0.f && hwnd) {
        POINT pt{ pos.x, pos.y };   // WM_NCHITTEST 的 lParam 是屏幕坐标
        ScreenToClient(hwnd, &pt);
        if (pt.y >= 0 && (float)pt.y < hitTransparentTop) return HTTRANSPARENT;
    }
    return HTCLIENT;
}

void Popup::onCreated()
{
    body->setBg(0);
    // 内容容器以绝对定位铺满窗口客户区，尺寸随窗口物理尺寸变化。
    listBox = body->makeChild<Ling::ScrollerBox>();
    listBox->setPositionType(Ling::Position::Absolute);
    listBox->setSizePercent(100.f, 100.f);
    listBox->setScrollBarVisible(false);   // 下拉弹层不显示滚动条
}

// 静态成员定义：标记当前打开的浮层，供全局低级鼠标钩子回调使用。
SettingUi::Popup* SettingUi::Popup::sOpen = nullptr;

void Popup::open()
{
    // 物理尺寸与 setSize 一致（dpi 换算），但不再拆成 setSize + place()->setPosition 的多次
    // SetWindowPos。每次 SetWindowPos 都会让窗口在"显示/未显示"边界重新结算一次位置与激活态；
    // 加上这些调用原本不带 SWP_NOACTIVATE，浮层在首次显现前后就可能短暂改写前台/激活状态，
    // 表现为宿主（设置窗口）原生标题栏"失焦→回焦"的闪烁。
    w = pw * dpi;
    h = totalH * dpi;
    place();  // 填充 this->x / this->y（屏幕物理坐标；其内部 setPosition 已含 SWP_NOACTIVATE）
    if (hwnd) {
        // 一次性完成 置顶 + 定位 + 显示，并强制 SWP_NOACTIVATE：
        // 浮层带 WS_EX_NOACTIVATE，本就不该参与激活；SWP_SHOWWINDOW 让窗口出现，
        // SWP_NOACTIVATE 保证出现瞬间不夺走宿主激活态，宿主始终保持前台/激活，标题栏不闪。
        // 不再调用 SetForegroundWindow —— 那会引发"浮层显焦→再强制夺回"的前台拉锯，正是在
        // SW_SHOWNA 之外仍产生闪烁的根源。
        SetWindowPos(hwnd, HWND_TOPMOST, (int)x, (int)y, (int)w, (int)h,
                     SWP_SHOWWINDOW | SWP_NOACTIVATE);
    }
    // 打开即挂全局低级鼠标钩子：点击浮层窗口之外的任何位置（其它软件/宿主空白/桌面）
    // 都能命中并关闭浮层，不再依赖宿主是否失焦/失活。同一时刻仅一个浮层 → sOpen 单例。
    sOpen = this;
    installHook();
    if (host) {
        // 点击宿主窗口区域（popup 之外）→ 关闭浮层。popup 是独立窗口，其自身的
        // 鼠标消息不会传回宿主，故宿主 onMouseDown 即代表点到外部。
        mouseTok = host->onMouseDown.add([this](POINT, bool) {
            if (onDismiss) onDismiss();
            else destroyPopup();
        });
        // 宿主内容滚动 → 重定位到按钮当前可见位置。
        wheelTok = host->onMouseWheel.add([this](POINT, float) { place(); });
    }
    refresh();
}

void Popup::place()
{
    if (!hwnd || !anchor || !host->hwnd) return;
    // anchor->x/y 是"未滚动"的窗口客户区物理坐标，需扣掉祖先 ScrollerBox 的滚动偏移
    // 得按钮当前可见物理位置，再经 ClientToScreen 转成屏幕物理坐标。
    float sy = 0.f;
    for (Ling::Node* p = anchor->parent; p; p = p->parent) {
        if (auto* sb = dynamic_cast<Ling::ScrollerBox*>(p)) sy += sb->getScrollY();
    }
    // 顶部吸附（下拉一体展开）：浮层左缘/上缘与锚定按钮完全对齐，直接盖住按钮，
    //  两者同宽 + 面板首行复刻按钮 → 展开后看起来就是一整块面板，不做底部翻转。
    if (attachTop) {
        POINT tl{ static_cast<LONG>(anchor->x), static_cast<LONG>(anchor->y - sy) };
        ClientToScreen(host->hwnd, &tl);
        // 面板四周各留了 1px 外环（见 bindPopup），窗口左上角相应回退 1px，
        // 面板内容（首行）才能与触发框严格对齐。
        setPosition(tl.x - 1, tl.y - 1);
        return;
    }
    // 触发按钮与浮层之间留 4px（设计单位 → 物理像素）间距，不再紧贴按钮下缘。
    const LONG gap = static_cast<LONG>(4.f * dpi);
    // 默认向下弹出：定位到锚定按钮正下缘 + 间距。
    POINT pt{ static_cast<LONG>(anchor->x), static_cast<LONG>(anchor->y - sy + anchor->h) + gap };
    ClientToScreen(host->hwnd, &pt);
    // 按钮贴近屏幕底部时，向下弹出会超出工作区被裁剪（发送按钮下拉被截断）→ 翻转向上。
    MONITORINFO mi{ sizeof(MONITORINFO) };
    if (HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST)) {
        GetMonitorInfo(mon, &mi);
        if (pt.y + (LONG)h > mi.rcWork.bottom) {
            POINT top{ static_cast<LONG>(anchor->x), static_cast<LONG>(anchor->y - sy) };
            ClientToScreen(host->hwnd, &top);
            setPosition(top.x, top.y - (LONG)h - gap);
            return;
        }
    }
    setPosition(pt.x, pt.y);
}

void Popup::removeHostHooks()
{
    if (host) {
        host->onMouseDown.remove(mouseTok);
        host->onMouseWheel.remove(wheelTok);
    }
}

// ─── 全局低级鼠标钩子：点击浮层之外的任意位置即关闭 ────────────────────
//   Popup 是带 WS_EX_NOACTIVATE 的独立置顶窗口：点击其它软件时宿主（设置窗口）失焦失活，
//   但 Popup 自身不参与激活、也不会收到任何失活通知，导致其持续悬浮在别的软件之上。
//   办法：浮层打开期间挂 WH_MOUSE_LL 钩子，捕获全系统鼠标按下事件；若落点不在浮层窗口
//   内（其它软件 / 宿主空白 / 桌面），立即关闭浮层。这是"点击外部即关闭"的确定性实现，
//   不依赖宿主焦点状态机，也天然满足所有下拉互斥（同一时刻仅 sOpen 一个）。
LRESULT CALLBACK Popup::lowLevelProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION) {
        if (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN || wParam == WM_MBUTTONDOWN) {
            if (sOpen && sOpen->hwnd) {
                auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
                HWND h = WindowFromPoint(ms->pt);
                // 点击落在浮层内（含其内所有自绘子项，均映射到 Popup 单一顶层层 HWND）
                // → 由浮层自身处理；其它任何落点 → 关闭浮层。
                if (h != sOpen->hwnd) {
                    // 左键落在其锚定/触发按钮上 → 交给按钮 onClick 做开关切换（点开/点关），
                    // 钩子不干预。钩子在消息派发前运行，若在此关闭浮层，按钮 onClick 会因 gPopup
                    // 已为 null 而重新展开，"点击第二下关闭"失效。故锚定/触发按钮上的点击留给 onClick 处理。
                    // 注意：触发按钮可与锚定按钮不同（如 Tab ▾ 触发、菜单却左对齐胶囊整体），
                    // 此时须同时识别 trigger，否则点击触发按钮会被当作"外部点击"先关闭、再被 onClick 重开。
                    auto buttonHit = [&](Ling::Button* btn) {
                        if (!btn) return false;
                        float sy = 0.f;
                        for (auto* p = btn->parent; p; p = p->parent)
                            if (auto* sb = dynamic_cast<Ling::ScrollerBox*>(p)) sy += sb->getScrollY();
                        POINT tl{ static_cast<LONG>(btn->x), static_cast<LONG>(btn->y - sy) };
                        ClientToScreen(sOpen->host->hwnd, &tl);
                        RECT rc{ tl.x, tl.y, tl.x + (LONG)btn->w, tl.y + (LONG)btn->h };
                        return !!PtInRect(&rc, ms->pt);
                    };
                    bool onAnchor = false;
                    if (wParam == WM_LBUTTONDOWN && sOpen->host && sOpen->host->hwnd) {
                        onAnchor = buttonHit(sOpen->anchor) ||
                                   (sOpen->trigger && sOpen->trigger != sOpen->anchor && buttonHit(sOpen->trigger));
                    }
                    if (!onAnchor) {
                        if (sOpen->onDismiss) sOpen->onDismiss();
                        else destroyPopup();
                    }
                }
            }
        }
    }
    // 必须放行，否则会吞掉全系统的鼠标消息（浮层内部点击、其它软件点击均受影响）。
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

void Popup::installHook()
{
    if (mouseHook) return;
    mouseHook = SetWindowsHookExW(WH_MOUSE_LL, lowLevelProc, GetModuleHandleW(nullptr), 0);
}

void Popup::uninstallHook()
{
    if (mouseHook) {
        UnhookWindowsHookEx(mouseHook);
        mouseHook = nullptr;
    }
}

// ─── SettingUi::HoverTip：悬停气泡（圆角矩形 + 底部小三角）───────────────
//   外壳直接复用工具栏停悬气泡那套 ToolbarChrome::paintPropBubble（一体闭合路径，
//   圆角与三角接缝处不会多出一条描边）；宽度固定 280，文本在其中自动折行。
namespace {
constexpr float kTipW{ 280.f };     // 气泡固定宽度；文本在此宽度内自动折行
constexpr float kTipPadX{ 12.f };   // 文本左右内边距（比系统 tooltip 宽）
constexpr float kTipPadY{ 8.f };    // 文本上下内边距（比系统 tooltip 高）
constexpr float kTipGap{ 4.f };     // 三角尖端到锚定控件边缘的间距
constexpr float kTipProbeH{ 400.f };// 首次测量的探测高度（只为取折行后的文本高）
// 颜色按 RRGGBBAA 解析（Ling::Color 的约定）
constexpr uint32_t kTipBg{ 0xE5E5E5FF };   // 气泡底色 #E5E5E5
constexpr uint32_t kTipFg{ 0x262626FF };   // 文字深灰（浅底必须用深字才读得清）
}

HoverTip::HoverTip(Ling::WinBase* host) : Ling::WinBase(), host(host)
{
    if (host) {
        dpi = host->dpi;
        // 兜底：按钮 onLeave 偶发漏掉（快速移出/页面重建）时，宿主鼠标移动即可收掉气泡。
        // 只订阅一次；anchor 在每次悬停时更新。
        moveTok = host->onMouseMove.add([this](POINT pos) {
            if (!isVisible || !anchor) return;
            if (pos.x == INT_MAX || !anchor->isPosIn(pos)) hideTip();
        });
    }
    createNativeWindow(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, WS_POPUP);
}

HoverTip::~HoverTip()
{
    if (host) host->onMouseMove.remove(moveTok);
    // WinBase::~WinBase 不销毁 HWND，需显式关闭，否则残留成幽灵窗口。
    if (hwnd) close();
}

void HoverTip::onCreated()
{
    auto d2d = Ling::D2D::get();
    if (d2d && d2d->deviceContext) {
        d2d->deviceContext->CreateSolidColorBrush(
            Ling::Color(kTipBg).getD2DColor(), brushBg.GetAddressOf());
    }
    body->setBg(0);
    // 外壳（圆角 + 三角）由 Canvas 手绘；节点层只负责文本。
    canvas = body->makeChild<Ling::Canvas>();
    canvas->setPositionType(Ling::Position::Absolute);
    canvas->setSizePercent(100.f, 100.f);
    content = body->makeChild<Ling::Node>();
    content->setPositionType(Ling::Position::Absolute);
    content->setPosition(Ling::Edge::Left, 0.f);
    content->setPosition(Ling::Edge::Right, 0.f);
    content->setFlexDirection(Ling::FlexDirection::Column);
    content->setAlignItems(Ling::Align::Center);
    content->setJustifyContent(Ling::Justify::Center);
    content->setPadding(kTipPadX, kTipPadY, kTipPadX, kTipPadY);
    // 三角所在一侧让给外壳：文本区避开它（与 paintPropBubble 的 ContentPad 配套）
    ToolbarChrome::applyPropBubbleContentPad(content, tipDown);
    label = content->makeChild<Ling::Label>();
    label->setFontSize(SettingTheme::fontSm);
    label->setColor(Ling::Color(kTipFg));
}

void HoverTip::layout()
{
    Ling::WinBase::layout();
    if (!canvas || !brushBg) return;
    auto ctx = canvas->startPaint();
    if (!ctx) return;
    ctx->Clear(0);
    // 无描边（0.f）：只填 #E5E5E5 底，气泡不带边框
    ToolbarChrome::paintPropBubble(ctx, w, h, dpi, brushBg.Get(), arrowX, tipDown, 0.f);
    canvas->finishPaint();
}

void HoverTip::bind(Ling::Button* btn, const std::wstring& t)
{
    // 每次悬停时才把锚点/文本切到本按钮：同一个气泡实例可服务页面上任意多个「?」。
    btn->onEnter.add([this, btn, t](Ling::Button*) { showFor(btn, t); });
    btn->onLeave.add([this, btn](Ling::Button*) { hideFor(btn); });
}

void HoverTip::showFor(Ling::Node* owner, const std::wstring& t)
{
    if (!owner) return;
    anchor = owner;
    text = t;
    showNow();
}

void HoverTip::hideFor(Ling::Node* owner)
{
    if (anchor == owner) hideTip();
}

void HoverTip::hideTip()
{
    if (!isVisible) return;
    hide();
    isVisible = false;
}

void HoverTip::showNow()
{
    if (!host || !anchor || !label || !hwnd) return;
    label->setText(text);
    const float d = dpi > 0.f ? dpi : 1.f;
    // 宽度固定 280：长文本由 Text::measureCB 在该宽度内自动折行，探测一次取折行后的文本高，
    // 再据此定窗口最终高度（文本 + 上下内边距 + 三角）。
    setSize(kTipW, kTipProbeH);
    layout();
    const float ch = std::max(1.f, label->h / d);
    setSize(kTipW, ch + kTipPadY * 2.f + ToolbarTheme::caretSize);
    layout();
    place();
    layout();   // place() 定下三角位置后再绘一次，避免显示首帧三角错位
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, (int)w, (int)h,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);
    isVisible = true;
}

void HoverTip::place()
{
    if (!hwnd || !anchor || !host || !host->hwnd) return;
    // anchor->x/y 是"未滚动"的客户区物理坐标：扣掉祖先 ScrollerBox 的滚动偏移才是可见位置
    float sy = 0.f;
    for (Ling::Node* p = anchor->parent; p; p = p->parent)
        if (auto* sb = dynamic_cast<Ling::ScrollerBox*>(p)) sy += sb->getScrollY();
    POINT tl{ static_cast<LONG>(anchor->x), static_cast<LONG>(anchor->y - sy) };
    POINT br{ static_cast<LONG>(anchor->x + anchor->w), static_cast<LONG>(anchor->y - sy + anchor->h) };
    ClientToScreen(host->hwnd, &tl);
    ClientToScreen(host->hwnd, &br);
    const RECT ref{ tl.x, tl.y, br.x, br.y };
    const float ax = (float)(tl.x + br.x) * 0.5f;   // 锚定控件上缘中点（屏坐标）
    const auto p = ToolbarChrome::placeBubble(ToolbarChrome::workAreaNear(ref),
        (float)tl.y, (float)br.y, w, h, kTipGap * dpi,
        ax - w * 0.5f, ax, false, tipDown, arrowX, x, y);
    if (p.tipDown != tipDown) {
        tipDown = p.tipDown;
        ToolbarChrome::applyPropBubbleContentPad(content, tipDown);
    }
    arrowX = p.arrowX;
    if (p.moved) setPosition(p.x, p.y);
    refresh();
}

// ─── SettingUi::ScrollFollower：在 SettingUi 直接作用域定义（不可放匿名 namespace）──
ScrollFollower::ScrollFollower(Ling::WinBase* w,
    Ling::ScrollerBox* p,
    std::function<std::pair<float, float>()> comp)
    : win(w), popup(p), compute(std::move(comp))
{
    // 订阅顺序保证：内容 ScrollerBox 的滚轮处理在窗口构建时注册（先于我），
    // 因此每次滚动时 scrollY 先被更新、再进入本回调重算 → 位置始终正确。
    wheelTok = win->onMouseWheel.add([this](POINT, float) { place(); });
    moveTok  = win->onMouseMove.add([this](POINT)        { place(); });
}

void ScrollFollower::place()
{
    if (!popup) return;
    auto [l, t] = compute ? compute() : std::pair<float, float>{ 0.f, 0.f };
    if (l == lastLeft && t == lastTop) return;   // 位置没变（如未滚动的纯鼠标移动），跳过重排
    lastLeft = l; lastTop = t;
    left = l; top = t;
    popup->setPosition(Ling::Edge::Left, l);
    popup->setPosition(Ling::Edge::Top,  t);
    // setPosition 只写入 yoga 位置样式，不会触发重排；这里的 visual.Offset 只有在
    // win->refresh() 引发的下一帧 layout() 时才会按新位置重算。若不 refresh，浮层坐标看着
    // 改了实则停留在原地（"内容滚动但下拉菜单不动"的根因）。
    win->refresh();
}

ScrollFollower::~ScrollFollower()
{
    if (win) {
        win->onMouseWheel.remove(wheelTok);
        win->onMouseMove.remove(moveTok);
    }
}

// ============================================================================
//  浮层管理
// ============================================================================
void closePopup(Ling::WinBase* win)
{
    if (!win || (gPopup && gPopup->hostWindow() == win)) destroyPopup();
}

bool hasPopup()
{
    return gPopup != nullptr;
}

std::pair<float, float> visiblePos(Ling::Node* node)
{
    // ScrollerBox 滚动只平移 content 视觉偏移、不改子节点布局坐标，node->x/y 是"未滚动"
    // 的窗口坐标。沿 parent 链累加祖先 ScrollerBox 的 scrollY，换算回按钮当前显示位置。
    float sy = 0.f;
    for (Ling::Node* p = node->parent; p; p = p->parent) {
        if (auto* sb = dynamic_cast<Ling::ScrollerBox*>(p)) sy += sb->getScrollY();
    }
    const float d = node->win->dpi;
    return { node->x / d, (node->y - sy) / d };
}

// 等价于 makeChild<T>() 的后半段：手动挂接裸节点并转移所有权
void adoptChild(Ling::Node* parent, Ling::Node* child)
{
    if (!parent || !child) return;
    SettingUi::NodeFriendAccess::Adopt(parent, child);
}

// ─── 末尾留白修剪：沿"视觉最底边"逐层清空末尾节点的底部 margin ──
void trimTrailingGap(Ling::Node* content)
{
    if (!content) return;
    // 从传入容器出发，反复取"最后一个子节点"；它是当前层视觉上最靠底的元素，
    // 其 marginBottom 会在外层已给 contentPadYBottom(20) 的基础上再垫高底部。
    // 逐层清掉该链上每个节点的 marginBottom，同时继续深入该节点的最后一个子节点，
    // 直到到达叶子（无子节点）为止，从而让底部留白只由外层 content 提供。
    // 对 wrap 网格特判（如某些双列 wrap 容器）：最后一行常有两个 cell，只清"最后一个"
    // 会漏掉同行的左列，故按"每行 2 个"的奇偶推最后一行起始下标，只把最后一行所有
    // cell 的 marginBottom 清零（保留中间行的行间距 optionGap），而不影响其它 cell。
    // 注：grid2 双栏现改为 Column+每行独立 Row（见 grid2Cells），不会触发本分支；该特判
    // 仅为其它仍用 flex-wrap 的网格保留。普通容器走 else 分支：清最后一个子节点即可。
    while (!content->children.empty()) {
        auto& children = content->children;
        bool wrapGrid = content->node
            && YGNodeStyleGetFlexWrap(content->node) == YGWrapWrap;
        if (wrapGrid) {
            const int n = (int)children.size();
            const int lastRowStart = (n % 2 == 0) ? (n - 2) : (n - 1);
            for (int i = n - 1; i >= lastRowStart; i--)
                children[i]->setMarginBottom(0.f);
        } else {
            children.back()->setMarginBottom(0.f);
        }
        content = children.back().get();
    }
}

// ============================================================================
//  Section Header：页面标题 + 描述
//  shadcn PageHeader：h1(20px zinc-950) + p(12px zinc-500)
//  注意：Ling 不支持粗体，通过字号对比与 muted 色建立层级。
// ============================================================================
Ling::Node* sectionHeader(Ling::Node* parent,
                          const std::wstring& title,
                          const std::wstring& desc,
                          const std::wstring& tip)
{
    auto* box = parent->makeChild<Ling::Node>();
    box->setFlexDirection(Ling::FlexDirection::Column);
    box->setMarginBottom(SettingTheme::sectionHeaderBottom);

    // 标题行：[标题][?]（问号说明）
    auto* titleRow = box->makeChild<Ling::Node>();
    titleRow->setFlexDirection(Ling::FlexDirection::Row);
    titleRow->setAlignItems(Ling::Align::Center);
    titleRow->setWidthPercent(100.f);

    auto* t = titleRow->makeChild<Ling::Label>();
    t->setText(title);
    t->setFontSize(SettingTheme::font2Xl);
    t->setColor(SettingTheme::textPrimary);
    t->setFlexShrink(1.f);
    helpTipButton(titleRow, tip);

    if (!desc.empty()) {
        auto* d = box->makeChild<Ling::Label>();
        d->setText(desc);
        d->setFontSize(SettingTheme::fontSm);
        d->setColor(SettingTheme::textSecondary);
        d->setMarginTop(SettingTheme::titleDescGap);
    }
    return box;
}

// ============================================================================
//  Group Card：shadcn Card 分组容器
//  白底 + zinc-200 描边 + radiusLg + header/content 结构
// ============================================================================
Ling::Node* groupCard(Ling::Node* parent,
                      const std::wstring& cardTitle,
                      const std::wstring& cardDesc)
{
    auto* c = parent->makeChild<Ling::Node>();
    c->setFlexDirection(Ling::FlexDirection::Column);
    c->setWidthPercent(100.f);
    c->setBg(SettingTheme::card);
    c->setBorder(1.f, SettingTheme::border);
    c->setBorderRadius(SettingTheme::radiusLg);
    c->setMarginBottom(SettingTheme::blockGap);

    if (!cardTitle.empty()) {
        auto* hBox = c->makeChild<Ling::Node>();
        hBox->setFlexDirection(Ling::FlexDirection::Column);
        hBox->setWidthPercent(100.f);
        hBox->setPadding(SettingTheme::cardPadX, SettingTheme::cardHeaderPadY,
                         SettingTheme::cardPadX, SettingTheme::cardHeaderPadY);

        auto* hl = hBox->makeChild<Ling::Label>();
        hl->setText(cardTitle);
        hl->setFontSize(SettingTheme::fontXl);
        hl->setColor(SettingTheme::textPrimary);
        if (!cardDesc.empty()) {
            auto* hd = hBox->makeChild<Ling::Label>();
            hd->setText(cardDesc);
            hd->setFontSize(SettingTheme::fontSm);
            hd->setColor(SettingTheme::textSecondary);
            hd->setMarginTop(SettingTheme::sp1);
        }
        // header 与 content 之间的 1px 分隔线
        auto* line = c->makeChild<Ling::Node>();
        line->setHeight(1.f);
        line->setWidthPercent(100.f);
        line->setBg(SettingTheme::border);
    }

    // 内容容器（无论是否有 header 都创建，便于调用方统一使用）
    auto* content = c->makeChild<Ling::Node>();
    content->setFlexDirection(Ling::FlexDirection::Column);
    content->setWidthPercent(100.f);
    content->setPadding(SettingTheme::cardPadX, SettingTheme::cardPadY,
                        SettingTheme::cardPadX, SettingTheme::cardPadY);
    return content;
}

// ============================================================================
//  Group Label：card 内分组小标题
// ============================================================================
// ─── 功能提示（问号说明）───────────────────────────────────────────────
namespace {
    // 设置窗口在 onCreated 里注册进来的悬停气泡（工具栏停悬气泡那套外壳：圆角矩形 + 小三角）。
    // 未注册时 helpTipButton 返回 nullptr —— 构建期不产生问号。
    HoverTip* s_helpTip{ nullptr };

    // 选项说明文案表：键 = 中文选项原文；
    // 英文界面下语言包给的是英文，查不到 → 不出问号）。
    const std::wstring& optionTipFor(const std::wstring& label)
    {
        static const std::map<std::wstring, std::wstring> tips = {
            { L"开机自启", L"登录系统后自动启动" },
            { L"以管理员身份运行", L"避免因权限不足导致快捷键注册失败、无法截取高权限窗口等问题" },
            { L"查找窗口元素", L"截图时识别并框选窗口内的子元素" },
            { L"“取消”弹窗", L"已绘制标注时，截图里按取消会先确认，避免误触丢失内容" },
            { L"以文件形式复制", L"截图先保存到指定目录，再以文件形式复制到剪贴板" },
            { L"截图后保存", L"截图完成后自动保存到指定目录" },
            { L"以鼠标为中心缩放", L"滚轮缩放贴图时以鼠标位置为中心" },
            { L"自动文本识别", L"贴图后自动进行文本识别" },
            { L"自动缩放窗口", L"贴图超出显示器时自动缩放窗口，完整显示内容" },
            { L"托盘图标", L"在系统托盘显示图标" },
            { L"功能提示", L"在设置项旁显示说明问号，悬停查看" },
            { L"单击托盘执行", L"单击托盘图标的操作，右键仍是托盘菜单" },
            { L"双击后执行", L"双击贴图时执行的操作" },
            { L"默认工具", L"进入全屏画布时默认选中的工具" },
            { L"工具栏贴边", L"贴边时吸附上/下边缘，或收起到顶部，鼠标移到边缘临时显示" },
            { L"编码模式", L"自动优先硬件编码，失败回落 H.264；也可指定 H.264 / H.265" },
            { L"编码速率", L"速率越快越省性能，但文件体积越大" },
            { L"启用硬件加速", L"编码模式不适配会导致录制失败；花屏或异常时关闭，改用软件编码" },
            { L"视频清晰度", L"输出高度上限：区域更高时等比缩小，不放大" },
            { L"动图清晰度", L"动图的输出高度上限，同样只缩不放" },
            { L"动图格式", L"GIF 兼容最好 / WebP 体积最小 / APNG 无损，扩展名仍为 .png" },
            { L"麦克风", L"选择录制使用的麦克风，无设备时使用系统默认" },
            { L"截图文件路径", L"截图与「以文件形式复制」的保存位置，留空用默认目录" },
            { L"录屏文件路径", L"录屏与动图的保存位置，留空用默认目录" },
            { L"字典模式", L"点击译文中的英文单词，即可查看详细释义" },
        };
        static const std::wstring empty;
        auto it = tips.find(label);
        return it == tips.end() ? empty : it->second;
    }
}

void attachHelpTip(HoverTip* tip) { s_helpTip = tip; }

HoverTip* helpTip() { return s_helpTip; }

Ling::Button* helpTipButton(Ling::Node* parent, const std::wstring& text)
{
    if (!parent || text.empty()) return nullptr;
    if (!s_helpTip) return nullptr;                                        // 窗口尚未注册气泡
    if (!Setting::get() || !Setting::get()->getFeatureTips()) return nullptr;  // 总开关关闭

    // 问号按钮：用图标字库里的「问题」字形（无圆底/描边）。
    // 颜色随主题：浅色 #1E1E1E（白卡片上看得清）；深色 #E5E5E5（深卡片上看得清）。
    // 悬停只弹统一气泡（HoverTip），按钮本身颜色不变。
    const uint32_t qc = SettingTheme::isDark() ? 0xE5E5E5FF : 0x1E1E1EFF;
    auto* btn = parent->makeChild<Ling::Button>();
    btn->setText(Icon::Problem);
    btn->setFontFamily(Icon::Family);
    btn->setFontSize(16.f);
    btn->setColor(qc);
    btn->setHoverColor(qc);           // 保持颜色不变
    btn->setSize(18.f, 18.f);
    btn->setPadding(0.f, 0.f, 0.f, 0.f);
    btn->setMarginLeft(6.f);
    btn->setFlexShrink(0.f);
    btn->setFlexDirection(Ling::FlexDirection::Row);
    btn->setJustifyContent(Ling::Justify::Center);
    btn->setAlignItems(Ling::Align::Center);
    s_helpTip->bind(btn, text);
    return btn;
}

// 造一个「标题 + 可选问号」的横向行；
// 返回标题 Label（与旧 groupLabel 返回类型一致，调用方大多忽略返回值）。
static Ling::Label* makeGroupLabelRow(Ling::Node* parent, const std::wstring& text,
                                      const std::wstring& tip)
{
    auto* row = parent->makeChild<Ling::Node>();
    row->setFlexDirection(Ling::FlexDirection::Row);
    row->setAlignItems(Ling::Align::Center);
    row->setWidthPercent(100.f);
    row->setMarginTop(SettingTheme::sp3);
    row->setMarginBottom(SettingTheme::sp2);

    auto* lab = row->makeChild<Ling::Label>();
    lab->setText(text);
    lab->setFontSize(SettingTheme::fontBase);
    // 组名用次级灰，避免与正文（textPrimary）同色
    lab->setColor(SettingTheme::textMuted);
    lab->setFlexShrink(1.f);

    helpTipButton(row, tip);
    return lab;
}

Ling::Label* groupLabel(Ling::Node* parent, const std::wstring& text)
{
    return makeGroupLabelRow(parent, text, optionTipFor(text));
}

Ling::Label* groupLabel(Ling::Node* parent, const std::wstring& text, const std::wstring& tip)
{
    return makeGroupLabelRow(parent, text, tip.empty() ? optionTipFor(text) : tip);
}

// ============================================================================
//  Separator：水平 1px zinc-200 分割线
// ============================================================================
Ling::Node* separator(Ling::Node* parent, float marginTop, float marginBottom)
{
    auto* line = parent->makeChild<Ling::Node>();
    line->setHeight(1.f);
    line->setWidthPercent(100.f);
    line->setBg(SettingTheme::border);
    line->setMarginTop(marginTop);
    line->setMarginBottom(marginBottom);
    return line;
}

// ============================================================================
//  Option Row：通用行容器（左 label/desc 区 + 右控件区）
//  chrome=false → 纯行（无卡片背景），配合 groupCard 内容区使用
//  chrome=true  → 独立行（白底 + 描边 + radiusMd），行与行之间留有 gap
// ============================================================================
OptionRowRef optionRow(Ling::Node* parent,
                       const std::wstring& label,
                       const std::wstring& desc,
                       bool chrome,
                       float ctrlH)
{
    OptionRowRef r{};
    auto* row = parent->makeChild<Ling::Node>();
    r.row = row;
    // 行高与控件高定了之后，右侧内边距取"控件的上下留白"（(行高-控件高)/2），
    // 这样控件四周（上/下/右）的留白一致，不会一边宽一边窄。
    const float rowH = desc.empty() ? SettingTheme::rowHeightCompact : SettingTheme::rowHeight;
    const float padRight = std::max(0.f, (rowH - ctrlH) * 0.5f);
    if (chrome) {
        row->setHeight(rowH);
        row->setBg(SettingTheme::card);
        row->setBorder(1.f, SettingTheme::border);
        row->setBorderRadius(SettingTheme::radiusRow);   // 与行内控件(8)+内边距(6)同心
        row->setPadding(SettingTheme::sp4, 0, padRight, 0);
        row->setMarginBottom(SettingTheme::optionGap);
    } else {
        row->setHeight(rowH);
        row->setBg(0);
        row->setBorder(0.f, 0);
        row->setPadding(0, 0, 0, 0);
        row->setMarginBottom(SettingTheme::optionGap);
    }
    row->setWidthPercent(100.f);
    row->setFlexDirection(Ling::FlexDirection::Row);
    row->setAlignItems(Ling::Align::Center);

    // Label 区（左 column）
    auto* labelBox = row->makeChild<Ling::Node>();
    r.labelBox = labelBox;
    labelBox->setFlexDirection(Ling::FlexDirection::Column);
    labelBox->setFlexGrow(1.f);
    labelBox->setFlexShrink(1.f);
    // 允许 label/desc 区收缩到文本宽度以下（配合 Text 折行），
    // 否则其 min-content = 最宽单行文字，会把右侧控件区挤出卡片右缘。
    YGNodeStyleSetMinWidth(labelBox->node, 0.f);
    labelBox->setJustifyContent(Ling::Justify::Center);
    labelBox->setMarginRight(SettingTheme::sp3);

    // 标题行：[标题][?]（问号说明）—— 与 desc 同处左 column
    auto* labRow = labelBox->makeChild<Ling::Node>();
    labRow->setFlexDirection(Ling::FlexDirection::Row);
    labRow->setAlignItems(Ling::Align::Center);
    labRow->setWidthPercent(100.f);
    YGNodeStyleSetMinWidth(labRow->node, 0.f);

    auto* lab = labRow->makeChild<Ling::Label>();
    r.label = lab;
    lab->setText(label);
    lab->setFontSize(SettingTheme::fontBase);
    lab->setColor(SettingTheme::textPrimary);
    lab->setFlexShrink(1.f);
    YGNodeStyleSetMinWidth(lab->node, 0.f);

    helpTipButton(labRow, optionTipFor(label));

    if (!desc.empty()) {
        auto* d = labelBox->makeChild<Ling::Label>();
        r.desc = d;
        d->setText(desc);
        d->setFontSize(SettingTheme::fontSm);
        d->setColor(SettingTheme::textSecondary);
        d->setMarginTop(2.f);
    }

    // 控件区（右 row，right align）
    auto* ctrlBox = row->makeChild<Ling::Node>();
    r.ctrlBox = ctrlBox;
    ctrlBox->setFlexDirection(Ling::FlexDirection::Row);
    ctrlBox->setJustifyContent(Ling::Justify::End);
    ctrlBox->setAlignItems(Ling::Align::Center);
    ctrlBox->setFlexShrink(0.f);
    return r;
}

// ============================================================================
//  双栏网格容器：把多个 optionRow/toggleRow/selectRow 以 2 列排布
//  视觉：每个选项 = 独立卡片（白底+描边+圆角），两两一行（shadcn grid-cols-2）。
//  用法：auto* g = SettingUi::grid2(p); 然后往 g 里 add toggleRow/selectRow(..., chrome=true)，
//       最后调用 SettingUi::grid2Cells(g) 统一拆成"每行 2 列"并等宽排布。
//  宽度保证：grid 是 Column；grid2Cells 把子行按两两一组塞进独立 Row，行内两列用
//   flexGrow(1) + widthPercent(0) 严格等分剩余空间，右列右缘恰好贴行右缘，因此
//   双栏总占宽（两列 + 列间距）与单栏 100% 宽度精确一致（不用 SpaceBetween/flex-wrap，
//   后者对 wrap 多行并不严格按"每行"分配，会造成右列不贴右缘的轻微错位）。
//  注意：子行本身已设 widthPercent(100)，须由 grid2Cells 覆盖为 0 + flexGrow。
// ============================================================================
Ling::Node* grid2(Ling::Node* parent)
{
    auto* g = parent->makeChild<Ling::Node>();
    g->setFlexDirection(Ling::FlexDirection::Column);  // 纵向堆叠"行"，行内两列由 grid2Cells 构造
    g->setWidthPercent(100.f);
    return g;
}

void grid2Cells(Ling::Node* grid)
{
    if (!grid) return;
    // 先把调用方连续 add 进来的直属子行收集下来（此刻它们都是 grid 的直接子节点）
    std::vector<Ling::Node*> cells;
    cells.reserve(grid->children.size());
    for (auto& c : grid->children) cells.push_back(c.get());

    const float cellGap = SettingTheme::optionGap;
    const int total = (int)cells.size();
    for (int i = 0; i < total; i += 2) {
        // 每个"行"是独立 Row：行内两列（末行可能只有左列）用 flexGrow 等分剩余空间
        auto* rowNode = grid->makeChild<Ling::Node>();
        rowNode->setFlexDirection(Ling::FlexDirection::Row);
        rowNode->setWidthPercent(100.f);
        if (i + 2 < total) rowNode->setMarginBottom(cellGap);  // 行间距；末行不设（尾距由 trimTrailingGap 清理）

        for (int col = 0; col < 2 && i + col < total; col++) {
            auto* cell = cells[i + col];
            auto owned = grid->detachChild(cell);      // 从 grid 摘下，解除父子挂接
            adoptChild(rowNode, owned.release());      // 改挂到本行（所有权随之转移）
            // 关键：等分布局 —— 基础宽度归零 + flexGrow 强制两列严格等宽（而非按内容长短分配）。
            // 这样右列右缘必然贴合行右缘，双栏总宽与单栏 100% 一致；列缝由左列的 marginLeft 提供。
            cell->setWidthPercent(0.f);
            cell->setFlexGrow(1.f);
            cell->setFlexShrink(1.f);                  // 窄时允许收缩，不溢出
            cell->setMarginLeft(col == 1 ? cellGap : 0.f);  // 列间留缝
            cell->setMarginBottom(0.f);                // 行间距改由 rowNode 的 marginBottom 承担
            cell->setHeight(SettingTheme::gridItemH);  // 与单栏紧凑行等高
        }
    }
}

// ============================================================================
//  Toggle Row：Switch 行
// ============================================================================
constexpr float    kToggleH{ 24.f };   // 胶囊开关高度（比 ctrlH 矮，行的右内边距要按它算才与上下留白一致）
// 轨道色随主题（关：浅色 E5E5E5 / 深色 3A3A3A；开：0FDC78）—— 见 SettingTheme

void styleToggle(Ling::Button* btn, bool on)
{
    // 44×24 轨道 + 20×20 滑块；无描边。轨道：关闭 #E5E5E5(浅)/#424242(深)，开启恒为 #0FDC78；
    // 滑块恒为纯白（两种状态、两种主题都一样），悬停不产生任何颜色变化。
    // 半径 12 是轨道整圆（胶囊形），不参与"圆角统一 10"。
    btn->setSize(44.f, kToggleH);
    btn->setBorderRadius(12.f);
    btn->setBorderWidth(0.f);
    btn->setText(L"");
    btn->setPadding(0, 0, 0, 0);
    const uint32_t track = on ? SettingTheme::toggleTrackOn : SettingTheme::toggleTrackOff;
    btn->setBg(track);
    // 悬停色与常态色取同一值：Button 在悬停期间 setBg 只写缓存不刷新 visual，
    // 必须靠 setHoverBg 立即落笔，同时保证鼠标进出不产生任何颜色变化。
    btn->setHoverBg(track);
    Ling::Node* thumb = nullptr;
    if (btn->children.size() >= 2) thumb = btn->children.back().get();
    if (!thumb) {
        thumb = btn->makeChild<Ling::Node>();
        thumb->setSize(20.f, 20.f);
        thumb->setBorderRadius(10.f);
        thumb->setPositionType(Ling::Position::Absolute);
    }
    thumb->setBorderWidth(0.f);              // 兼容旧构建已带描边的滑块
    thumb->setBg(0xFFFFFFFF);                // 纯白：开/关、浅色/深色都一样
    thumb->setPosition(Ling::Edge::Left, on ? 22.f : 2.f);
    thumb->setPosition(Ling::Edge::Top,  2.f);   // (24-20)/2=2，垂直居中
}

Ling::Button* toggleRow(Ling::Node* parent,
                        const std::wstring& label,
                        const std::wstring& desc,
                        bool on,
                        std::function<void(bool)> onChange,
                        bool chrome)
{
    auto r = optionRow(parent, label, desc, chrome, kToggleH);   // 右内边距按开关高度算，与上下留白一致
    auto* btn = r.ctrlBox->makeChild<Ling::Button>();
    styleToggle(btn, on);
    btn->onClick.add([btn, on, onChange](Ling::Button*) mutable {
        on = !on;
        styleToggle(btn, on);
        if (onChange) onChange(on);
    });
    return btn;
}

// ============================================================================
//  Select Row：Dropdown Select 行（带左侧 label/desc）
// ============================================================================
Ling::Button* selectDdl(Ling::Node* parent,
                        const std::vector<std::wstring>& options,
                        int current,
                        std::function<void(int, const std::wstring&)> onChange)
{
    if (current < 0 || current >= (int)options.size()) current = 0;
    auto* btn = parent->makeChild<Ling::Button>();
    btn->setHeight(SettingTheme::ctrlH);
    btn->setBorderRadius(SettingTheme::radiusCtl);   // 与所在行的外框(10)同心
    btn->setBorder(1.f, SettingTheme::input);
    btn->setBg(SettingTheme::inputBg);           // 常态：与卡片同底（不额外加底色）
    btn->setColor(SettingTheme::popupFg);
    btn->setHoverBg(SettingTheme::popupSelBg);   // 底色只在停悬时出现（浅色 F5F4F4 / 深色 424242）
    btn->setHoverColor(SettingTheme::popupFg);
    btn->setFlexDirection(Ling::FlexDirection::Row);
    btn->setJustifyContent(Ling::Justify::Start);
    btn->setAlignItems(Ling::Align::Center);
    btn->setPadding(10.f, 0, 10.f, 0);
    btn->setText(L"");   // 文本与图标由子 Label 控制，需不同字体族

    // 选中值（左，普通字体，宽度不足可收缩截断）
    auto* valueLab = btn->makeChild<Ling::Label>();
    valueLab->setFontSize(SettingTheme::fontBase);
    valueLab->setColor(SettingTheme::popupFg);
    valueLab->setFlexShrink(1.f);
    if (!options.empty()) valueLab->setText(options[current]);

    // 弹性空白：把下拉图标推到按钮最右侧
    auto* spacer = btn->makeChild<Ling::Node>();
    spacer->setFlexGrow(1.f);
    spacer->setFlexShrink(1.f);

    // 下拉图标（右，iconfont）
    auto* iconLab = btn->makeChild<Ling::Label>();
    iconLab->setText(Icon::Dropdown);
    iconLab->setFontFamily(Icon::Family);
    iconLab->setFontSize(Icon::DropSize);
    iconLab->setColor(SettingTheme::textMuted);
    iconLab->setFlexShrink(0.f);
    iconLab->setMarginLeft(6.f);

    bindPopup(btn, options, current, onChange,
        [valueLab](const std::wstring& t) { valueLab->setText(t); });
    return btn;
}

Ling::Button* selectRow(Ling::Node* parent,
                        const std::wstring& label,
                        const std::wstring& desc,
                        const std::vector<std::wstring>& options,
                        int current,
                        std::function<void(int, const std::wstring&)> onChange,
                        bool chrome)
{
    auto r = optionRow(parent, label, desc, chrome);
    auto* btn = selectDdl(r.ctrlBox, options, current, onChange);
    // 触发按钮宽度与弹出项统一为 dropdownWidth(160)，保证二者等宽对齐。
    btn->setWidth(SettingTheme::dropdownWidth);
    btn->setFlexShrink(0.f);
    return btn;
}

// ============================================================================
//  Text Row：单行 Input 行（带左侧 label/desc）
// ============================================================================
Ling::TextBox* textRow(Ling::Node* parent,
                       const std::wstring& label,
                       const std::wstring& desc,
                       const std::wstring& value,
                       const std::wstring& placeholder,
                       std::function<void(const std::wstring&)> onCommit,
                       bool chrome)
{
    auto r = optionRow(parent, label, desc, chrome);
    auto* box = r.ctrlBox->makeChild<Ling::TextBox>();
    box->setWidth(220.f);
    box->setFlexShrink(0.f);
    box->setFlexGrow(1.f);
    box->setHeight(SettingTheme::ctrlH);
    box->setBorderRadius(SettingTheme::radiusCtl);   // 与所在行的外框(10)同心
    box->setBorder(1.f, SettingTheme::input);
    box->setBg(SettingTheme::inputBg);
    box->setColor(SettingTheme::textPrimary);
    box->setPadding(SettingTheme::sp2, SettingTheme::sp1, SettingTheme::sp2, SettingTheme::sp1);
    box->setFontSize(SettingTheme::fontBase);
    box->setVerticalCenter(true);
    box->setPlaceholder(placeholder);
    box->setPlaceholderColor(SettingTheme::placeholder);
    box->setText(value);
    box->onFocusChanged.add([onCommit](Ling::TextBox* tb, bool focused) {
        if (!focused && onCommit) onCommit(tb->getText());
    });
    return box;
}

// ============================================================================
//  Textarea Field（多行 Prompt 大文本框，独立 block）
// ============================================================================
Ling::TextBox* textareaField(Ling::Node* parent,
                             const std::wstring& label,
                             const std::wstring& hint,
                             const std::wstring& value,
                             std::function<void(const std::wstring&)> onCommit)
{
    auto* wrap = parent->makeChild<Ling::Node>();
    wrap->setFlexDirection(Ling::FlexDirection::Column);
    wrap->setWidthPercent(100.f);
    wrap->setBg(SettingTheme::card);
    wrap->setBorder(1.f, SettingTheme::border);
    wrap->setBorderRadius(SettingTheme::radiusLg);
    wrap->setPadding(SettingTheme::cardPadX, SettingTheme::cardPadY,
                     SettingTheme::cardPadX, SettingTheme::cardPadY);
    wrap->setMarginBottom(SettingTheme::blockGap);

    auto* lab = wrap->makeChild<Ling::Label>();
    lab->setText(label);
    lab->setFontSize(SettingTheme::fontXl);
    lab->setColor(SettingTheme::textPrimary);
    lab->setMarginBottom(SettingTheme::sp1);
    if (!hint.empty()) {
        auto* h = wrap->makeChild<Ling::Label>();
        h->setText(hint);
        h->setFontSize(SettingTheme::fontSm);
        h->setColor(SettingTheme::textSecondary);
        h->setMarginBottom(SettingTheme::sp3);
    }
    auto* box = wrap->makeChild<Ling::TextBox>();
    box->setHeight(96.f);
    box->setWidthPercent(100.f);
    box->setBorderRadius(SettingTheme::radiusSm);
    box->setBorder(1.f, SettingTheme::input);
    box->setBg(SettingTheme::inputBg);
    box->setColor(SettingTheme::textPrimary);
    box->setPadding(SettingTheme::sp3, SettingTheme::sp2, SettingTheme::sp3, SettingTheme::sp2);
    box->setFontSize(SettingTheme::fontSm);
    box->setText(value);
    box->onFocusChanged.add([onCommit](Ling::TextBox* tb, bool focused) {
        if (!focused && onCommit) onCommit(tb->getText());
    });
    return box;
}

// ============================================================================
//  Input Field（独立文本输入框，带 label + hint）
// ============================================================================
Ling::TextBox* inputField(Ling::Node* parent,
                          const std::wstring& label,
                          const std::wstring& hint,
                          const std::wstring& value,
                          const std::wstring& placeholder,
                          std::function<void(const std::wstring&)> onCommit,
                          float height)
{
    auto* wrap = parent->makeChild<Ling::Node>();
    wrap->setFlexDirection(Ling::FlexDirection::Column);
    wrap->setWidthPercent(100.f);
    wrap->setMarginBottom(SettingTheme::blockGap);

    if (!label.empty()) {
        auto* lab = wrap->makeChild<Ling::Label>();
        lab->setText(label);
        lab->setFontSize(SettingTheme::fontBase);
        lab->setColor(SettingTheme::textPrimary);
        lab->setMarginBottom(SettingTheme::sp1);
    }
    if (!hint.empty()) {
        auto* h = wrap->makeChild<Ling::Label>();
        h->setText(hint);
        h->setFontSize(SettingTheme::fontXs);
        h->setColor(SettingTheme::textSecondary);
        h->setMarginBottom(SettingTheme::sp2);
    }
    auto* box = wrap->makeChild<Ling::TextBox>();
    float h = height > 0.f ? height : SettingTheme::ctrlH;
    box->setHeight(h);
    box->setWidthPercent(100.f);
    box->setBorderRadius(SettingTheme::radiusSm);
    box->setBorder(1.f, SettingTheme::input);
    box->setBg(SettingTheme::inputBg);
    box->setColor(SettingTheme::textPrimary);
    box->setPadding(SettingTheme::sp2, SettingTheme::sp1, SettingTheme::sp2, SettingTheme::sp1);
    box->setFontSize(SettingTheme::fontBase);
    box->setVerticalCenter(true);
    box->setPlaceholder(placeholder);
    box->setPlaceholderColor(SettingTheme::placeholder);
    box->setText(value);
    box->onFocusChanged.add([onCommit](Ling::TextBox* tb, bool focused) {
        if (!focused && onCommit) onCommit(tb->getText());
    });
    return box;
}

// ============================================================================
//  Sub Tabs：shadcn Tabs 样式
//  variant="solid"（默认）: active = primary / inactive = white+border
// ============================================================================
void styleSubTab(Ling::Button* btn, bool on)
{
    btn->setBorderRadius(SettingTheme::radiusLg);
    if (on) {
        btn->setBorder(1.f, SettingTheme::primary);
        btn->setBg(SettingTheme::primary);
        btn->setColor(SettingTheme::primaryForeground);
        btn->setHoverBg(SettingTheme::primary);
        btn->setHoverColor(SettingTheme::primaryForeground);
    } else {
        btn->setBorder(1.f, SettingTheme::border);
        btn->setBg(SettingTheme::card);
        btn->setColor(SettingTheme::textPrimary);
        btn->setHoverBg(SettingTheme::accent);
        btn->setHoverColor(SettingTheme::textPrimary);
    }
}

std::vector<Ling::Button*> subTabs(Ling::Node* parent,
                                    const std::vector<std::wstring>& labels,
                                    int current,
                                    std::function<void(int)> onChange,
                                    std::function<void(Ling::Node* row)> onRight)
{
    std::vector<Ling::Button*> btns;
    auto* row = parent->makeChild<Ling::Node>();
    row->setFlexDirection(Ling::FlexDirection::Row);
    row->setFlexWrap(Ling::Wrap::Wrap);
    row->setWidthPercent(100.f);
    row->setAlignItems(Ling::Align::Center);
    row->setMarginBottom(SettingTheme::blockGap);
    for (int i = 0; i < (int)labels.size(); i++) {
        auto* btn = row->makeChild<Ling::Button>();
        btn->setText(labels[i]);
        btn->setHeight(SettingTheme::ctrlH);
        btn->setPadding(SettingTheme::sp3, 0, SettingTheme::sp3, 0);
        if (i < (int)labels.size() - 1) btn->setMarginRight(SettingTheme::sp2);
        styleSubTab(btn, i == current);
        int idx = i;
        btn->onClick.add([idx, onChange](Ling::Button*) {
            if (onChange) onChange(idx);
        });
        btns.push_back(btn);
    }
    if (onRight) {
        auto* grow = row->makeChild<Ling::Node>();
        grow->setFlexGrow(1.f);
        grow->setFlexShrink(1.f);
        onRight(row);
    }
    return btns;
}

// ============================================================================
//  Segmented Pills：分段胶囊（选中段高亮卡片，自适应宽度）
//  布局：轨道 secondary(zinc100) + 常规圆角(radiusLg)；每段按内容自适应宽度。
//  选中段：primary(黑) 背景 + primaryForeground 文字；未选中：透明背景 + textPrimary。
//  交互：点击段落 → 即时切换选中段（无定时器/绝对定位滑动 → 无动画卡死与滑块丢失问题）。
// ============================================================================
namespace {

// 单个分段控制的状态（由 SegmentedPillsRef::state 持有；无定时器/动画，稳定无卡死）
struct SegPillsState {
    std::vector<Ling::Button*> btns;
    int count{ 0 };
    int current{ 0 };
};

// 切换选中段的视觉：选中段 = primary 底 + primaryForeground 字（浅色=深底白字 / 深色=浅底深字）；
// 未选中透明 + textPrimary 字（hover 微高亮）。与「外观设置」页的分段导航取同一对令牌，两处观感一致。
void styleSegBtn(Ling::Button* btn, bool on)
{
    if (on) {
        btn->setBorder(0.f, 0);
        btn->setBg(SettingTheme::primary);
        btn->setHoverBg(SettingTheme::primary);
        btn->setColor(SettingTheme::primaryForeground);
        btn->setHoverColor(SettingTheme::primaryForeground);
    } else {
        btn->setBorder(0.f, 0);
        btn->setBg(0);
        btn->setHoverBg(SettingTheme::accent);
        btn->setColor(SettingTheme::textPrimary);
        btn->setHoverColor(SettingTheme::textPrimary);
    }
}

} // namespace

SegmentedPillsRef segmentedPills(Ling::Node* parent,
    const std::vector<std::wstring>& labels,
    int current,
    std::function<void(int)> onChange)
{
    SegmentedPillsRef r{};
    if (labels.empty()) return r;

    const int count = (int)labels.size();
    if (current < 0) current = 0;
    if (current >= count) current = count - 1;

    // 轨道：secondary 底 + 常规圆角(radiusLg，非胶囊)；宽度由内容自适应（不等宽拉宽）
    auto* row = parent->makeChild<Ling::Node>();
    r.row = row;
    row->setFlexDirection(Ling::FlexDirection::Row);
    row->setHeight(SettingTheme::ctrlH);
    row->setBorderRadius(SettingTheme::radiusLg);
    row->setBg(SettingTheme::secondary);

    auto state = std::make_shared<SegPillsState>();
    state->count   = count;
    state->current = current;
    r.state = state;

    for (int i = 0; i < count; i++) {
        auto* btn = row->makeChild<Ling::Button>();
        btn->setText(labels[i]);
        btn->setHeight(SettingTheme::ctrlH);
        btn->setPadding(SettingTheme::sp4, 0, SettingTheme::sp4, 0);
        btn->setFontSize(SettingTheme::fontBase);
        btn->setBorderRadius(SettingTheme::radiusLg);
        btn->setFlexDirection(Ling::FlexDirection::Row);
        btn->setJustifyContent(Ling::Justify::Center);
        btn->setAlignItems(Ling::Align::Center);
        styleSegBtn(btn, i == current);

        int idx = i;
        btn->onClick.add([state, idx, onChange](Ling::Button*) {
            if (idx == state->current) return;
            state->current = idx;
            for (int j = 0; j < (int)state->btns.size(); j++)
                styleSegBtn(state->btns[j], j == idx);
            if (onChange) onChange(idx);
        });

        state->btns.push_back(btn);
    }

    // 程序化切换选中段：不触发 onChange（供外部状态同步，避免点击后递归）
    r.select = [state](int idx) {
        if (!state || idx < 0 || idx >= state->count) return;
        if (idx == state->current) return;
        state->current = idx;
        for (int j = 0; j < (int)state->btns.size(); j++)
            styleSegBtn(state->btns[j], j == idx);
    };

    r.btns = state->btns;
    return r;
}

// ============================================================================
//  Card：通用白底卡片容器（不带 header，纯 content box）
// ============================================================================
Ling::Node* card(Ling::Node* parent)
{
    auto* c = parent->makeChild<Ling::Node>();
    c->setFlexDirection(Ling::FlexDirection::Column);
    c->setWidthPercent(100.f);
    c->setBg(SettingTheme::card);
    c->setBorder(1.f, SettingTheme::border);
    c->setBorderRadius(SettingTheme::radiusLg);
    c->setPadding(SettingTheme::cardPadX, SettingTheme::cardPadY,
                  SettingTheme::cardPadX, SettingTheme::cardPadY);
    c->setMarginBottom(SettingTheme::blockGap);
    return c;
}

// ============================================================================
//  Pill / Badge：标签胶囊（selected=default / unselected=outline）
// ============================================================================
void stylePill(Ling::Button* btn, bool selected)
{
    btn->setBorderRadius(SettingTheme::radiusFull);
    if (selected) {
        btn->setBorder(1.f, SettingTheme::primary);
        btn->setBg(SettingTheme::primary);
        btn->setColor(SettingTheme::primaryForeground);
        btn->setHoverBg(SettingTheme::primary);
        btn->setHoverColor(SettingTheme::primaryForeground);
    } else {
        btn->setBorder(1.f, SettingTheme::border);
        btn->setBg(SettingTheme::card);
        btn->setColor(SettingTheme::textPrimary);
        btn->setHoverBg(SettingTheme::accent);
        btn->setHoverColor(SettingTheme::textPrimary);
    }
}

Ling::Button* pill(Ling::Node* row, const std::wstring& label, bool selected)
{
    auto* btn = row->makeChild<Ling::Button>();
    btn->setText(label);
    btn->setHeight(SettingTheme::ctrlSm);
    btn->setPadding(SettingTheme::sp3, 0, SettingTheme::sp3, 0);
    btn->setMargin(0.f, 0.f, SettingTheme::sp2, SettingTheme::sp1 + SettingTheme::sp2);
    stylePill(btn, selected);
    return btn;
}

// ============================================================================
//  Dashed Add：虚线添加按钮
// ============================================================================
Ling::Button* dashedAdd(Ling::Node* parent, const std::wstring& label,
                        std::function<void()> onClick)
{
    auto* btn = parent->makeChild<Ling::Button>();
    // 文字仅标签、无 "＋" 前缀，字号与正文一致
    btn->setText(label);
    btn->setHeight(40.f);
    btn->setWidthPercent(100.f);
    btn->setBorderRadius(SettingTheme::radiusLg);
    btn->setBorder(1.f, SettingTheme::border);
    btn->setBorderColor(SettingTheme::input);
    btn->setBg(0);
    btn->setColor(SettingTheme::textSecondary);
    btn->setHoverBg(SettingTheme::accent);
    btn->setHoverColor(SettingTheme::textPrimary);
    btn->setHoverBorderColor(SettingTheme::border);
    btn->setMarginBottom(SettingTheme::blockGap);
    btn->onClick.add([onClick](Ling::Button*) { if (onClick) onClick(); });
    return btn;
}

// ============================================================================
//  shadcn Button 四变体
// ============================================================================
Ling::Button* btnPrimary(Ling::Node* parent, const std::wstring& text,
                         std::function<void()> onClick, float w)
{
    return makeBtn(parent, text, onClick, w, BtnVariant::Primary);
}
Ling::Button* btnOutline(Ling::Node* parent, const std::wstring& text,
                         std::function<void()> onClick, float w)
{
    return makeBtn(parent, text, onClick, w, BtnVariant::Outline);
}
Ling::Button* btnGhost(Ling::Node* parent, const std::wstring& text,
                       std::function<void()> onClick, float w)
{
    return makeBtn(parent, text, onClick, w, BtnVariant::Ghost);
}
Ling::Button* btnDestructive(Ling::Node* parent, const std::wstring& text,
                             std::function<void()> onClick, float w)
{
    return makeBtn(parent, text, onClick, w, BtnVariant::Destructive);
}

// ============================================================================
//  Callout：提示卡（shadcn Callout variant=info/success/warning/destructive）
// ============================================================================
Ling::Node* callout(Ling::Node* parent,
                    const std::wstring& message,
                    CalloutVariant variant,
                    const std::wstring& title)
{
    auto* wrap = parent->makeChild<Ling::Node>();
    wrap->setFlexDirection(Ling::FlexDirection::Column);
    wrap->setWidthPercent(100.f);
    wrap->setBorderRadius(SettingTheme::radiusMd);
    wrap->setPadding(SettingTheme::cardPadX, SettingTheme::cardPadY,
                     SettingTheme::cardPadX, SettingTheme::cardPadY);
    wrap->setMarginBottom(SettingTheme::blockGap);

    uint32_t bgC   = SettingTheme::secondary;
    uint32_t brdC  = SettingTheme::border;
    uint32_t fgC   = SettingTheme::textPrimary;
    switch (variant) {
    case CalloutVariant::Success:
        bgC   = SettingTheme::successBg;
        brdC  = SettingTheme::successBorder;
        fgC   = SettingTheme::successFg;
        break;
    case CalloutVariant::Warning:
        bgC   = SettingTheme::warningBg;
        brdC  = SettingTheme::warningBorder;
        fgC   = SettingTheme::warningFg;
        break;
    case CalloutVariant::Destructive:
        bgC   = SettingTheme::destructiveBg;
        brdC  = SettingTheme::destructiveBorder;
        fgC   = SettingTheme::destructiveFg;
        break;
    case CalloutVariant::Info:
    default:
        bgC   = SettingTheme::infoBg;
        brdC  = SettingTheme::infoBorder;
        fgC   = SettingTheme::infoFg;
        break;
    }
    wrap->setBg(bgC);
    wrap->setBorder(1.f, brdC);

    if (!title.empty()) {
        auto* t = wrap->makeChild<Ling::Label>();
        t->setText(title);
        t->setFontSize(SettingTheme::fontXl);
        t->setColor(fgC);
        t->setMarginBottom(SettingTheme::sp1);
    }
    auto* m = wrap->makeChild<Ling::Label>();
    m->setText(message);
    m->setFontSize(SettingTheme::fontBase);
    m->setColor(fgC);
    m->setFlexGrow(1.f);
    return wrap;
}

// ============================================================================
//  ConfirmDialog：确认弹窗（压暗底 + 居中卡片 + 取消/确定）
// ============================================================================
namespace {
// 对 BGRA 做一次「先横后纵」的盒式模糊（滑动窗口，O(w*h) 与半径无关）；
// 迭代两次就非常接近高斯，且不改变分辨率 —— 绝不能靠"降采样再放大"来糊，
// 放大插值的折点会在文字上留下一层网格/十字纹。
void boxBlurBGRA(std::vector<BYTE>& img, int w, int h, int radius)
{
    if (radius < 1 || w < 2 || h < 2) return;
    std::vector<BYTE> tmp(img.size());
    // 横向（读 img 写 tmp）
    for (int y = 0; y < h; ++y) {
        BYTE* row = img.data() + (size_t)y * w * 4;
        BYTE* dst = tmp.data() + (size_t)y * w * 4;
        for (int c = 0; c < 3; ++c) {
            int lo = 0, hi = -1, sum = 0, cnt = 0;
            for (int x = 0; x < w; ++x) {
                const int wantHi = (std::min)(w - 1, x + radius);
                const int wantLo = (std::max)(0, x - radius);
                while (hi < wantHi) { ++hi; sum += row[(size_t)hi * 4 + c]; ++cnt; }
                while (lo < wantLo) { sum -= row[(size_t)lo * 4 + c]; --cnt; ++lo; }
                dst[(size_t)x * 4 + c] = (BYTE)(sum / (cnt > 0 ? cnt : 1));
            }
        }
        for (int x = 0; x < w; ++x) dst[(size_t)x * 4 + 3] = 255;
    }
    // 纵向（读 tmp 写 img）
    for (int x = 0; x < w; ++x) {
        for (int c = 0; c < 3; ++c) {
            int lo = 0, hi = -1, sum = 0, cnt = 0;
            for (int y = 0; y < h; ++y) {
                const int wantHi = (std::min)(h - 1, y + radius);
                const int wantLo = (std::max)(0, y - radius);
                while (hi < wantHi) { ++hi; sum += tmp[((size_t)hi * w + x) * 4 + c]; ++cnt; }
                while (lo < wantLo) { sum -= tmp[((size_t)lo * w + x) * 4 + c]; --cnt; ++lo; }
                img[((size_t)y * w + x) * 4 + c] = (BYTE)(sum / (cnt > 0 ? cnt : 1));
            }
        }
    }
    for (size_t i = 3; i < img.size(); i += 4) img[i] = 255;
}
}   // namespace

ConfirmDialog* ConfirmDialog::s_open = nullptr;

ConfirmDialog::ConfirmDialog(Ling::WinBase* host, std::wstring message,
    std::wstring confirmText, std::wstring cancelText, std::function<void(bool)> onResult)
    : host(host), message(std::move(message)),
    confirmText(std::move(confirmText)), cancelText(std::move(cancelText)),
    onResult(std::move(onResult))
{
    if (host) dpi = host->dpi;
    createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, WS_POPUP);
}

ConfirmDialog::~ConfirmDialog()
{
    // host 在宿主销毁路径里会被置空（那一路的 onDestroy 回调），这里只在宿主还活着时解绑
    if (host) host->onDestroy.remove(hostDestroyTok);
    if (s_open == this) s_open = nullptr;
    // WinBase 析构不销毁 HWND：不 close 就析构会留下"幽灵窗"，WndProc 重入已释放对象
    if (hwnd) { close(); hwnd = nullptr; }
}

bool ConfirmDialog::isOpen()
{
    return s_open != nullptr;
}

void ConfirmDialog::ask(Ling::WinBase* host, const std::wstring& message,
    std::function<void(bool)> onResult, std::wstring confirmText, std::wstring cancelText)
{
    if (s_open) return;   // 同一时刻只弹一个
    if (confirmText.empty()) confirmText = Lang::get(L"setting.confirm");
    if (cancelText.empty()) cancelText = Lang::get(L"setting.cancel");
    auto* dlg = new ConfirmDialog(host, message, std::move(confirmText),
        std::move(cancelText), std::move(onResult));
    s_open = dlg;
    dlg->placeOverHost();
    dlg->prepareBackdrop();   // 必须在 show() 之前抓：晚了就抓到弹窗自己
    dlg->show();
    dlg->refresh();
    if (dlg->hwnd) {
        SetForegroundWindow(dlg->hwnd);
        SetFocus(dlg->hwnd);      // 拿到键盘焦点，Esc 才生效
    }
}

void ConfirmDialog::placeOverHost()
{
    if (!host || !host->hwnd || !hwnd) return;
    RECT rc{};
    GetClientRect(host->hwnd, &rc);
    POINT tl{ rc.left, rc.top };
    ClientToScreen(host->hwnd, &tl);
    const float d = dpi > 0.f ? dpi : 1.f;
    setSize((rc.right - rc.left) / d, (rc.bottom - rc.top) / d);
    setPosition(tl.x, tl.y);
    // owned 窗口：始终盖在宿主之上
    SetWindowLongPtr(hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(host->hwnd));
}

void ConfirmDialog::onCreated()
{
    // 居中：body 定为「主轴/交叉轴都居中」，卡片作为普通子节点即落在正中
    body->setFlexDirection(Ling::FlexDirection::Row);
    body->setJustifyContent(Ling::Justify::Center);
    body->setAlignItems(Ling::Align::Center);

    // 毛玻璃背景：自绘画布铺满整窗，layout() 里把模糊图拉满画上去
    blurCanvas = body->makeChild<Ling::Canvas>();
    blurCanvas->setPositionType(Ling::Position::Absolute);
    blurCanvas->setPosition(Ling::Edge::Left, 0.f);
    blurCanvas->setPosition(Ling::Edge::Top, 0.f);
    blurCanvas->setPosition(Ling::Edge::Right, 0.f);
    blurCanvas->setPosition(Ling::Edge::Bottom, 0.f);

    // 压暗层：叠在模糊之上（点击它 = 点卡片外 = 取消）
    auto* dim = body->makeChild<Ling::Node>();
    dim->setPositionType(Ling::Position::Absolute);
    dim->setPosition(Ling::Edge::Left, 0.f);
    dim->setPosition(Ling::Edge::Top, 0.f);
    dim->setPosition(Ling::Edge::Right, 0.f);
    dim->setPosition(Ling::Edge::Bottom, 0.f);
    dim->setBg(0x00000059);

    card = body->makeChild<Ling::Node>();
    card->setWidth(320.f);
    card->setFlexDirection(Ling::FlexDirection::Column);
    card->setBg(SettingTheme::card);
    card->setBorder(1.f, SettingTheme::border);
    card->setBorderRadius(SettingTheme::radiusLg);
    card->setPadding(20.f, 18.f, 20.f, 18.f);

    auto* msg = card->makeChild<Ling::Label>();
    msg->setText(message);
    msg->setFontSize(SettingTheme::fontBase);
    msg->setColor(SettingTheme::textPrimary);
    msg->setWidthPercent(100.f);
    msg->setJustifyContent(Ling::Justify::Start);
    msg->setAlignItems(Ling::Align::FlexStart);

    auto* row = card->makeChild<Ling::Node>();
    row->setFlexDirection(Ling::FlexDirection::Row);
    row->setWidthPercent(100.f);
    row->setJustifyContent(Ling::Justify::End);
    row->setAlignItems(Ling::Align::Center);
    row->setMarginTop(18.f);

    if (!cancelText.empty()) {
        auto* cancel = btnOutline(row, cancelText, [this]() { this->finish(false); }, 80.f);
        cancel->setMarginRight(8.f);
    }
    btnPrimary(row, confirmText, [this]() { this->finish(true); }, 80.f);

    downTok = onMouseDown.add([this](POINT pos, bool) {
        if (card && !card->isPosIn(pos)) this->finish(false);
    });
    keyTok = onKeyDown.add([this](UINT key) {
        if (key == VK_ESCAPE) this->finish(false);
    });

    // 宿主销毁时跟着收掉：不执行回调（调用方页面也在销毁），并置空 host 免得析构里回访
    if (host) {
        hostDestroyTok = host->onDestroy.add([this]() {
            if (done) return;
            done = true;
            if (s_open == this) s_open = nullptr;
            onResult = nullptr;
            host = nullptr;
            close();
            auto self = this;
            if (auto* app = Ling::App::get()) app->dq.TryEnqueue([self]() { delete self; });
            else delete self;
        });
    }
}

void ConfirmDialog::layout()
{
    Ling::WinBase::layout();
    if (!blurCanvas || !blurBmp) return;
    auto ctx = blurCanvas->startPaint();
    if (!ctx) return;
    ctx->Clear(0);
    // 小图拉满整窗：线性插值把降采样后的色块糊开，得到毛玻璃观感
    ctx->DrawBitmap(blurBmp.Get(), D2D1::RectF(0.f, 0.f, w, h), 1.f,
        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    blurCanvas->finishPaint();
}

void ConfirmDialog::prepareBackdrop()
{
    if (!host || !host->hwnd) return;
    RECT rc{};
    GetClientRect(host->hwnd, &rc);
    POINT tl{ rc.left, rc.top };
    ClientToScreen(host->hwnd, &tl);
    const int cw = rc.right - rc.left, chh = rc.bottom - rc.top;
    if (cw <= 1 || chh <= 1) return;
    auto pix = Util::captureScreen(tl.x, tl.y, cw, chh);
    if (pix.size() < (size_t)cw * (size_t)chh * 4) return;

    // 真·模糊：可分离盒式模糊迭代两遍（半径按 dpi 缩放，基准 10），
    // 原分辨率 1:1 画回去，不做任何放大插值
    const int radius = (std::max)(2, (int)(10.0 * dpi + 0.5));
    boxBlurBGRA(pix, cw, chh, radius);
    boxBlurBGRA(pix, cw, chh, radius);

    D2D1_BITMAP_PROPERTIES1 prop{
        .pixelFormat{ DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE },
        .dpiX{ 96.0f }, .dpiY{ 96.0f },
        .bitmapOptions{ D2D1_BITMAP_OPTIONS_NONE }
    };
    Ling::D2D::get()->deviceContext->CreateBitmap(
        D2D1::SizeU((UINT32)cw, (UINT32)chh), pix.data(), (UINT32)cw * 4, &prop,
        blurBmp.GetAddressOf());
}

void ConfirmDialog::finish(bool ok)
{
    if (done) return;
    done = true;
    if (s_open == this) s_open = nullptr;
    auto cb = std::move(onResult);
    onResult = nullptr;
    close();                    // 先让弹窗消失
    if (cb) cb(ok);             // 再回调（调用方可能重建页面 / 删条目）
    // 延后销毁：finish 常常跑在某个按钮自己的 onClick 调用栈里，同步 delete 会当场把该按钮拆掉
    auto self = this;
    if (auto* app = Ling::App::get()) app->dq.TryEnqueue([self]() { delete self; });
    else delete self;
}

} // namespace SettingUi
