#pragma once
#include <include/Ling.h>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <windows.h>

namespace SettingUi { class Popup; class HoverTip; }

class ImageEditCollage;
class ImageEditMerge;
class ImageEditAnnotHost;
class ShapeBase;

// 图片编辑窗口：左列画布 + 底部素材条（拼图模式）+ 右列模式胶囊/尺寸行/属性区/保存。
// 画布与文档内容分别由 ImageEditCollage（拼图）与 ImageEditMerge（合并文档）承载，
// 本窗口只负责外壳、模式切换、尺寸行与入口。
class WinImageEdit : public Ling::WinBase
{
public:
    enum class Mode { Collage = 0, DocMerge = 1 };

    ~WinImageEdit();

    static void init();
    static void dispose();
    static WinImageEdit* get();

    Mode mode() const { return mode_; }
    void setMode(Mode mode);
    // 用指定图片新建一张编辑画布（供插件右键「图片编辑」使用）。
    void openImage(const std::wstring& path);
    // 导入图片，并把贴图里已有的矢量标注一起搬进来（可在编辑器里继续选中/修改）。
    // shapes 使用图片像素坐标；width/height 是图片像素尺寸。
    void importImageWithShapes(const std::wstring& path,
        std::vector<std::unique_ptr<ShapeBase>> shapes, int width, int height);
    bool hasContent() const;
    // 当前是否处于「贴图导入 + 编辑器激活」的置顶状态（标注属性栏据此跟随）
    bool pinAwareTopmostActive() const;

    // —— 标注（工具属性栏 / 画布标注）——
    // 客户区坐标 → 画板像素 / 画板像素 → 客户区（标注坐标 = 画板内像素）
    POINT annotClientToImage(POINT p) const;
    D2D1_POINT_2F annotImageToClient(float ax, float ay) const;
    D2D1_RECT_F annotArtRect() const;
    // 底部工具栏的屏幕矩形；btnCenterX 返回当前工具按钮中心相对工具栏左边的距离
    RECT toolbarScreenRect(float* btnCenterX = nullptr) const;

    // 收起当前标注工具（右键逐层退出的最后一层）：工具栏选中态与属性栏一起复位
    void exitAnnotTool();

    // 文件拖放（WM_DROPFILES 子类化后转发到这里，clientPos 为窗口客户区坐标）
    void onFilesDropped(const std::vector<std::wstring>& paths, POINT clientPos);
    // Ctrl+V：把系统剪贴板里的图片贴进画布（位图或资源管理器里复制的图片文件）。
    // 内部会转成文件路径再走 onFilesDropped，和拖放/点击导入是同一条路。
    // 返回 false = 剪贴板里没有图片（或当前正在输入文字，这次 Ctrl+V 归输入框）。
    bool pasteFromClipboard();

private:
    WinImageEdit();
    void onCreated() override;

    void buildUi();
    void buildLeftColumn();
    void buildRightColumn();
    void buildToolbar();
    void buildTopCapsule();
    void buildCollageProps();
    void buildDocProps();

