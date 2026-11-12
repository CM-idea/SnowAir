#pragma once
#include <include/Ling.h>
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <windows.h>

class ImageEditAnnotHost;

// 拼图模式：无限画布上的图片容器、分割、吸附、画板与导出。
// 画布内容全部在 D2D 上自绘，属性栏与工具栏由 WinImageEdit 负责。
class ImageEditCollage : public Ling::Canvas
{
public:
    enum class Tool { None, Split, Layout };

    explicit ImageEditCollage(Ling::WinBase* win);
    ~ImageEditCollage();

    void layout() override;

    // 工具与参数
    void setTool(Tool tool);
    void setToolById(const std::wstring& id);   // 工具栏槽位 id -> 工具
    void fitToView();                            // 缩放到刚好容纳画板
    void setRowsCols(int rows, int cols);
    int rows() const { return rows_; }
    int cols() const { return cols_; }
    // 布局类型：0 自由（行×列）/ 1 上和下 / 2 左和右；切块作用于选中容器或拖出的区域
    void setLayoutType(int type) { layoutType_ = type; }
    int layoutType() const { return layoutType_; }
    void applyLayoutToSelection();
    void createLayoutInRect(const D2D1_RECT_F& rect);
    void setBorder(float value);
    void setGap(float value);
    void setRadius(float value);
    float border() const { return border_; }
    float gap() const { return gap_; }
    float radius() const { return radius_; }
    void setBackgroundColor(uint32_t rgba);
    uint32_t backgroundColor() const { return bgColor_; }

    // 布局
    void applyLayoutPreset(int index);

    // 尺寸：w/h <= 0 表示该轴自动（由画板内容 + 外边距决定）
    void setCanvasSize(int w, int h);
    int canvasWidth() const;                     // 自动时为当前画板宽，空画布为 0
    int canvasHeight() const;
    int manualWidth() const { return manualW_; }  // >0 = 固定尺寸
    int manualHeight() const { return manualH_; }
    // 该轴是否是用户主动输入固定的（拖入内容自动建板不算；绿色只提示主动固定）
    bool manualSizeEnteredW() const { return sizeEnteredW_; }
    bool manualSizeEnteredH() const { return sizeEnteredH_; }
    std::wstring zoomText() const;

    // 选择与内容
    bool hasContent() const;
    bool hasSelection() const;
    // 分割工具可用性：多选或布局组（多格）时不可分割，只有独立容器/已分割组可以
    bool canSplitSelection() const;
    // 是否正在拖拽/缩放等高频交互（窗口层据此跳过昂贵的整画布导出）
    bool isInteracting() const;
    void clear();
    void undo();
    void redo();
    bool canUndo() const { return !undoStack_.empty(); }

    // 素材缩略图
    struct Thumb { std::wstring path; bool selected; int cell; };
    std::vector<Thumb> thumbnails() const;
    void selectCell(int index);

    // 拖放与文件
    void handleDrop(const std::vector<std::wstring>& paths, POINT clientPos);
    void pickImageForSelected();
    // 底部素材条：替换指定容器的图片（弹文件选择）
    void replaceCellImage(int index);

    // 导出（只输出画板区域）
    bool exportPixels(std::vector<BYTE>& pixels, int& width, int& height);

    // —— 标注宿主（工具属性栏 + 画布标注）——
    void setAnnotHost(ImageEditAnnotHost* host) { annot_ = host; }
    bool annotating() const;                   // 当前是否选中了标注工具
    // 客户区 → 画板像素 / 画板像素 → 客户区
    POINT clientToArtPixel(POINT clientPos) const;
    D2D1_POINT_2F artPixelToClient(float ax, float ay) const;
    D2D1_RECT_F artPixelRect() const;           // (0,0,画板宽,画板高)

