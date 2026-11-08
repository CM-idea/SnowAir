#pragma once
#include <include/Ling.h>
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <windows.h>

// 合并文档模式：图片卡片列表、排序、文档属性与逐页导出。
class ImageEditMerge : public Ling::Canvas
{
public:
    struct Item {
        std::wstring path;
        int w{ 0 }, h{ 0 };
    };

    explicit ImageEditMerge(Ling::WinBase* win);
    ~ImageEditMerge();

    void layout() override;

    bool addImagesDialog();
    void addPaths(const std::vector<std::wstring>& paths);
    void clear();
    bool hasContent() const { return !items_.empty(); }
    int itemCount() const { return (int)items_.size(); }
    const Item& item(int index) const { return items_[index]; }
    // 删除当前选中的卡片（框选/单击选中的一组），返回是否真的删了
    bool deleteSelectedCards();
    bool hasSelection() const { return !selected_.empty(); }

    void setBorder(float value);
    void setBackgroundColor(uint32_t rgba);
    void setPageSize(int w, int h);
    int pageWidth() const { return pageW_; }
    int pageHeight() const { return pageH_; }
    void setFormat(int format);
    int format() const { return format_; }
    float border() const { return border_; }
    uint32_t backgroundColor() const { return bgColor_; }

    void handleDrop(const std::vector<std::wstring>& paths, POINT clientPos);

    // 把第 index 页渲染成 BGRA；尺寸 0 时按原图。
    bool renderPage(int index, std::vector<BYTE>& pixels, int& width, int& height);

    std::function<void()> onChanged;

private:
    void paintScene(ID2D1RenderTarget* rt);
    D2D1_RECT_F cardRect(int index) const;
    // 当前窗口宽度能放下几列卡片（随画布宽度自适应，最少 1 列）
    int columns() const;
    // 预览卡片高度：设了页宽高就按页面比例（宽固定 130），否则用默认 160
    float cardPreviewHeight() const;
    bool isSelected(int index) const;
    int indexAt(POINT clientPos);
    void onMouseDown(POINT pos, bool right);
    void onMouseMove(POINT pos);
    void onMouseUp(POINT pos, bool right);
    void onMouseWheel(POINT pos, float space);
    ID2D1Bitmap* bitmapFor(ID2D1RenderTarget* rt, const std::wstring& path, int& imgW, int& imgH);
    void notifyChanged();

private:
    std::vector<Item> items_;
    float border_{ 0.f };
    uint32_t bgColor_{ 0xFFFFFFFF };
    int pageW_{ 0 }, pageH_{ 0 };
    int format_{ 0 };   // 0 = PDF, 1 = PPTX

    float scrollY_{ 0.f };
    bool dragging_{ false };
    bool dragMoved_{ false };            // 超过阈值才算真正拖拽（单击只选中）
    int dragFrom_{-1}, dragTo_{-1};
    float dragX_{ 0.f }, dragY_{ 0.f };  // 拖拽预览的位置（画布内坐标）
    float dragStartX_{ 0.f }, dragStartY_{ 0.f };
    std::vector<int> selected_;          // 选中的卡片（单击 / 框选，可多选）
    // 框选（在空白处按下拖动）
    bool marquee_{ false };
    float marqueeX0_{ 0.f }, marqueeY0_{ 0.f }, marqueeX1_{ 0.f }, marqueeY1_{ 0.f };

    std::map<std::wstring, ID2D1Bitmap*> bitmapCache_;
    std::map<std::wstring, std::pair<int, int>> bitmapSize_;

    winrt::event_token tokDown_, tokMove_, tokUp_, tokWheel_;
};