    // 右侧拼图属性区
    struct NumProp {
        Ling::TextBox* box{ nullptr };
        Ling::Slider* slider{ nullptr };
        float value{ 0.f };
    };
    Ling::Node* buildPropRow(Ling::Node* parent, const std::wstring& label, float top,
        float minV, float maxV, NumProp& out, std::function<void(float)> apply,
        const wchar_t* unitText = L"PX");
    Ling::Node* buildFieldRow(Ling::Node* parent, const std::wstring& caption,
        float left, float top, float width, Ling::TextBox*& outBox, Ling::Label** outLabel = nullptr,
        Ling::Label** outUnit = nullptr, Ling::Button** outUnitHit = nullptr);
    Ling::Node* addDivider(Ling::Node* parent, float top);
    // 一组背景色块的外环引用（无背景 / 5 个预设色 / 自定义），选中态画在外环上
    struct BgSwatchRefs {
        std::vector<Ling::Node*> rings;
        Ling::Node* none{ nullptr };
        Ling::Node* custom{ nullptr };
        Ling::Label* noneIcon{ nullptr };   // 选中「无色」时图标本身变绿、不描边
    };
    // 背景色板：无色 + 5 个预设色 + 自定义（彩虹），几何按截图
    void buildSwatchRow(Ling::Node* parent, float top, BgSwatchRefs& out,
        std::function<void(uint32_t)> applyColor);
    Ling::Button* buildLayoutCard(Ling::Node* parent, int preset);
    void setActiveTool(int index);
    void applyToolColors();        // 按当前选中工具/可用性刷新工具栏图标颜色（分割不可用时置灰）
    void updateToolAvailability(); // 选中变化时刷新分割工具可用性
    // 滚轮在输入框上微调：尺寸 / 行列 / 边框·间距·圆角（滚轮一次 ±1）
    void onWheelAdjust(POINT pos, float space);
    void bumpSizeBox(Ling::TextBox* box, int dir);
    void bumpCountBox(Ling::TextBox* box, int dir);
    std::vector<std::pair<Ling::TextBox*, Ling::Slider*>> propBoxes_;
    int splitToolIndex_{ -1 };
    bool splitAvailable_{ true };
    void syncCollageProps();
    // 背景色板：预设色/自定义色选中时外环加绿圈；「无色」选中时图标本身变绿
    void updateBgSwatches();

    // 右侧合并文档属性区
    void syncDocProps();
    // 文件格式下拉（合并文档 PDF/PPTX、拼图 PNG/WEBP 共用外观）
    Ling::Button* buildFormatDdl(Ling::Node* parent, float top, Ling::Label*& outValue);
    // 打开格式下拉：items 是显示文案，pick 收到选中下标（关弹窗后回调）
    void openFormatPopup(Ling::Button* anchor, std::vector<std::wstring> items, int current,
        std::function<void(int)> pick);
    void closeFormatPopup();

    // 图层面板
    void buildCanvasBar();          // 画布左上角小工具栏：图层 / 画板
    void buildLayerPanel();         // 左侧浮层图层面板（默认收起）
    void updateLayerPanelBounds();  // 窗口尺寸变化后重算面板高度
    void rebuildLayers();           // 内容变化：重建图层项
    void applyLayerStyles();        // 只刷新选中描边（结构没变时不重建节点，避免闪烁）
    void updateLayerSelection();    // 刷新选中态，并把选中项滚动到可见
    void setLayerPanelOpen(bool open);
    void updateBoardButton();       // 画板按钮的锁定态配色
    float layerMaxScroll() const;
    void setLayerScroll(float y);
    int layerIndexAt(POINT pos) const;   // 命中图层行，-1 = 未命中
    int layerPlusIndexAt(POINT pos) const;   // 命中空格子的「+」热区，-1 = 未命中
    void setLayerPlusHover(int index);
    void toggleLayerGroup(int group);
    void ensureLayerVisible(int cell);
    void onLayerMouseDown(POINT pos, bool right);
    void onLayerMouseMove(POINT pos);
    void onLayerMouseUp(POINT pos, bool right);

    // 顶部胶囊 / 历史按钮
    void updateCapsule();
    void updateHistoryButtons();

    void rebuildForMode();       // 销毁旧模式页面，按 mode_ 新建 ImageEditCollage / ImageEditMerge
    void applyModeUi();          // 素材条/工具栏/属性区显隐、模式胶囊、尺寸行
    void updateModeCapsule();
    void updateSizeRow();
    void updateSizeUnit(bool percent);   // 只改单位标签的文案/颜色（缓存状态，避免重复重排）
    void updateEmptyState();
    void switchModeDeferred(Mode mode);
    // source = 触发提交的输入框（只应用该轴；nullptr = 两个轴都按当前文本应用）
    void syncCanvasSizeFromEditors(Ling::TextBox* source = nullptr);
    void syncAnnotContent();     // 画板内容 → 标注宿主底图（滤镜取样 / 视口范围）