    // 状态回调
    std::function<void()> onSelectionChanged;
    std::function<void()> onContentChanged;
    std::function<void()> onZoomChanged;
    // 拖拽布局的区域尺寸实时回调（w/h，live=false 表示结束/清除）
    std::function<void(int, int, bool)> onLiveSize;
    // 右键退到最后一层：让编辑器收起当前工具/属性栏
    std::function<void()> onRequestExitTool;

    // 窗口层注入：返回 true 表示该窗口坐标点落在浮动 UI（工具栏/胶囊）上，画布应忽略。
    std::function<bool(POINT)> uiHitTest;

    bool selectedCellSize(int& w, int& h) const;
    // 选中布局组时返回该组的布局类型/行列数（供右侧属性区回填）
    bool selectedLayoutGroupInfo(int& type, int& rows, int& cols) const;
    // 当前选中的是不是已分割的容器（右侧「布局」属性此时应禁用，避免再套布局破坏分割）
    bool selectionIsSplitGroup() const;
    // 选中容器时按输入值改容器尺寸（分割组不允许单块改尺寸，返回 false）
    bool setSelectedCellSize(int w, int h);
    bool isCellSelected(int index) const;
    void clearCellSelection();
    std::vector<int> selectedCellIndices() const;

private:
    struct Pt { float x{ 0.f }, y{ 0.f }; };
    struct Cell {
        std::vector<Pt> poly;      // 世界坐标（画板内为 0..画板宽）
        std::wstring image;
        float rotation{ 0.f };     // 图片旋转角（度，顺时针）；容器不旋转
        int group{ -1 };
        bool stretch{ false };
        float imgScale{ 1.f };      // 容器内图片的缩放（双击容器后滚轮调整）
        float imgOffX{ 0.f }, imgOffY{ 0.f };   // 容器内图片偏移（相对容器宽高的比例）
        // 图片绘制尺寸相对容器的比例：容器缩放时图片整体跟着缩放，位置始终钉在容器内
        float imgNW{ 0.f }, imgNH{ 0.f };
        bool flipH{ false }, flipV{ false };   // 图片水平/垂直镜像（快捷控件）
    };
    struct SplitLine { Pt a{}, b{}; };
    struct SplitGroup {
        std::vector<Pt> original;
        Cell base;                 // 原容器（含图片位置/缩放/旋转），用于切块时继承图片切片
        std::vector<SplitLine> lines;   // 分割刀数不限
    };
    // 布局组：一次布局生成的所有格子算一个组（自由 R×C / 上下两带 / 左右两带）。
    // 组只保存外框 + 类型 + 各格比例，格子的真实矩形由 rebuildLayoutGroup 统一重算；
    // 组内缝隙拖动就是改相邻两格的比例（表格式），外框尺寸不变。
    struct LayoutGroupState {
        int type{ 0 };              // 0 自由行列 / 1 上和下 / 2 左和右
        int rows{ 0 }, cols{ 0 };   // 类型 0：行数/列数；类型 1/2：第一带格数/第二带格数
        float band{ 0.5f };         // 类型 1：上带高度占比；类型 2：左带宽度占比
        std::vector<float> fr1;     // 类型 0：列宽占比；类型 1：上带列宽；类型 2：左带行高
        std::vector<float> fr2;     // 类型 0：行高占比；类型 1：下带列宽；类型 2：右带行高
        std::vector<int> members;   // 按格位顺序排列的 cells_ 下标
        D2D1_RECT_F rect{ 0, 0, 0, 0 };
    };
    // 组内可拖缝隙（表格行列分隔线）
    struct LayoutDivider {
        bool vertical{ false };     // 竖缝 = 左右拖动
        int array{ 0 };             // 0 = 改 fr1，1 = 改 fr2；-1 = 改两带 band 比例
        int index{ -1 };            // 缝隙在数组中的下标（1..n-1）
        float pos{ 0.f };           // 世界坐标（竖缝 x / 横缝 y）
        float lo{ 0.f }, hi{ 0.f }; // 沿另一轴的范围
    };
    struct Snapshot {
        std::vector<Cell> cells;
        std::map<int, SplitGroup> groups;
        std::map<int, LayoutGroupState> layouts;
        int nextGroup{ 1 };
        uint32_t bgColor{ 0xFFFFFFFF };
        float border{ 0.f }, gap{ 0.f }, radius{ 0.f };
        int rows{ 2 }, cols{ 2 };
        int manualW{ 0 }, manualH{ 0 };
        int manualL{ 0 }, manualT{ 0 };
        bool sizeEnteredW{ false }, sizeEnteredH{ false };
        int selected{ -1 };
    };
    // 容器的 8 个缩放控点
    enum class Handle { None = -1, TL = 0, T, TR, R, BR, B, BL, L };

