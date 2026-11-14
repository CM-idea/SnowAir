#pragma once
#include "AnnotHost.h"
#include <windows.h>

class WinImageEdit;

// 图片编辑器的标注宿主：本体是一个不可见的窗口（只承载定时器、工具属性栏归属），
// 鼠标/键盘/输入法由 ImageEditCollage 转发，标注直接画到画布的 D2D 上下文，
// 导出时再按画板像素合成。标注坐标 = 画板内像素坐标。
class ImageEditAnnotHost : public AnnotHost
{
public:
    explicit ImageEditAnnotHost(WinImageEdit* editor);
    ~ImageEditAnnotHost();

    // 工具栏选中某个标注工具（id 为工具栏槽位 id；空 = 收起工具）
    void activateTool(const std::wstring& id);
    void deactivateTool();
    // 编辑器窗口切到别的应用时收起属性栏（保留工具状态），切回来再摆出来
    void suspendToolBar();
    void resumeToolBar();

    bool annotating() const { return !getCurToolId().empty(); }
    bool hasAnyShapes() const;
    bool canUndoAnnot() const;
    void undoAnnot();
    void clearAllShapesNow();

    // 输入转发：pos 是图片编辑窗口的客户区坐标
    bool forwardDown(POINT pos, bool right);
    // 空工具（未激活任何标注工具）时也能点选/悬停标注
    bool hitAnnotAtClient(POINT clientPos);
    // 删除键统一入口：返回 true 表示标注宿主已经消费（文字编辑中，或删除了选中标注）
    // Delete / Backspace 都算删除键，key 原样透传给标注宿主
    bool forwardDeleteKey(UINT key = VK_DELETE);
    void forwardMove(POINT pos);
    void forwardUp(POINT pos, bool right);
    void forwardWheel(int dir);
    void forwardChar(UINT ch);
    void forwardIme();
    void forwardKey(UINT key) override;

    // 底图（画板像素）：马赛克/包浆等滤镜取样用
    void setContentBitmap(std::vector<BYTE>& pixels, int w, int h);
    void clearContentBitmap();
    // 画板原点移动：把已有标注整体平移到新的原点下（视口滤镜重新铺满）
    void shiftCoordinates(float dx, float dy);

    // 画到画布上：调用前已把「画板像素 → 屏幕」的变换设好
    void paintOnScreen(ID2D1DeviceContext* ctx);

    // —— AnnotHost ——
    HWND annotInputHwnd() const override;
    // 右键逐层退出：选中标注 → 取消选中；正在画的草稿 → 丢弃；样式开关 → 关闭；
    // 最后一层才收起当前工具与属性栏（与截图标注一致）
    bool annotRightClick() override;
    POINT toImgPos(const POINT& pos) const override;
    D2D1_POINT_2F annotToClientPt(float ax, float ay) const override;
    D2D1_POINT_2F clientToAnnotPt(float cx, float cy) const override;
    D2D1_RECT_F annotContentRect() const override;
    bool queryMainBarAnchor(MainBarAnchor& out) const override;
    void layoutTools() override;
    void onToolChanged() override;

private:
    void handleTimer(UINT id);

    WinImageEdit* editor_{ nullptr };
    winrt::event_token tokTimer_{};
};