    void onSaveClicked();        // M1 先留空实现
    void onCancelClicked();      // 关闭窗口

    // 贴图导入的编辑器要能盖住置顶贴图：激活时临时置顶，切走后恢复普通层
    void applyPinAwareTopmost(bool active);

    static LRESULT CALLBACK dropWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    static WinImageEdit* s_instance;
    static WNDPROC s_oldProc;
    unsigned long long uid_{ 0 };

    Mode mode_{ Mode::Collage };

    // 左列
    Ling::Node* leftCol_{ nullptr };
    Ling::Node* canvasHost_{ nullptr };   // #171717、圆角 20
    ImageEditCollage* collage_{ nullptr };
    ImageEditMerge* merge_{ nullptr };
    std::unique_ptr<ImageEditAnnotHost> annot_;   // 隐藏的标注宿主（工具属性栏 + 画布标注）
    Ling::Node* emptyRow_{ nullptr };        // 空态一行：提示文字 + 合并模式的「添加图片」按钮
    Ling::Label* emptyLabel_{ nullptr };
    Ling::Button* emptyAddBtn_{ nullptr };   // 合并模式空态的「添加图片」（M3 用）
    // 图层栏（浮层）：宽度 102，高度铺满工作区（底部留 19），纵向滚动
    Ling::Node* canvasBar_{ nullptr };
    Ling::Button* btnLayer_{ nullptr };
    Ling::Button* btnBoard_{ nullptr };
    Ling::Node* layerPanel_{ nullptr };
    Ling::Node* layerView_{ nullptr };     // 视口：直角裁剪
    Ling::Node* layerTrack_{ nullptr };    // 纵向内容轨道
    // 图层行：单格 84×55 / 布局组标题 84×22 / 组内图片 68×50
    struct LayerRow {
        int cell{ -1 };        // 代表格子（组 = 首个格）
        int group{ -1 };       // -1 = 单格
        int unit{ -1 };
        int kind{ 0 };         // 1 布局组 / 2 分割组（按自由布局）
        int layoutType{ 0 };   // 0 自由 / 1 上下 / 2 左右
        bool header{ false };
        bool empty{ false };
        bool selected{ false };
        bool unitAll{ false };  // 整个单位（组内全部格子）都被选中：整块描一个外框
        bool chevron{ false }; // 组内有图片才显示下拉
        bool expanded{ false };
        std::wstring path;
        float y{ 0.f };        // 内容坐标（逻辑像素）
        float h{ 0.f };
    };
    std::vector<LayerRow> layerRows_;
    std::vector<Ling::Node*> layerRowNodes_;
    std::vector<Ling::Node*> layerUnitBoxes_;   // 每个分组单位的大卡片（整组选中时改它的描边）
    std::vector<Ling::Node*> layerPlusNodes_;   // 与 layerRows_ 对应的「+」热区（非空卡片为 nullptr）
    int layerHoverPlus_{ -1 };
    std::vector<bool> layerUnitAll_;
    std::vector<std::wstring> layerSig_;        // 结构签名：一样就只更新样式
    int layerLastScrollCell_{ -1 };             // 上一次滚动定位到的格子（避免每帧重复滚动）
    std::vector<int> layerExpanded_;   // 已展开的布局组 id
    // 拖动排序：跟手的半透明卡片 + 插入光标（与合并文档的拖拽手感一致）
    Ling::Node* layerGhost_{ nullptr };
    Ling::ImageBox* layerGhostImg_{ nullptr };
    Ling::Node* layerCaret_{ nullptr };
    float layerContentH_{ 0.f };
    float layerScrollY_{ 0.f };
    bool layerPanelOpen_{ false };
    // 图层拖动排序
    bool layerPressed_{ false };
    bool layerDragMoved_{ false };
    POINT layerPressPos_{};
    int layerPressIndex_{ -1 };
    int layerDropSlot_{ -1 };          // 插入位置（行下标，== 行数表示放到最后）
    Ling::Node* toolbar_{ nullptr };      // 底部工具栏（宽度自适应）
    Ling::Node* toolbarRail_{ nullptr };  // 贴底轨道：水平居中工具栏
    std::vector<Ling::Button*> toolBtns_;
    std::vector<std::wstring> toolIds_;
    int activeTool_{ -1 };
    Ling::Button* btnUndo_{ nullptr };
    Ling::Button* btnReset_{ nullptr };
    Ling::Node* topCapsule_{ nullptr };
    std::vector<Ling::Button*> capsuleBtns_;
    bool capsuleLocked_{ false };