    void paintScreen(ID2D1DeviceContext* rt);
    void paintWorld(ID2D1RenderTarget* rt, float ox, float oy, float scale, bool chrome);
    void paintSelection(ID2D1RenderTarget* rt, float ox, float oy, float scale);

    // 坐标系换算
    float viewScale() const;
    void viewOrigin(float& ox, float& oy) const;
    D2D1_RECT_F artRect() const;                 // 世界坐标画板
    // 画板原点变化时把标注换算到新原点，避免改外边距/尺寸时标注跟着画板整体滑走
    void syncAnnotOrigin();
    Pt toWorld(POINT clientPos) const;
    bool toWorldIfInside(POINT clientPos, Pt& out);

    // 命中与几何
    int cellAt(const Pt& p) const;
    Handle handleAt(POINT clientPos) const;
    bool pointInPoly(const std::vector<Pt>& poly, const Pt& p) const;
    D2D1_RECT_F boundsOf(const std::vector<Pt>& poly) const;
    static std::vector<Pt> rectPoly(float l, float t, float r, float b);
    static std::vector<Pt> clipHalf(const std::vector<Pt>& poly, const Pt& a, const Pt& b, bool keepPositive);
    static float polyArea(const std::vector<Pt>& poly);

    // 容器操作
    void translateCell(Cell& c, float dx, float dy);
    // 间距变化：相邻容器原地按增量推开（正）/收拢（负），位置和大小本身不变
    void reflowGap(float delta);
    // 固定画板只涨不缩：内容超出锁定时把锁定尺寸同步扩到实际大小（尺寸行显示真实值）
    void syncLockedBoard();
    void resizeSelected(Handle h, const Pt& world, bool keepRatio);
    float snapCoord(float value, bool horizontal, int ignoreCell, float tol) const;
    void snapCellToNeighbors(int index);
    // 自由吸附：容器/图片边缘吸附到其它容器与画板边缘
    struct SnapGuide { bool vertical{ false }; float pos{ 0.f }; };
    std::vector<float> snapTargets(bool verticalAxis, const std::vector<int>& ignoreCells) const;
    // edgeA/edgeB 是矩形两条边的坐标；返回让较近那条边贴到目标线的位移，命中时给出参考线位置
    float snapEdge(float edgeA, float edgeB, const std::vector<float>& targets,
        float tol, float& outGuidePos, bool& snapped) const;
    std::vector<SnapGuide> snapGuides_;
    void clearSnapGuides() { if (!snapGuides_.empty()) { snapGuides_.clear(); if (win) win->refresh(); } }