    // 右列
    Ling::Node* rightCol_{ nullptr };
    Ling::Button* btnModeCollage_{ nullptr };
    Ling::Button* btnModeDoc_{ nullptr };
    Ling::TextBox* editW_{ nullptr };
    Ling::TextBox* editH_{ nullptr };
    Ling::Label* unitW_{ nullptr };      // 尺寸行单位标签：PX / %
    Ling::Label* unitH_{ nullptr };
    Ling::Button* unitHitW_{ nullptr };  // 单位点击热区：只有选中容器时才可点（PX ↔ %）
    Ling::Button* unitHitH_{ nullptr };
    Ling::Button* btnSwap_{ nullptr };
    Ling::Node* collageProps_{ nullptr };
    Ling::Node* docProps_{ nullptr };

    // 拼图属性
    NumProp collageBorder_;
    NumProp collageGap_;
    NumProp collageRadius_;
    // 背景色板（拼图 / 合并文档各一组）：当前背景色对应的色块加一圈绿色
    BgSwatchRefs bgSwatchCollage_;
    BgSwatchRefs bgSwatchMerge_;
    int lastLayerClickCell_{ -1 };
    DWORD lastLayerClickTick_{ 0 };
    // 拖拽布局时的实时尺寸（预览期间尺寸行显示它）
    int liveSizeW_{ 0 }, liveSizeH_{ 0 };
    bool liveSizeValid_{ false };
    std::vector<Ling::Button*> layoutCards_;
    int layoutIndex_{ 0 };
    Ling::TextBox* rowsBox_{ nullptr };
    Ling::TextBox* colsBox_{ nullptr };
    Ling::Label* rowsLabel_{ nullptr };
    Ling::Label* colsLabel_{ nullptr };

    // 合并属性
    NumProp docBorder_;
    NumProp docQuality_;
    // 输出质量（1..100，默认 100）：合并文档导出图片 / PDF / PPTX 的编码质量
    int outputQuality_{ 100 };
    Ling::Button* formatDdl_{ nullptr };
    Ling::Label* formatValue_{ nullptr };
    // 合并文档输出去向：PDF / PPTX，或图片（单文件存一张，多文件选文件夹按页导出）
    enum class DocOut { Pdf, Pptx, Png, Webp, Jpg };
    DocOut docOut_{ DocOut::Pdf };
    std::vector<DocOut> docOutOptions() const;   // 当前可选的输出格式（JPG 无背景时不可用）
    void updateDocFormatText();
    std::unique_ptr<SettingUi::Popup> formatPopup_;

    bool syncingSize_{ false };
    // 由贴图「导入图片编辑」打开：激活期间临时置顶，才能盖住 WS_EX_TOPMOST 的贴图
    bool keepAbovePins_{ false };
    // 尺寸单位：单击「PX」在容器/布局尺寸上切到 %（相对画板），画板尺寸恒为 PX
    bool sizePercent_{ false };
    bool unitShownPercent_{ false };
    int lastGroupKey_{ -1 };   // 上一次回填到属性区的布局组信息（类型+行列）
    bool lastSplitSel_{ false };   // 上一次选中是否为已分割容器（控制行列输入框置灰）
    bool syncingProps_{ false };
    std::unique_ptr<SettingUi::HoverTip> hoverTip_;
};