    // 编辑
    void pushUndo();
    void restore(const Snapshot& s);
    Snapshot snapshot() const;
    void commitSplit(int index, const Pt& a, const Pt& b);
    // 用户画的分割线段与容器边缘的交点：必须穿过两条边（两个交点都在线段内）才算有效
    bool splitSegmentForCell(int index, const Pt& a, const Pt& b, Pt& outA, Pt& outB) const;
    // 这条分割线段作用在哪个容器上：优先取线段中点所在的容器，否则取第一个被穿过的容器
    int splitTargetForSegment(const Pt& a, const Pt& b) const;
    // 分割线是持久对象：重算组内所有块（含图片切片与块间间距）
    void rebuildSplitGroup(int group);
    bool nearestSplitLine(int group, const Pt& p, float tol, int& lineIndex) const;
    void deleteSplitLine(int group, int lineIndex);
    // 分割组整体选中：选中任意一块 = 选中整组（框住原容器），选中时显示分割线与控点
    int selectionGroup() const;
    void selectCellGroup(int hit);
    // 命中分割线：endpoint = 0/1 命中端点，-1 命中线身
    bool splitLineHit(POINT clientPos, int& group, int& line, int& endpoint) const;
    void paintSplitLines(ID2D1RenderTarget* rt, float ox, float oy, float scale);
    // 当前布局类型下的切块矩形（自由 R×C / 上下两带 / 左右两带）
    std::vector<std::vector<Pt>> layoutPieceRects(const D2D1_RECT_F& rect) const;
    // —— 布局组（自由/上下/左右都算一个组）——
    int layoutGroupOf(int cellIndex) const;                 // 属于布局组返回组 id，否则 -1
    bool isLayoutGroup(int group) const;
    D2D1_RECT_F layoutGroupBounds(int group) const;         // 整组外框（世界坐标）
    // 按组外框 + 类型 + 各格比例算出所有格位矩形（含间距）
    std::vector<D2D1_RECT_F> layoutSlotRects(const LayoutGroupState& grp) const;
    void initLayoutFractions(LayoutGroupState& grp) const;
    // 把组内格子按格位重算（保留图片的绝对绘制框，拖动缝隙时像裁切窗口）
    void rebuildLayoutGroup(int group);
    // 改行/列/布局类型后重建组（数量增减、比例重置、保持外框）
    void reshapeLayoutGroup(int group);
    // 组内所有可拖缝隙
    std::vector<LayoutDivider> layoutDividers(const LayoutGroupState& grp) const;
    bool layoutDividerHit(POINT clientPos, int& group, int& dividerIndex) const;
    void paintLayoutGroupChrome(ID2D1RenderTarget* rt, const LayoutGroupState& grp,
        float ox, float oy, float scale, bool withHandles);
    // —— 选中对象上方的快捷控件：左旋 / 垂直镜像 / 水平镜像 / 右旋 ——
    bool quickActionLayout(float& bgL, float& bgT, float& bgW, float& bgH,
        D2D1_RECT_F btnRects[4], bool& placeBelow) const;
    int quickActionHit(POINT clientPos) const;      // 0..3，未命中 -1
    void applyQuickAction(int action);
    void rotateSelectedContent(int dir);            // 单容器：图片旋转 ±90（无图则容器换向）
    void mirrorSelectedContent(bool horizontal);
    void rotateSelectedContainer(int dir);          // 容器旋转 ±90（图片保持朝向）
    void mirrorSelectedContainer(bool horizontal);  // 容器镜像（图片保持朝向）
    void rotateLayoutGroup(int group, int dir);     // 布局组换向/旋转
    void mirrorLayoutGroup(int group, bool horizontal);
    // 分割组整组旋转/镜像：各块、分割线、原容器与块内图片一起变换
    // action：0=右转 90 / 1=左转 90 / 2=水平镜像 / 3=垂直镜像
    void transformSplitGroup(int group, int action);
    void paintQuickActions(ID2D1RenderTarget* rt);
    int quickHoverAction_{ -1 };
    int hoverDividerGroup_{ -1 }, hoverDividerIndex_{ -1 };
    int hoverCell_{ -1 };              // 悬停的可选容器（空工具时显示浅色取景框）
    bool annotDragging_{ false };      // 空工具下正在拖动/点选标注（事件转发给标注宿主）
    bool draggingDivider_{ false };
    bool drawingNewLayout_{ false };   // 布局工具：只有从空白处起拖才是「画新布局」
    int dragDividerGroup_{ -1 }, dragDividerIndex_{ -1 };
    LayoutGroupState dragDividerStart_{};
    Pt dragDividerStartPos_{};
    // 从原容器继承图片：每块只显示自己区域内的那一片
    Cell makeSubCell(const Cell& base, const std::vector<Pt>& piece) const;
    void pasteCells();
    void setCellImage(int index, const std::wstring& path, bool stretch = false);
    void addImageBlock(const std::wstring& path, const Pt& at, bool useNaturalSize);
    void notifyChanged(bool content = true);

    // 鼠标交互
    void onMouseDown(POINT pos, bool right);
    void onMouseMove(POINT pos);
    void onMouseUp(POINT pos, bool right);
    void onMouseWheel(POINT pos, float space);
    void onKeyDown(UINT key);
    void onKeyUp(UINT key);
    void onMiddleDown(POINT pos);
    void onMiddleUp(POINT pos);
    void onCursor(bool* handled);

    // 绘制资源
    ID2D1Bitmap* bitmapFor(ID2D1RenderTarget* rt, const std::wstring& path, int& imgW, int& imgH);
    bool imageSizeOf(const std::wstring& path, int& imgW, int& imgH) const;
    // 按容器尺寸算出图片的归一化绘制尺寸（保持原始比例铺满容器；stretch = 直接铺满）
    void normalizeCellImage(Cell& c) const;

    // 图片在容器内的实际绘制框（含旋转，用于虚线控件/旋转/缩放命中）
    struct ImgFrame {
        bool valid{ false };
        float cx{ 0.f }, cy{ 0.f }, w{ 0.f }, h{ 0.f }, angleRad{ 0.f };
        int iw{ 0 }, ih{ 0 };
        D2D1_MATRIX_3X2_F m{ D2D1::Matrix3x2F::Identity() };
    };
    ImgFrame imageFrameOf(const Cell& c) const;
    D2D1_POINT_2F screenPointOf(const Pt& world) const;   // 世界 → 画布节点内坐标
    enum class ImgHit { None, Inside, Corner, Edge, Rotate };
    ImgHit imageHitAt(POINT clientPos, Handle& handle, int& cornerIndex) const;
    void paintImageFrame(ID2D1RenderTarget* rt, float ox, float oy, float scale);
    // 控件框与图片像素按同一个图片矩形绘制，保证永远对齐。
    // clipPoly 为空 = 不裁剪（图片调整模式下显示容器外的半透明整图）。
    void paintCellImage(ID2D1RenderTarget* rt, const Cell& c, const D2D1_MATRIX_3X2_F& world,
        float opacity, const std::vector<Pt>* clipPoly);
    void drawGrid(ID2D1RenderTarget* rt);

    std::wstring pickImageFile();

private:
    // 文档状态
    std::vector<Cell> cells_;
    std::map<int, SplitGroup> splitGroups_;
    std::map<int, LayoutGroupState> layoutGroups_;   // 布局生成的组（自由/上下/左右）
    uint32_t bgColor_{ 0xFFFFFFFF };
    float border_{ 0.f }, gap_{ 0.f }, radius_{ 0.f };
    int layoutType_{ 0 };              // 0 自由 / 1 上和下 / 2 左和右
    int rows_{ 1 }, cols_{ 1 };        // 自由=行/列；上和下=上/下；左和右=左/右（默认 1×1）
    int selected_{ -1 };
    std::vector<int> extraCells_;   // 框选时选中的其余容器（selected_ 为主选中）
    int nextGroup_{ 1 };

    // 画板：固定尺寸（0 = 自动：内容包围盒 + 外边距）。
    // 固定后只随内容变大而变大，不会随容器缩小而缩回（manualL_/manualT_ 是锁定时画板原点）。
    int manualW_{ 0 }, manualH_{ 0 };
    int manualL_{ 0 }, manualT_{ 0 };
    bool sizeEnteredW_{ false }, sizeEnteredH_{ false };   // 用户主动输入过该轴尺寸（显示绿色）
    // 手工输入的尺寸原样生效；这里记录「输入当时的内容范围」，之后只有容器继续变大才把画板撑大
    float contentFloorR_{ -FLT_MAX }, contentFloorB_{ -FLT_MAX };
    void captureContentExtents();   // 记录当前内容范围（含已固定画板右/下边），供「只涨不缩」判断
    // 上一次绘制时的画板原点（用于标注坐标换算）
    D2D1_RECT_F lastArt_{};
    bool lastArtValid_{ false };

    // 视图（无限画布）
    float zoomFactor_{ 1.f };      // 1 = 100%
    float panDX_{ 0.f }, panDY_{ 0.f };

    Tool tool_{ Tool::None };
    ImageEditAnnotHost* annot_{ nullptr };

    bool exporting_{ false };

    // 交互状态
    bool dragging_{ false };
    bool panning_{ false };
    bool spaceHeld_{ false };
    bool marqueeSelecting_{ false };
    bool marqueeAdditive_{ false };
    Pt marqueeStart_, marqueeCur_;
    bool adjustingSplit_{ false };
    int splitEnd_{ -1 };   // 正在拖的分割线端点：0/1 = 端点，-1 = 整条线
    bool resizing_{ false };
    bool imageAdjust_{ false };
    bool imgDrag_{ false };
    bool imgResizing_{ false };
    bool rotatingImg_{ false };
    Handle imgHandle_{ Handle::None };
    float rotStartAngle_{ 0.f }, rotStartDeg_{ 0.f };
    float imgStartScale_{ 1.f }, imgStartNW_{ 1.f }, imgStartNH_{ 1.f };
    D2D1_POINT_2F imgStartLocal_{ 0.f, 0.f };
    Handle resizeHandle_{ Handle::None };
    int dragGroup_{ -1 };
    int dragLineIndex_{ -1 };
    Pt dragStart_, dragCur_;
    // 拖动容器：按下时的原始多边形 + 未吸附的累计位移（吸附按累计位移算，避免贴住参考线后拖不动）
    std::vector<std::vector<Pt>> dragOrigPolys_;
    // 拖动分割组：按下时的原始分割线与原容器（分组 id → 几何），拖动时按累计位移重算
    std::map<int, std::vector<SplitLine>> dragOrigSplitLines_;
    std::map<int, std::vector<Pt>> dragOrigSplitOriginal_;
    Pt dragAccum_{};
    // 图片调整拖动：按累计位移算，避免图片正好贴容器边时被吸附钉死
    float imgOffStartX_{ 0.f }, imgOffStartY_{ 0.f };
    Pt resizePivot_{};
    bool dragShift_{ false };
    POINT panStart_{};
    float panStartDX_{ 0.f }, panStartDY_{ 0.f };

    // 双击检测
    DWORD lastClickTick_{ 0 };
    POINT lastClickPos_{};
    int lastClickCell_{ -1 };

    std::vector<Snapshot> undoStack_, redoStack_;
    std::vector<Cell> cellClipboard_;   // Ctrl+C / Ctrl+V 的容器剪贴板
    int pasteCount_{ 0 };               // 连续粘贴的阶梯偏移

    // WIC / D2D 图片缓存（位图属于创建它的 RenderTarget，换目标时必须整体重建）
    std::map<std::wstring, ID2D1Bitmap*> bitmapCache_;
    std::map<std::wstring, std::pair<int, int>> bitmapSize_;
    // 图片原始像素尺寸缓存：拖动时每帧都会问尺寸，不能每帧重开 WIC 解码器（很卡）
    mutable std::map<std::wstring, std::pair<int, int>> imageSizeCache_;
    ID2D1RenderTarget* bitmapRT_{ nullptr };

    // 事件令牌
    winrt::event_token tokDown_, tokMove_, tokUp_, tokWheel_, tokKey_, tokKeyUp_, tokMidDown_, tokMidUp_, tokCursor_;
};
