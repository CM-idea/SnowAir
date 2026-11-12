#include "pch.h"
#include "ImageEditCollage.h"
#include "ImageEditAnnot.h"
#include "ImageEditTheme.h"
#include "../Lang.h"
#include "../Shape/AnnotCursor.h"
#include "../Tool/IconCodes.h"
#include <wincodec.h>
#include <algorithm>
#include <cmath>

using namespace Ling;

namespace {
    constexpr float kZoomMin{ 0.05f };
    constexpr float kZoomMax{ 10.f };
    constexpr float kSnapTol{ 3.f };              // 吸附阈值（屏幕像素，轻微吸附）
    constexpr float kHandleSize{ 6.f };           // 选中控点（屏幕像素）

    Microsoft::WRL::ComPtr<IWICImagingFactory> wicFactory()
    {
        static Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
        if (!factory) {
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&factory));
        }
        return factory;
    }

    bool hasImageExt(const std::wstring& path)
    {
        auto dot = path.find_last_of(L'.');
        if (dot == std::wstring::npos) return false;
        std::wstring ext = path.substr(dot);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
        return ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".webp"
            || ext == L".bmp" || ext == L".gif" || ext == L".tif" || ext == L".tiff";
    }

    const std::vector<COMDLG_FILTERSPEC>& imageFilters()
    {
        static const std::vector<COMDLG_FILTERSPEC> filters{
            { L"图片文件", L"*.png;*.jpg;*.jpeg;*.webp;*.bmp;*.gif;*.tif;*.tiff" },
            { L"所有文件", L"*.*" },
        };
        return filters;
    }

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> solidBrush(ID2D1RenderTarget* rt, uint32_t rgba)
    {
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
        if (rt && SUCCEEDED(rt->CreateSolidColorBrush(Color(rgba).getD2DColor(), &brush))) return brush;
        return nullptr;
    }

    ID2D1Factory* d2dFactory()
    {
        auto* d2d = D2D::get();
        return d2d ? d2d->d2dFactory.Get() : nullptr;
    }

    // 与标注选中框一致的虚线（4/3 自定义段长）
    ID2D1StrokeStyle* dashStyle()
    {
        static Microsoft::WRL::ComPtr<ID2D1StrokeStyle> style;
        static ID2D1Factory* styleFactory = nullptr;
        auto* factory = d2dFactory();
        if (!factory) return nullptr;
        if (!style || styleFactory != factory) {
            style.Reset();
            styleFactory = factory;
            float dashes[]{ 4.f, 3.f };
            D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(
                D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                D2D1_LINE_JOIN_MITER, 10.f, D2D1_DASH_STYLE_CUSTOM, 0.f);
            if (FAILED(factory->CreateStrokeStyle(&sp, dashes, 2, style.GetAddressOf())) )
                style.Reset();
        }
        return style.Get();
    }

    // 控点：和标注一样的圆点（阴影 + 白心 + 主题色描边）。白心固定白色，不跟主题
    void paintHandleDot(ID2D1RenderTarget* rt, float cx, float cy, float dpi)
    {
        const float ro = 5.f * dpi;
        const float ri = 4.f * dpi;
        auto shadow = solidBrush(rt, 0x00000040u);
        auto core = solidBrush(rt, 0xFFFFFFFFu);
        auto ring = solidBrush(rt, ImageEditTheme::accent);
        const D2D1_ELLIPSE outer = D2D1::Ellipse(D2D1::Point2F(cx, cy), ro, ro);
        const D2D1_ELLIPSE inner = D2D1::Ellipse(D2D1::Point2F(cx, cy), ri, ri);
        if (shadow) rt->FillEllipse(outer, shadow.Get());
        if (core) rt->FillEllipse(inner, core.Get());
        if (ring) rt->DrawEllipse(inner, ring.Get(), 1.5f * dpi);
    }

    // 空容器提示：中间一个「+」，下面一句「拖拽或选择图片」（屏幕空间绘制，字号不随缩放变化）
    void paintEmptyCellHint(ID2D1RenderTarget* rt, const D2D1_RECT_F& scr, float dpi)
    {
        if (!rt) return;
        const float d = dpi;
        if (scr.right - scr.left < 90.f * d || scr.bottom - scr.top < 70.f * d) return;
        const float cx = (scr.left + scr.right) * 0.5f;
        const float cy = (scr.top + scr.bottom) * 0.5f;
        if (auto plus = solidBrush(rt, ImageEditTheme::hintText)) {
            const float half = 6.f * d;          // 加号缩小（原 9）
            const float th = 1.2f * d;
            const float py = cy - 15.f * d;      // 加号整体上移，给文字留出更大的间距
            rt->DrawLine(D2D1::Point2F(cx - half, py), D2D1::Point2F(cx + half, py), plus.Get(), th);
            rt->DrawLine(D2D1::Point2F(cx, py - half), D2D1::Point2F(cx, py + half), plus.Get(), th);
        }
        if (auto textBrush = solidBrush(rt, ImageEditTheme::hintTextSoft)) {
            auto* d2d = D2D::get();
            if (d2d) {
                // 文本布局按 DPI 缓存：拖动时空格子会每帧重画，不能每帧重建布局
                static Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
                static D2D* layoutOwner = nullptr;
                static float layoutDpi = 0.f;
                if (!layout || layoutOwner != d2d || layoutDpi != d) {
                    layout = d2d->makeTextLayout(Lang::get(L"imageEdit.emptyCellHint"),
                        12.f * d, FLT_MAX, FLT_MAX);
                    layoutOwner = d2d;
                    layoutDpi = d;
                }
                if (layout) {
                    DWRITE_TEXT_METRICS tm{};
                    layout->GetMetrics(&tm);
                    rt->DrawTextLayout(
                        D2D1::Point2F(cx - tm.width * 0.5f, cy + 8.f * d - tm.height * 0.5f),
                        layout.Get(), textBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
                }
            }
        }
    }

    template<typename TPoint>
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> buildPolyGeometry(ID2D1Factory* factory, const std::vector<TPoint>& poly)
    {
        Microsoft::WRL::ComPtr<ID2D1PathGeometry> geo;
        if (!factory || poly.size() < 3) return geo;
        if (FAILED(factory->CreatePathGeometry(&geo))) return nullptr;
        Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(geo->Open(&sink))) return nullptr;
        sink->BeginFigure(D2D1::Point2F(poly[0].x, poly[0].y), D2D1_FIGURE_BEGIN_FILLED);
        for (size_t i = 1; i < poly.size(); ++i)
            sink->AddLine(D2D1::Point2F(poly[i].x, poly[i].y));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        return geo;
    }

    // 任意多边形圆角：每个角沿两条邻边各缩进 d，再用二次贝塞尔把两个切点连起来。
    // 矩形、分割后的三角/梯形块都适用；d 限制在相邻边长的一半以内，短边不会被吃掉。
    template<typename TPoint>
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> buildRoundedPolyGeometry(
        ID2D1Factory* factory, const std::vector<TPoint>& poly, float radius)
    {
        if (!factory || poly.size() < 3 || radius <= 0.f) return {};
        const int n = (int)poly.size();
        auto dirTo = [&](int from, int to, float& ux, float& uy, float& len) {
            ux = (float)(poly[to].x - poly[from].x);
            uy = (float)(poly[to].y - poly[from].y);
            len = std::sqrt(ux * ux + uy * uy);
            if (len > 1e-4f) { ux /= len; uy /= len; }
            };
        // 每个角的两个切点：p1 = 从上一顶点进入、p2 = 离开到下一顶点
        std::vector<D2D1_POINT_2F> p1(n), p2(n);
        for (int i = 0; i < n; ++i) {
            const int prev = (i + n - 1) % n, next = (i + 1) % n;
            float ux1, uy1, len1, ux2, uy2, len2;
            dirTo(i, prev, ux1, uy1, len1);
            dirTo(i, next, ux2, uy2, len2);
            const float d = std::min(radius, std::min(len1, len2) * 0.5f);
            p1[i] = D2D1::Point2F((float)poly[i].x + ux1 * d, (float)poly[i].y + uy1 * d);
            p2[i] = D2D1::Point2F((float)poly[i].x + ux2 * d, (float)poly[i].y + uy2 * d);
        }
        Microsoft::WRL::ComPtr<ID2D1PathGeometry> geo;
        if (FAILED(factory->CreatePathGeometry(&geo))) return nullptr;
        Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(geo->Open(&sink))) return nullptr;
        sink->BeginFigure(p2[0], D2D1_FIGURE_BEGIN_FILLED);
        for (int k = 1; k <= n; ++k) {
            const int i = k % n;
            sink->AddLine(p1[i]);
            sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(
                D2D1::Point2F((float)poly[i].x, (float)poly[i].y), p2[i]));
        }
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        return geo;
    }
}

ImageEditCollage::ImageEditCollage(WinBase* win) :Canvas(win)
{
    // 画布高频重绘（悬停/拖拽时每次鼠标移动都整帧重画）：单缓冲 DrawingSurface
    // 会让合成器采样到「清屏 → 重画」的中间态，表现为缩放比例等末绘内容闪烁抖动
    enableSwapChain();
    setWidthPercent(100.f);
    setHeightPercent(100.f);
    setFlexGrow(1.f);

    tokDown_ = win->onMouseDown.add([this](POINT pos, bool right) { onMouseDown(pos, right); });
    tokMove_ = win->onMouseMove.add([this](POINT pos) { onMouseMove(pos); });
    tokUp_ = win->onMouseUp.add([this](POINT pos, bool right) { onMouseUp(pos, right); });
    tokWheel_ = win->onMouseWheel.add([this](POINT pos, float space) { onMouseWheel(pos, space); });
    tokKey_ = win->onKeyDown.add([this](UINT key) { onKeyDown(key); });
    tokKeyUp_ = win->onKeyUp.add([this](UINT key) { onKeyUp(key); });
    tokMidDown_ = win->onMouseMiddleDown.add([this](POINT pos) { onMiddleDown(pos); });
    tokMidUp_ = win->onMouseMiddleUp.add([this](POINT pos) { onMiddleUp(pos); });
    tokCursor_ = win->onCursor.add([this](bool* handled) { onCursor(handled); });
}

ImageEditCollage::~ImageEditCollage()
{
    if (win) {
        win->onMouseDown.remove(tokDown_);
        win->onMouseMove.remove(tokMove_);
        win->onMouseUp.remove(tokUp_);
        win->onMouseWheel.remove(tokWheel_);
        win->onKeyDown.remove(tokKey_);
        win->onKeyUp.remove(tokKeyUp_);
        win->onMouseMiddleDown.remove(tokMidDown_);
        win->onMouseMiddleUp.remove(tokMidUp_);
        win->onCursor.remove(tokCursor_);
    }
    for (auto& [key, bmp] : bitmapCache_)
        if (bmp) bmp->Release();
    bitmapCache_.clear();
}

void ImageEditCollage::layout()
{
    Canvas::layout();
    if (w <= 0.f || h <= 0.f) return;
    syncAnnotOrigin();
    auto ctx = startPaint();
    if (!ctx) return;
    paintScreen(ctx);
    finishPaint();
}

// ---------------- 画板与视图 ----------------

D2D1_RECT_F ImageEditCollage::artRect() const
{
    D2D1_RECT_F bbox{ FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX };
    bool has{ false };
    if (!cells_.empty()) {
        has = true;
        for (const auto& c : cells_) {
            auto b = boundsOf(c.poly);
            bbox.left = std::min(bbox.left, b.left);
            bbox.top = std::min(bbox.top, b.top);
            bbox.right = std::max(bbox.right, b.right);
            bbox.bottom = std::max(bbox.bottom, b.bottom);
        }
        if (bbox.right <= bbox.left || bbox.bottom <= bbox.top) has = false;
    }

    // 每个轴独立：手动尺寸固定原点 0，自动轴由内容包围盒 + 外边距决定
    const float b = border_;
    float left = 0.f, top = 0.f, right = 0.f, bottom = 0.f;
    if (manualW_ > 0) {
        // 固定尺寸：输入值原样生效，原点保持锁定时的位置（增长由 syncLockedBoard 处理）
        left = (float)manualL_;
        right = (float)manualL_ + (float)manualW_;
    }
    else if (has) {
        left = bbox.left - b;
        right = bbox.right + b;
    }
    if (manualH_ > 0) {
        top = (float)manualT_;
        bottom = (float)manualT_ + (float)manualH_;
    }
    else if (has) {
        top = bbox.top - b;
        bottom = bbox.bottom + b;
    }
    return D2D1::RectF(left, top, right, bottom);
}

float ImageEditCollage::viewScale() const
{
    const float d = (win && win->dpi > 0.f) ? win->dpi : 1.f;
    return zoomFactor_ * d;
}

void ImageEditCollage::viewOrigin(float& ox, float& oy) const
{
    const float s = viewScale();
    auto art = artRect();
    // 世界坐标 0 固定映射到「窗口中心 + pan」：画板尺寸变化（内容增长/固定尺寸）
    // 不再牵动屏幕映射，避免内容区随画板涨缩出现亚像素抖动。
    ox = w * 0.5f + panDX_ + art.left * s;
    oy = h * 0.5f + panDY_ + art.top * s;
}

ImageEditCollage::Pt ImageEditCollage::toWorld(POINT clientPos) const
{
    const float s = viewScale();
    float ox = 0.f, oy = 0.f;
    viewOrigin(ox, oy);
    auto art = artRect();
    const float lx = (float)clientPos.x - x;
    const float ly = (float)clientPos.y - y;
    return { (lx - ox) / s + art.left, (ly - oy) / s + art.top };
}

bool ImageEditCollage::toWorldIfInside(POINT clientPos, Pt& out)
{
    if (!isPosIn(clientPos)) return false;
    out = toWorld(clientPos);
    return true;
}

bool ImageEditCollage::annotating() const
{
    return annot_ && !annot_->getCurToolId().empty();
}

POINT ImageEditCollage::clientToArtPixel(POINT clientPos) const
{
    Pt wp = toWorld(clientPos);
    const auto art = artRect();
    return POINT{ (LONG)std::lround(wp.x - art.left), (LONG)std::lround(wp.y - art.top) };
}

D2D1_POINT_2F ImageEditCollage::artPixelToClient(float ax, float ay) const
{
    const float s = viewScale();
    float ox = 0.f, oy = 0.f;
    viewOrigin(ox, oy);
    return D2D1::Point2F(x + ox + ax * s, y + oy + ay * s);
}

D2D1_RECT_F ImageEditCollage::artPixelRect() const
{
    const auto art = artRect();
    return D2D1::RectF(0.f, 0.f, art.right - art.left, art.bottom - art.top);
}

D2D1_POINT_2F ImageEditCollage::screenPointOf(const Pt& world) const
{
    const float s = viewScale();
    float ox = 0.f, oy = 0.f;
    viewOrigin(ox, oy);
    auto art = artRect();
    return D2D1::Point2F(ox + (world.x - art.left) * s, oy + (world.y - art.top) * s);
}

ImageEditCollage::ImgFrame ImageEditCollage::imageFrameOf(const Cell& c) const
{
    ImgFrame f;
    if (c.image.empty()) return f;
    int iw = 0, ih = 0;
    if (!imageSizeOf(c.image, iw, ih)) return f;
    auto b = boundsOf(c.poly);
    const float tw = std::max(1.f, b.right - b.left);
    const float th = std::max(1.f, b.bottom - b.top);
    const float nw = c.stretch ? 1.f : ((c.imgNW > 0.f) ? c.imgNW : 1.f);
    const float nh = c.stretch ? 1.f : ((c.imgNH > 0.f) ? c.imgNH : 1.f);
    f.cx = (b.left + b.right) * 0.5f + c.imgOffX * tw;
    f.cy = (b.top + b.bottom) * 0.5f + c.imgOffY * th;
    f.w = tw * nw * c.imgScale;
    f.h = th * nh * c.imgScale;
    f.angleRad = c.rotation * 3.14159265f / 180.f;
    f.iw = iw;
    f.ih = ih;
    if (f.w <= 1.f || f.h <= 1.f) return f;
    f.m = D2D1::Matrix3x2F::Translation(f.cx, f.cy)
        * D2D1::Matrix3x2F::Rotation(c.rotation)
        * D2D1::Matrix3x2F::Scale(f.w / (float)iw, f.h / (float)ih)
        * D2D1::Matrix3x2F::Translation(-iw * 0.5f, -ih * 0.5f);
    f.valid = true;
    return f;
}

ImageEditCollage::ImgHit ImageEditCollage::imageHitAt(POINT clientPos, Handle& handle, int& cornerIndex) const
{
    handle = Handle::None;
    cornerIndex = -1;
    if (!hasSelection()) return ImgHit::None;
    const auto& cell = cells_[selected_];
    auto f = imageFrameOf(cell);
    if (!f.valid) return ImgHit::None;

    const float ca = std::cos(f.angleRad), sa = std::sin(f.angleRad);
    const float hw = f.w * 0.5f, hh = f.h * 0.5f;
    auto screenOfLocal = [&](float lx, float ly) {
        float wx = f.cx + lx * ca - ly * sa;
        float wy = f.cy + lx * sa + ly * ca;
        return screenPointOf({ wx, wy });
    };
    D2D1_POINT_2F corners[4]{
        screenOfLocal(-hw, -hh), screenOfLocal(hw, -hh),
        screenOfLocal(hw, hh), screenOfLocal(-hw, hh)
    };
    // 鼠标 → 图片局部坐标（以图片中心为原点、-角度旋转）
    const Pt wp = toWorld(clientPos);
    const float dx = wp.x - f.cx, dy = wp.y - f.cy;
    const float lx = dx * ca + dy * sa;
    const float ly = -dx * sa + dy * ca;

    const float lxp = (float)clientPos.x - x;
    const float lyp = (float)clientPos.y - y;
    const float d = (win && win->dpi > 0.f) ? win->dpi : 1.f;
    const float hs = kHandleSize * d;
    const float hitR = hs * 0.5f + 3.f;
    const float rotR = 18.f * d;

    // 四个角控点
    static const Handle kCorners[4]{ Handle::TL, Handle::TR, Handle::BR, Handle::BL };
    for (int i = 0; i < 4; ++i) {
        const float ddx = lxp - corners[i].x, ddy = lyp - corners[i].y;
        if (std::fabs(ddx) <= hitR && std::fabs(ddy) <= hitR) {
            handle = kCorners[i];
            cornerIndex = i;
            return ImgHit::Corner;
        }
    }
    const bool inside = std::fabs(lx) <= hw && std::fabs(ly) <= hh;
    // 角的外围：出现旋转光标
    if (!inside) {
        float best = FLT_MAX;
        int bestIdx = -1;
        for (int i = 0; i < 4; ++i) {
            const float ddx = lxp - corners[i].x, ddy = lyp - corners[i].y;
            const float dist = std::sqrt(ddx * ddx + ddy * ddy);
            if (dist < best) { best = dist; bestIdx = i; }
        }
        if (bestIdx >= 0 && best <= rotR) {
            cornerIndex = bestIdx;
            return ImgHit::Rotate;
        }
    }
    if (!inside) return ImgHit::None;

    // 四条边：允许直接拖边缩放图片
    const float bandWorld = hitR / std::max(0.01f, viewScale());
    if (std::fabs(lx + hw) <= bandWorld) { handle = Handle::L; return ImgHit::Edge; }
    if (std::fabs(lx - hw) <= bandWorld) { handle = Handle::R; return ImgHit::Edge; }
    if (std::fabs(ly + hh) <= bandWorld) { handle = Handle::T; return ImgHit::Edge; }
    if (std::fabs(ly - hh) <= bandWorld) { handle = Handle::B; return ImgHit::Edge; }
    return ImgHit::Inside;
}

void ImageEditCollage::syncAnnotOrigin()
{
    const auto art = artRect();
    if (!lastArtValid_) {
        lastArt_ = art;
        lastArtValid_ = true;
        return;
    }
    const float dx = lastArt_.left - art.left;
    const float dy = lastArt_.top - art.top;
    lastArt_ = art;
    if (std::fabs(dx) < 0.01f && std::fabs(dy) < 0.01f) return;
    // 标注存储的是「画板内像素」：画板原点移动时反向补偿，标注才钉在画面内容上
    if (annot_) annot_->shiftCoordinates(dx, dy);
}

int ImageEditCollage::canvasWidth() const
{
    auto art = artRect();
    const float aw = art.right - art.left;
    return aw > 0.f ? (int)std::lround(aw) : 0;
}

int ImageEditCollage::canvasHeight() const
{
    auto art = artRect();
    const float ah = art.bottom - art.top;
    return ah > 0.f ? (int)std::lround(ah) : 0;
}

void ImageEditCollage::fitToView()
{
    auto art = artRect();
    if (art.right - art.left <= 0.f || art.bottom - art.top <= 0.f) {
        zoomFactor_ = 1.f;
        panDX_ = panDY_ = 0.f;
        if (win) win->refresh();
        return;
    }
    const float d = (win && win->dpi > 0.f) ? win->dpi : 1.f;
    const float pad = 40.f * d;
    const float fit = std::min((w - pad * 2.f) / (art.right - art.left),
        (h - pad * 2.f) / (art.bottom - art.top));
    zoomFactor_ = std::max(kZoomMin, std::min(kZoomMax, fit / d));
    // 以「画板居中」为初始视图：屏幕 x(art.left) 落在窗口内居中位置
    const float s = viewScale();
    panDX_ = -(art.right - art.left) * s * 0.5f - art.left * s;
    panDY_ = -(art.bottom - art.top) * s * 0.5f - art.top * s;
    if (onZoomChanged) onZoomChanged();
    if (win) win->refresh();
}

// ---------------- 几何工具 ----------------

std::vector<ImageEditCollage::Pt> ImageEditCollage::rectPoly(float l, float t, float r, float b)
{
    return { {l, t}, {r, t}, {r, b}, {l, b} };
}

bool ImageEditCollage::pointInPoly(const std::vector<Pt>& poly, const Pt& p) const
{
    bool inside{ false };
    size_t n = poly.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const Pt& a = poly[i];
        const Pt& b = poly[j];
        if (((a.y > p.y) != (b.y > p.y)) &&
            (p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y + 1e-6f) + a.x))
            inside = !inside;
    }
    return inside;
}

int ImageEditCollage::cellAt(const Pt& p) const
{
    for (int i = (int)cells_.size() - 1; i >= 0; --i) {
        if (pointInPoly(cells_[i].poly, p)) return i;
    }
    return -1;
}

D2D1_RECT_F ImageEditCollage::boundsOf(const std::vector<Pt>& poly) const
{
    D2D1_RECT_F rc{ FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (const auto& p : poly) {
        rc.left = std::min(rc.left, p.x);
        rc.top = std::min(rc.top, p.y);
        rc.right = std::max(rc.right, p.x);
        rc.bottom = std::max(rc.bottom, p.y);
    }
    return rc;
}

std::vector<ImageEditCollage::Pt> ImageEditCollage::clipHalf(const std::vector<Pt>& poly, const Pt& a, const Pt& b, bool keepPositive)
{
    std::vector<Pt> out;
    if (poly.empty()) return out;
    auto side = [&](const Pt& p) {
        float d = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
        return keepPositive ? d : -d;
    };
    size_t n = poly.size();
    for (size_t i = 0; i < n; ++i) {
        const Pt& cur = poly[i];
        const Pt& prev = poly[(i + n - 1) % n];
        float dc = side(cur);
        float dp = side(prev);
        bool inCur = dc >= 0.f;
        bool inPrev = dp >= 0.f;
        if (inCur && inPrev) {
            out.push_back(cur);
        } else if (inCur && !inPrev) {
            float t = dp / (dp - dc);
            out.push_back({ prev.x + (cur.x - prev.x) * t, prev.y + (cur.y - prev.y) * t });
            out.push_back(cur);
        } else if (!inCur && inPrev) {
            float t = dp / (dp - dc);
            out.push_back({ prev.x + (cur.x - prev.x) * t, prev.y + (cur.y - prev.y) * t });
        }
    }
    return out;
}

float ImageEditCollage::polyArea(const std::vector<Pt>& poly)
{
    float sum{ 0.f };
    size_t n = poly.size();
    for (size_t i = 0; i < n; ++i) {
        const Pt& a = poly[i];
        const Pt& b = poly[(i + 1) % n];
        sum += a.x * b.y - b.x * a.y;
    }
    return sum * 0.5f;
}

void ImageEditCollage::translateCell(Cell& c, float dx, float dy)
{
    for (auto& p : c.poly) { p.x += dx; p.y += dy; }
}

// 把 value 吸附到最近的候选（其他容器边 ± 间距 / 画板内容边），返回吸附后的值
float ImageEditCollage::snapCoord(float value, bool horizontal, int ignoreCell, float tol) const
{
    float best = value;
    float bestDist = tol;
    auto consider = [&](float target) {
        const float d = std::fabs(target - value);
        if (d < bestDist) { bestDist = d; best = target; }
    };
    for (int i = 0; i < (int)cells_.size(); ++i) {
        if (i == ignoreCell) continue;
        auto b = boundsOf(cells_[i].poly);
        if (horizontal) {
            consider(b.left - gap_);
            consider(b.right + gap_);
            consider(b.left);
            consider(b.right);
        } else {
            consider(b.top - gap_);
            consider(b.bottom + gap_);
            consider(b.top);
            consider(b.bottom);
        }
    }
    // 画板内容区（外边距以内）的四边
    auto art = artRect();
    const float inset = border_;
    if (horizontal) {
        consider(art.left + inset);
        consider(art.right - inset);
    } else {
        consider(art.top + inset);
        consider(art.bottom - inset);
    }
    return best;
}

// 拖入新图片后：靠近已有容器/画板边就吸过去，保证接缝按间距无缝对齐
void ImageEditCollage::snapCellToNeighbors(int index)
{
    if (index < 0 || index >= (int)cells_.size()) return;
    auto b = boundsOf(cells_[index].poly);
    const float tol = 16.f / std::max(0.01f, viewScale());

    const float nl = snapCoord(b.left, true, index, tol);
    const float nr = snapCoord(b.right, true, index, tol);
    float dx = 0.f;
    if (std::fabs(nl - b.left) <= std::fabs(nr - b.right)) {
        if (std::fabs(nl - b.left) <= tol) dx = nl - b.left;
    } else if (std::fabs(nr - b.right) <= tol) {
        dx = nr - b.right;
    }

    const float nt = snapCoord(b.top, false, index, tol);
    const float nb = snapCoord(b.bottom, false, index, tol);
    float dy = 0.f;
    if (std::fabs(nt - b.top) <= std::fabs(nb - b.bottom)) {
        if (std::fabs(nt - b.top) <= tol) dy = nt - b.top;
    } else if (std::fabs(nb - b.bottom) <= tol) {
        dy = nb - b.bottom;
    }

    if (std::fabs(dx) > 0.01f || std::fabs(dy) > 0.01f)
        translateCell(cells_[index], dx, dy);
}

std::vector<float> ImageEditCollage::snapTargets(bool verticalAxis,
    const std::vector<int>& ignoreCells) const
{
    std::vector<float> targets;
    const auto art = artRect();
    const float inset = border_;
    // 画板内容区（外边距以内）的四边
    if (verticalAxis) { targets.push_back(art.left + inset); targets.push_back(art.right - inset); }
    else { targets.push_back(art.top + inset); targets.push_back(art.bottom - inset); }
    for (int i = 0; i < (int)cells_.size(); ++i) {
        if (std::find(ignoreCells.begin(), ignoreCells.end(), i) != ignoreCells.end()) continue;
        auto b = boundsOf(cells_[i].poly);
        // 有间距时要留出缝：吸附目标线在其他容器边缘外侧 gap 处
        if (verticalAxis) {
            targets.push_back(b.left - gap_);
            targets.push_back(b.right + gap_);
        }
        else {
            targets.push_back(b.top - gap_);
            targets.push_back(b.bottom + gap_);
        }
    }
    return targets;
}

float ImageEditCollage::snapEdge(float edgeA, float edgeB, const std::vector<float>& targets,
    float tol, float& outGuidePos, bool& snapped) const
{
    float best = tol;
    float delta = 0.f;
    outGuidePos = 0.f;
    snapped = false;
    for (float t : targets) {
        const float da = t - edgeA;
        if (std::fabs(da) < best) {
            best = std::fabs(da);
            delta = da;
            outGuidePos = t;
            snapped = true;
        }
        const float db = t - edgeB;
        if (std::fabs(db) < best) {
            best = std::fabs(db);
            delta = db;
            outGuidePos = t;
            snapped = true;
        }
    }
    return snapped ? delta : 0.f;
}

ImageEditCollage::Handle ImageEditCollage::handleAt(POINT clientPos) const
{
    // 分组成组选中：整组（分割组/布局组）统一按外框四角 + 整边缩放
    if (!hasSelection()) return Handle::None;
    const int selGroup = selectionGroup();
    const float s = viewScale();
    float ox = 0.f, oy = 0.f;
    viewOrigin(ox, oy);
    auto art = artRect();
    D2D1_RECT_F b{};
    if (isLayoutGroup(selGroup)) {
        b = layoutGroupBounds(selGroup);
    }
    else {
        auto it = splitGroups_.find(selGroup);
        b = (selGroup >= 0 && it != splitGroups_.end())
            ? boundsOf(it->second.original)
            : boundsOf(cells_[selected_].poly);
    }
    const float l = ox + (b.left - art.left) * s;
    const float t = oy + (b.top - art.top) * s;
    const float r = ox + (b.right - art.left) * s;
    const float bo = oy + (b.bottom - art.top) * s;
    const float lx = (float)clientPos.x - x;
    const float ly = (float)clientPos.y - y;
    const float hs = kHandleSize * ((win && win->dpi > 0.f) ? win->dpi : 1.f);
    const float hh = hs * 0.5f + 3.f;

    // 四角控点优先（和标注一样，圆点命中半径固定）
    struct H { Handle h; float cx, cy; } corners[]{
        { Handle::TL, l, t }, { Handle::TR, r, t },
        { Handle::BR, r, bo }, { Handle::BL, l, bo },
    };
    for (const auto& p : corners) {
        if (std::fabs(lx - p.cx) <= hh && std::fabs(ly - p.cy) <= hh) return p.h;
    }

    // 整条边都能拖：贴边线的带状区域（角附近交给四角控点），和矩形标注一致
    const float band = 5.f * ((win && win->dpi > 0.f) ? win->dpi : 1.f);
    const bool inX = lx > l + hh && lx < r - hh;
    const bool inY = ly > t + hh && ly < bo - hh;
    if (inX && std::fabs(ly - t) <= band) return Handle::T;
    if (inX && std::fabs(ly - bo) <= band) return Handle::B;
    if (inY && std::fabs(lx - l) <= band) return Handle::L;
    if (inY && std::fabs(lx - r) <= band) return Handle::R;
    return Handle::None;
}

// ---------------- 状态 ----------------

bool ImageEditCollage::hasContent() const
{
    for (const auto& c : cells_)
        if (!c.image.empty()) return true;
    return !cells_.empty();
}

bool ImageEditCollage::hasSelection() const
{
    return selected_ >= 0 && selected_ < (int)cells_.size();
}

// 分割工具可用性：多选（跨组）/布局组（多格）不可分割；独立容器或已分割组可用
bool ImageEditCollage::canSplitSelection() const
{
    if (!hasSelection()) return false;   // 默认置灰：必须先选中单个容器才能使用分割
    const int g = cells_[selected_].group;
    if (isLayoutGroup(g)) return false;     // 布局组（有多格/多列）不能分割
    for (int idx : extraCells_) {
        if (idx < 0 || idx >= (int)cells_.size()) continue;
        if (g < 0) return false;                    // 独立容器又带了别的选中 = 多选
        if (cells_[idx].group != g) return false;   // 多选了不同组的容器
    }
    return true;
}

bool ImageEditCollage::isInteracting() const
{
    return dragging_ || resizing_ || panning_ || draggingDivider_ || marqueeSelecting_
        || adjustingSplit_ || rotatingImg_ || imgResizing_;
}

bool ImageEditCollage::isCellSelected(int index) const
{
    if (index < 0) return false;
    if (index == selected_) return true;
    return std::find(extraCells_.begin(), extraCells_.end(), index) != extraCells_.end();
}

void ImageEditCollage::clearCellSelection()
{
    selected_ = -1;
    extraCells_.clear();
    imageAdjust_ = false;
}

std::vector<int> ImageEditCollage::selectedCellIndices() const
{
    std::vector<int> out;
    if (hasSelection()) out.push_back(selected_);
    for (int i : extraCells_)
        if (i >= 0 && i < (int)cells_.size() && i != selected_) out.push_back(i);
    return out;
}

void ImageEditCollage::clear()
{
    // 工具栏「重置」只清空画面内容（布局/分割/图片/标注）；右侧属性区是「菜单数值」，
    // 外边框 / 间距 / 圆角 / 背景 / 行列 / 画布尺寸都保留，下次拖入内容继续沿用。
    cells_.clear();
    splitGroups_.clear();
    layoutGroups_.clear();
    selected_ = -1;
    extraCells_.clear();
    imageAdjust_ = false;
    contentFloorR_ = contentFloorB_ = -FLT_MAX;
    undoStack_.clear();
    redoStack_.clear();
    nextGroup_ = 1;
    zoomFactor_ = 1.f;
    panDX_ = panDY_ = 0.f;
    dragging_ = panning_ = resizing_ = imageAdjust_ = false;
    notifyChanged();
    if (win) win->refresh();
}

void ImageEditCollage::pushUndo()
{
    undoStack_.push_back(snapshot());
    if (undoStack_.size() > 50) undoStack_.erase(undoStack_.begin());
    redoStack_.clear();
}

ImageEditCollage::Snapshot ImageEditCollage::snapshot() const
{
    return { cells_, splitGroups_, layoutGroups_, nextGroup_, bgColor_, border_, gap_, radius_,
             rows_, cols_, manualW_, manualH_, manualL_, manualT_, sizeEnteredW_, sizeEnteredH_, selected_ };
}

void ImageEditCollage::restore(const Snapshot& s)
{
    cells_ = s.cells;
    splitGroups_ = s.groups;
    layoutGroups_ = s.layouts;
    nextGroup_ = s.nextGroup;
    bgColor_ = s.bgColor;
    border_ = s.border;
    gap_ = s.gap;
    radius_ = s.radius;
    rows_ = s.rows;
    cols_ = s.cols;
    manualW_ = s.manualW;
    manualH_ = s.manualH;
    manualL_ = s.manualL;
    manualT_ = s.manualT;
    sizeEnteredW_ = s.sizeEnteredW;
    sizeEnteredH_ = s.sizeEnteredH;
    selected_ = s.selected;
    captureContentExtents();
    if (selected_ >= (int)cells_.size()) selected_ = -1;
    extraCells_.clear();
    // 恢复选中时把同组成员补回（否则拖动只会动其中一格）
    const int selGroup = (selected_ >= 0) ? cells_[selected_].group : -1;
    if (selGroup >= 0) {
        for (int i = 0; i < (int)cells_.size(); ++i)
            if (i != selected_ && cells_[i].group == selGroup) extraCells_.push_back(i);
    }
    notifyChanged();
    if (win) win->refresh();
}

void ImageEditCollage::undo()
{
    if (undoStack_.empty()) return;
    redoStack_.push_back(snapshot());
    auto s = undoStack_.back();
    undoStack_.pop_back();
    restore(s);
}

void ImageEditCollage::redo()
{
    if (redoStack_.empty()) return;
    undoStack_.push_back(snapshot());
    auto s = redoStack_.back();
    redoStack_.pop_back();
    restore(s);
}

void ImageEditCollage::captureContentExtents()
{
    if (cells_.empty()) return;
    D2D1_RECT_F bbox{ FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (const auto& c : cells_) {
        if (c.poly.size() < 3) continue;
        auto b = boundsOf(c.poly);
        bbox.right = std::max(bbox.right, b.right);
        bbox.bottom = std::max(bbox.bottom, b.bottom);
    }
    if (bbox.right <= -FLT_MAX + 1.f) return;
    contentFloorR_ = bbox.right + border_;
    contentFloorB_ = bbox.bottom + border_;
    // 已经固定的画板边也算进基准：输入尺寸本身不该被内容立刻撑大
    if (manualW_ > 0) contentFloorR_ = std::max(contentFloorR_, (float)(manualL_ + manualW_));
    if (manualH_ > 0) contentFloorB_ = std::max(contentFloorB_, (float)(manualT_ + manualH_));
}

void ImageEditCollage::syncLockedBoard()
{
    if ((manualW_ <= 0 && manualH_ <= 0) || cells_.empty()) return;
    D2D1_RECT_F bbox{ FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (const auto& c : cells_) {
        if (c.poly.size() < 3) continue;
        auto b = boundsOf(c.poly);
        bbox.left = std::min(bbox.left, b.left);
        bbox.top = std::min(bbox.top, b.top);
        bbox.right = std::max(bbox.right, b.right);
        bbox.bottom = std::max(bbox.bottom, b.bottom);
    }
    if (bbox.right <= bbox.left) return;
    const float needR = bbox.right + border_;
    const float needB = bbox.bottom + border_;
    if (contentFloorR_ <= -FLT_MAX + 1.f) contentFloorR_ = needR;
    if (contentFloorB_ <= -FLT_MAX + 1.f) contentFloorB_ = needB;
    // 只有「输入尺寸之后内容又继续变大」才把画板撑大；输入值本身原样保留
    if (manualW_ > 0 && needR > contentFloorR_ + 0.01f) {
        const int need = (int)std::ceil(needR - (float)manualL_);
        manualW_ = std::max(manualW_, need);
        contentFloorR_ = needR;
    }
    if (manualH_ > 0 && needB > contentFloorB_ + 0.01f) {
        const int need = (int)std::ceil(needB - (float)manualT_);
        manualH_ = std::max(manualH_, need);
        contentFloorB_ = needB;
    }
}

void ImageEditCollage::notifyChanged(bool content)
{
    // 画板固定后只随内容变大（拖动/缩放容器可能把画板撑大，但不会缩回）
    syncLockedBoard();
    if (onSelectionChanged) onSelectionChanged();
    if (content && onContentChanged) onContentChanged();
}

// ---------------- 参数 ----------------

void ImageEditCollage::setTool(Tool tool)
{
    tool_ = tool;
    dragging_ = false;
    adjustingSplit_ = false;
    resizing_ = false;
    imageAdjust_ = false;
    if (win) win->refresh();
}

void ImageEditCollage::setToolById(const std::wstring& id)
{
    if (id == L"split") setTool(Tool::Split);
    else if (id == L"layout") setTool(Tool::Layout);
    else setTool(Tool::None);
}

void ImageEditCollage::setRowsCols(int rows, int cols)
{
    rows_ = std::max(0, std::min(6, rows));
    cols_ = std::max(0, std::min(6, cols));
}

std::vector<std::vector<ImageEditCollage::Pt>> ImageEditCollage::layoutPieceRects(const D2D1_RECT_F& rc) const
{
    std::vector<std::vector<Pt>> out;
    const float g = gap_;
    const float W = rc.right - rc.left;
    const float H = rc.bottom - rc.top;
    if (W < 8.f || H < 8.f) return out;

    if (layoutType_ == 0) {
        const int r = std::max(0, std::min(6, rows_));
        const int c = std::max(0, std::min(6, cols_));
        if (r <= 0 || c <= 0) { out.push_back(rectPoly(rc.left, rc.top, rc.right, rc.bottom)); return out; }
        const float cw = (W - g * (c - 1)) / c;
        const float ch = (H - g * (r - 1)) / r;
        if (cw < 8.f || ch < 8.f) { out.push_back(rectPoly(rc.left, rc.top, rc.right, rc.bottom)); return out; }
        for (int y = 0; y < r; ++y) {
            for (int x = 0; x < c; ++x) {
                const float l = rc.left + (cw + g) * x;
                const float t = rc.top + (ch + g) * y;
                out.push_back(rectPoly(l, t, l + cw, t + ch));
            }
        }
    }
    else if (layoutType_ == 1) {
        // 上和下：上下两条带，上带横向切 a 块、下带横向切 b 块
        const int a = std::max(1, std::min(6, rows_));
        const int b = std::max(1, std::min(6, cols_));
        const float mid = (rc.top + rc.bottom - g) * 0.5f;
        const float bandH = (H - g) * 0.5f;
        if (bandH < 8.f) { out.push_back(rectPoly(rc.left, rc.top, rc.right, rc.bottom)); return out; }
        const float wa = (W - g * (a - 1)) / a;
        const float wb = (W - g * (b - 1)) / b;
        if (wa < 8.f || wb < 8.f) { out.push_back(rectPoly(rc.left, rc.top, rc.right, rc.bottom)); return out; }
        for (int i = 0; i < a; ++i) {
            const float l = rc.left + (wa + g) * i;
            out.push_back(rectPoly(l, rc.top, l + wa, mid));
        }
        for (int i = 0; i < b; ++i) {
            const float l = rc.left + (wb + g) * i;
            out.push_back(rectPoly(l, mid + g, l + wb, rc.bottom));
        }
    }
    else {
        // 左和右：左右两条带，左带纵向切 a 块、右带纵向切 b 块
        const int a = std::max(1, std::min(6, rows_));
        const int b = std::max(1, std::min(6, cols_));
        const float mid = (rc.left + rc.right - g) * 0.5f;
        const float bandW = (W - g) * 0.5f;
        if (bandW < 8.f) { out.push_back(rectPoly(rc.left, rc.top, rc.right, rc.bottom)); return out; }
        const float ha = (H - g * (a - 1)) / a;
        const float hb = (H - g * (b - 1)) / b;
        if (ha < 8.f || hb < 8.f) { out.push_back(rectPoly(rc.left, rc.top, rc.right, rc.bottom)); return out; }
        for (int i = 0; i < a; ++i) {
            const float t = rc.top + (ha + g) * i;
            out.push_back(rectPoly(rc.left, t, mid, t + ha));
        }
        for (int i = 0; i < b; ++i) {
            const float t = rc.top + (hb + g) * i;
            out.push_back(rectPoly(mid + g, t, rc.right, t + hb));
        }
    }
    return out;
}

ImageEditCollage::Cell ImageEditCollage::makeSubCell(const Cell& base, const std::vector<Pt>& piece) const
{
    Cell c = base;
    c.poly = piece;
    c.group = -1;
    if (c.image.empty()) return c;

    auto bb = boundsOf(base.poly);
    const float baseW = std::max(1.f, bb.right - bb.left);
    const float baseH = std::max(1.f, bb.bottom - bb.top);
    const float nw0 = base.stretch ? 1.f : ((base.imgNW > 0.f) ? base.imgNW : 1.f);
    const float nh0 = base.stretch ? 1.f : ((base.imgNH > 0.f) ? base.imgNH : 1.f);
    const float absW = baseW * nw0 * base.imgScale;
    const float absH = baseH * nh0 * base.imgScale;
    const float baseCx = (bb.left + bb.right) * 0.5f + base.imgOffX * baseW;
    const float baseCy = (bb.top + bb.bottom) * 0.5f + base.imgOffY * baseH;

    auto pb = boundsOf(piece);
    const float tw = std::max(1.f, pb.right - pb.left);
    const float th = std::max(1.f, pb.bottom - pb.top);
    const float s = (base.imgScale > 0.f) ? base.imgScale : 1.f;
    c.stretch = false;
    c.imgScale = s;
    c.imgNW = absW / (tw * s);
    c.imgNH = absH / (th * s);
    c.imgOffX = (baseCx - (pb.left + pb.right) * 0.5f) / tw;
    c.imgOffY = (baseCy - (pb.top + pb.bottom) * 0.5f) / th;
    return c;
}

void ImageEditCollage::applyLayoutToSelection()
{
    if (!hasSelection()) return;   // 无选中：字段只作为拖拽生成时的初始属性
    // 已分割的容器不能再套布局：会把分割结果覆盖掉（对应属性输入框在窗口层已置灰）
    if (selectionIsSplitGroup()) return;
    // 选中布局组：保持组外框，按新类型/行列重建格位
    const int gid = layoutGroupOf(selected_);
    if (gid >= 0) {
        reshapeLayoutGroup(gid);
        return;
    }
    const auto base = cells_[selected_];
    auto b = boundsOf(base.poly);
    // 单独填写行或列也要立即生效：没填的那一轴按 1 计算（2 行 = 2×1），
    // 但「1」只用于这次切块，不改写输入框本身的值。
    const int r = std::clamp(rows_, 1, 6);
    const int c = std::clamp(cols_, 1, 6);
    const int saveR = rows_, saveC = cols_;
    rows_ = r;
    cols_ = c;
    auto pieces = layoutPieceRects(D2D1::RectF(b.left, b.top, b.right, b.bottom));
    rows_ = saveR;
    cols_ = saveC;
    if (pieces.size() <= 1) return;

    pushUndo();
    int at = selected_;
    cells_.erase(cells_.begin() + at);
    std::vector<int> members;
    for (const auto& piece : pieces) {
        cells_.insert(cells_.begin() + at, makeSubCell(base, piece));
        members.push_back(at++);
    }
    if (members.size() > 1) {
        LayoutGroupState grp;
        grp.type = layoutType_;
        grp.rows = r;
        grp.cols = c;
        grp.rect = D2D1::RectF(b.left, b.top, b.right, b.bottom);
        grp.members = members;
        const int newGroup = nextGroup_++;
        for (int idx : members) cells_[idx].group = newGroup;
        initLayoutFractions(grp);
        layoutGroups_[newGroup] = grp;
        rebuildLayoutGroup(newGroup);
    }
    selected_ = members.empty() ? -1 : members.front();
    extraCells_.clear();
    if (members.size() > 1) extraCells_.assign(members.begin() + 1, members.end());
    notifyChanged();
    if (win) win->refresh();
}

void ImageEditCollage::createLayoutInRect(const D2D1_RECT_F& rect)
{
    auto pieces = layoutPieceRects(rect);
    if (pieces.empty()) return;
    pushUndo();
    int at = (int)cells_.size();
    Cell base;
    base.poly = rectPoly(rect.left, rect.top, rect.right, rect.bottom);
    std::vector<int> members;
    for (const auto& piece : pieces) {
        cells_.insert(cells_.begin() + at, makeSubCell(base, piece));
        members.push_back(at++);
    }
    if (members.size() > 1) {
        // 布局生成的格子注册为一个组：点任意一格 = 选中整组
        LayoutGroupState grp;
        grp.type = layoutType_;
        grp.rows = rows_;
        grp.cols = cols_;
        grp.rect = D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom);
        grp.members = members;
        const int gid = nextGroup_++;
        for (int idx : members) cells_[idx].group = gid;
        initLayoutFractions(grp);
        layoutGroups_[gid] = grp;
        rebuildLayoutGroup(gid);
        selected_ = members.front();
        extraCells_.assign(members.begin() + 1, members.end());
    }
    else {
        selected_ = members.empty() ? -1 : members.front();
        extraCells_.clear();
    }
    // 拖拽布局和拖入首图一致：没有画板时按这次拖出的区域创建并固定画板
    if (manualW_ <= 0 && manualH_ <= 0) {
        const auto fixed = artRect();
        manualL_ = (int)std::lround(fixed.left);
        manualT_ = (int)std::lround(fixed.top);
        manualW_ = (int)std::lround(fixed.right - fixed.left);
        manualH_ = (int)std::lround(fixed.bottom - fixed.top);
        captureContentExtents();
        // 视图映射不随画板尺寸变化：画多大就是多大，也不挪到窗口中间
    }
    // 拖完布局取消选中：尺寸行显示画板尺寸
    selected_ = -1;
    extraCells_.clear();
    notifyChanged();
    if (win) win->refresh();
}

void ImageEditCollage::setBorder(float value)
{
    const float old = border_;
    border_ = std::max(0.f, std::min(400.f, value));
    const float delta = border_ - old;
    // 画板不是用户手输尺寸时：外边距变化直接体现为画板四周留白（内容位置不变），
    // 否则画板在第一次放图后就被固定，外边框会看起来「无效」
    if (std::fabs(delta) > 0.01f && !cells_.empty()) {
        bool changed = false;
        const int d = (int)std::lround(delta);
        if (manualW_ > 0 && !sizeEnteredW_) {
            manualL_ -= d;
            manualW_ = std::max(1, manualW_ + d * 2);
            changed = true;
        }
        if (manualH_ > 0 && !sizeEnteredH_) {
            manualT_ -= d;
            manualH_ = std::max(1, manualH_ + d * 2);
            changed = true;
        }
        if (changed) captureContentExtents();
    }
    if (win) win->refresh();
}

void ImageEditCollage::setGap(float value)
{
    const float next = std::max(0.f, std::min(200.f, value));
    const float delta = next - gap_;
    if (std::fabs(delta) < 0.01f) return;
    gap_ = next;
    // 分割块之间的缝由分割线两侧各让一半：间距变化时按新间距整体重算
    std::vector<int> groups;
    for (const auto& [gid, grp] : splitGroups_) groups.push_back(gid);
    for (int gid : groups) rebuildSplitGroup(gid);
    // 布局组的缝按格位比例重算（组外框不变，格子按新缝收放）
    std::vector<int> layoutGids;
    for (const auto& [gid, grp] : layoutGroups_) layoutGids.push_back(gid);
    for (int gid : layoutGids) rebuildLayoutGroup(gid);
    reflowGap(delta);
    if (win) win->refresh();
}

// 间距是“容器与容器之间”的缝：改间距时把相邻容器原地推开/收拢，不改容器自身大小
void ImageEditCollage::reflowGap(float delta)
{
    if (cells_.size() < 2 || std::fabs(delta) < 0.01f) return;
    const float oldGap = std::max(0.f, gap_ - delta);
    const float tol = oldGap + 2.f;
    const float eps = 0.5f;
    const float half = delta * 0.5f;

    // 相邻容器各自让出一半，保证中心位置不变、只改缝
    for (size_t i = 0; i < cells_.size(); ++i) {
        if (cells_[i].group >= 0) continue;   // 分割块的缝已由 rebuildSplitGroup 按新间距重算
        for (size_t j = i + 1; j < cells_.size(); ++j) {
            if (cells_[j].group >= 0) continue;
            auto a = boundsOf(cells_[i].poly);
            auto b = boundsOf(cells_[j].poly);
            const bool yOverlap = std::min(a.bottom, b.bottom) - std::max(a.top, b.top) > 1.f;
            const bool xOverlap = std::min(a.right, b.right) - std::max(a.left, b.left) > 1.f;

            float dirX = 0.f, dirY = 0.f;
            if (yOverlap) {
                if (b.left >= a.right - eps && b.left - a.right <= tol) dirX = -1.f;       // j 在 i 右
                else if (a.left >= b.right - eps && a.left - b.right <= tol) dirX = 1.f;  // j 在 i 左
            }
            if (xOverlap) {
                if (b.top >= a.bottom - eps && b.top - a.bottom <= tol) dirY = -1.f;       // j 在 i 下
                else if (a.top >= b.bottom - eps && a.top - b.bottom <= tol) dirY = 1.f;  // j 在 i 上
            }
            if (dirX == 0.f && dirY == 0.f) continue;

            if (dirX != 0.f) {
                translateCell(cells_[i], dirX * half, 0.f);
                translateCell(cells_[j], -dirX * half, 0.f);
            }
            if (dirY != 0.f) {
                translateCell(cells_[i], 0.f, dirY * half);
                translateCell(cells_[j], 0.f, -dirY * half);
            }
        }
    }
}

void ImageEditCollage::setRadius(float value)
{
    radius_ = std::max(0.f, std::min(200.f, value));
    if (win) win->refresh();
}

void ImageEditCollage::setBackgroundColor(uint32_t rgba)
{
    bgColor_ = rgba;
    if (win) win->refresh();
}

void ImageEditCollage::setCanvasSize(int w, int h)
{
    // 每个轴独立：<=0 表示该轴自动（由内容 + 外边距决定）
    const int nextW = (w > 0) ? std::max(1, std::min(10000, w)) : 0;
    const int nextH = (h > 0) ? std::max(1, std::min(10000, h)) : 0;
    // 手工输入尺寸 = 该轴以原点为左上角固定；已经固定的轴保持它原来的原点（输入数值不会让画板跳动）
    if (nextW > 0 && manualW_ == 0) manualL_ = 0;
    if (nextH > 0 && manualH_ == 0) manualT_ = 0;
    manualW_ = nextW;
    manualH_ = nextH;
    // 绿色只在「用户主动输入」时亮起：清空某轴 = 该轴恢复自适应，标志也关掉
    sizeEnteredW_ = nextW > 0;
    sizeEnteredH_ = nextH > 0;
    captureContentExtents();   // 输入值即基准：与自适应值一致也会被固定住
    notifyChanged(false);
    if (win) win->refresh();
}

std::wstring ImageEditCollage::zoomText() const
{
    return wfmt(L"%d%%", (int)std::lround(zoomFactor_ * 100.f));
}

bool ImageEditCollage::selectedLayoutGroupInfo(int& type, int& rows, int& cols) const
{
    if (!hasSelection()) return false;
    auto it = layoutGroups_.find(selectionGroup());
    if (it == layoutGroups_.end()) return false;
    type = it->second.type;
    rows = it->second.rows;
    cols = it->second.cols;
    return true;
}

bool ImageEditCollage::selectedCellSize(int& w, int& h) const
{
    if (!hasSelection()) return false;
    // 组按整体：分割组显示原容器尺寸，布局组显示组外框尺寸
    const int g = selectionGroup();
    D2D1_RECT_F rc;
    if (isLayoutGroup(g)) {
        rc = layoutGroupBounds(g);
    }
    else {
        auto it = splitGroups_.find(g);
        rc = (g >= 0 && it != splitGroups_.end())
            ? boundsOf(it->second.original)
            : boundsOf(cells_[selected_].poly);
    }
    w = std::max(1, (int)std::lround(rc.right - rc.left));
    h = std::max(1, (int)std::lround(rc.bottom - rc.top));
    return true;
}

// 选中容器时按输入尺寸缩放它（左上角固定）；布局组整体缩放，分割组不允许单块改尺寸
bool ImageEditCollage::setSelectedCellSize(int w, int h)
{
    if (!hasSelection()) return false;
    const int selGroup = selectionGroup();
    if (isLayoutGroup(selGroup)) {
        auto& grp = layoutGroups_[selGroup];
        auto b = grp.rect;
        int tw = (w > 0) ? std::min(10000, w) : (int)std::lround(b.right - b.left);
        int th = (h > 0) ? std::min(10000, h) : (int)std::lround(b.bottom - b.top);
        tw = std::max(1, tw);
        th = std::max(1, th);
        const float sx = (float)tw / std::max(1.f, b.right - b.left);
        const float sy = (float)th / std::max(1.f, b.bottom - b.top);
        if (std::fabs(sx - 1.f) < 0.001f && std::fabs(sy - 1.f) < 0.001f) return true;
        pushUndo();
        for (int idx : grp.members) {
            if (idx < 0 || idx >= (int)cells_.size()) continue;
            for (auto& p : cells_[idx].poly) {
                p.x = b.left + (p.x - b.left) * sx;
                p.y = b.top + (p.y - b.top) * sy;
            }
        }
        grp.rect = D2D1::RectF(b.left, b.top, b.left + tw, b.top + th);
        notifyChanged(false);
        if (win) win->refresh();
        return true;
    }
    if (selGroup >= 0) {
        // 分割组：整体等比例缩放原容器、分割线与各块；其它组不支持按数值改尺寸
        auto splitIt = splitGroups_.find(selGroup);
        if (splitIt == splitGroups_.end()) return false;
        auto b = boundsOf(splitIt->second.original);
        int tw = (w > 0) ? std::min(10000, w) : (int)std::lround(b.right - b.left);
        int th = (h > 0) ? std::min(10000, h) : (int)std::lround(b.bottom - b.top);
        tw = std::max(1, tw);
        th = std::max(1, th);
        const float sx = (float)tw / std::max(1.f, b.right - b.left);
        const float sy = (float)th / std::max(1.f, b.bottom - b.top);
        if (std::fabs(sx - 1.f) < 0.001f && std::fabs(sy - 1.f) < 0.001f) return true;
        pushUndo();
        auto& grp = splitIt->second;
        auto mapPt = [&](Pt& q) {
            q.x = b.left + (q.x - b.left) * sx;
            q.y = b.top + (q.y - b.top) * sy;
            };
        for (auto& q : grp.original) mapPt(q);
        for (auto& L : grp.lines) { mapPt(L.a); mapPt(L.b); }
        for (int idx : selectedCellIndices()) {
            if (idx < 0 || idx >= (int)cells_.size() || cells_[idx].group != selGroup) continue;
            for (auto& q : cells_[idx].poly) mapPt(q);
        }
        notifyChanged(false);
        if (win) win->refresh();
        return true;
    }
    auto& c = cells_[selected_];
    auto b = boundsOf(c.poly);
    // <=0 的轴保持原尺寸（清空输入框不会把容器缩成 1px）
    int tw = (w > 0) ? std::min(10000, w) : (int)std::lround(b.right - b.left);
    int th = (h > 0) ? std::min(10000, h) : (int)std::lround(b.bottom - b.top);
    tw = std::max(1, tw);
    th = std::max(1, th);
    const float sx = (float)tw / std::max(1.f, b.right - b.left);
    const float sy = (float)th / std::max(1.f, b.bottom - b.top);
    if (std::fabs(sx - 1.f) < 0.001f && std::fabs(sy - 1.f) < 0.001f) return true;
    pushUndo();
    for (auto& p : c.poly) {
        p.x = b.left + (p.x - b.left) * sx;
        p.y = b.top + (p.y - b.top) * sy;
    }
    notifyChanged(false);
    if (win) win->refresh();
    return true;
}

// ---------------- 布局 ----------------

void ImageEditCollage::applyLayoutPreset(int index)
{
    layoutType_ = std::max(0, std::min(2, index));
    // 选中布局组时切回「自由」保留它当前的行列，避免误把组重置成单格
    const bool groupSelected = hasSelection() && isLayoutGroup(selectionGroup());
    if (layoutType_ == 0) {
        if (!groupSelected) { rows_ = 1; cols_ = 1; }      // 自由：默认 1×1
    }
    else if (layoutType_ == 1) { rows_ = 1; cols_ = 1; }    // 上和下：1 / 1
    else { rows_ = 2; cols_ = 1; }                          // 左和右：2 / 1
    applyLayoutToSelection();   // 有选中容器就立即按该编组切；没有就只作为初始属性
}

// ---------------- 分割 ----------------

void ImageEditCollage::commitSplit(int index, const Pt& a, const Pt& b)
{
    if (index < 0 || index >= (int)cells_.size()) return;
    if (layoutGroupOf(index) >= 0) return;   // 布局组内不走自由分割
    // 分割线就是用户画的那条线段：与容器边缘相交的两个点才是分割控点。
    // 只擦到一个边（或整条线在容器内/外）都不生效
    Pt ca, cb;
    if (!splitSegmentForCell(index, a, b, ca, cb)) return;

    Cell& cell = cells_[index];
    int group = cell.group;
    auto it = (group >= 0) ? splitGroups_.find(group) : splitGroups_.end();
    if (it != splitGroups_.end()) {
        // 不限制刀数：每画一条有效分割线就追加一刀
        pushUndo();
        it->second.lines.push_back({ ca, cb });
        rebuildSplitGroup(group);
        return;
    }

    // 新编组：原容器作为底，按第一条线切
    pushUndo();
    int g = nextGroup_++;
    SplitGroup grp;
    grp.original = cell.poly;
    grp.base = cell;
    grp.base.group = g;
    grp.lines.push_back({ ca, cb });
    splitGroups_[g] = grp;
    cell.group = g;
    rebuildSplitGroup(g);
}

bool ImageEditCollage::splitSegmentForCell(int index, const Pt& a, const Pt& b, Pt& outA, Pt& outB) const
{
    if (index < 0 || index >= (int)cells_.size()) return false;
    // 已分割组按「原容器」判断：第二刀可以跨过整组，而不是只跨过当前这一小块
    std::vector<Pt> poly;
    const int g = cells_[index].group;
    auto it = (g >= 0) ? splitGroups_.find(g) : splitGroups_.end();
    if (it != splitGroups_.end()) poly = it->second.original;
    else poly = cells_[index].poly;
    if (poly.size() < 3) return false;

    const float dx = b.x - a.x, dy = b.y - a.y;
    if (std::sqrt(dx * dx + dy * dy) < 1.f) return false;

    // 线段与容器包围盒的 slab 相交（容器都是轴对齐矩形）：t 是线段参数，0..1 为整条线
    const auto bb = boundsOf(poly);
    float t0 = -FLT_MAX, t1 = FLT_MAX;
    bool miss{ false };
    auto clampT = [&](float p0, float dir, float lo, float hi) {
        if (std::fabs(dir) < 1e-4f) {
            if (p0 < lo || p0 > hi) miss = true;   // 平行且在盒外：不相交
            return;
        }
        float ta = (lo - p0) / dir, tb = (hi - p0) / dir;
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
    };
    clampT(a.x, dx, bb.left, bb.right);
    clampT(a.y, dy, bb.top, bb.bottom);
    if (miss || t1 <= t0) return false;
    // 两个交点都必须落在画出的线段内部（两端都在容器外）= 真正穿过了两条边
    constexpr float kEps{ 1e-3f };
    if (t0 <= kEps || t1 >= 1.f - kEps) return false;

    outA = { a.x + dx * t0, a.y + dy * t0 };
    outB = { a.x + dx * t1, a.y + dy * t1 };
    return true;
}

int ImageEditCollage::splitTargetForSegment(const Pt& a, const Pt& b) const
{
    // 分割工具只在选中单个容器时可用：优先作用在选中的容器上
    if (selected_ >= 0 && selected_ < (int)cells_.size() && layoutGroupOf(selected_) < 0) {
        Pt ca, cb;
        if (splitSegmentForCell(selected_, a, b, ca, cb)) return selected_;
    }
    // 线段中点落在哪个容器里就切哪个（最直观：线穿过谁就切谁）
    const Pt mid{ (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f };
    for (int i = 0; i < (int)cells_.size(); ++i) {
        if (layoutGroupOf(i) >= 0) continue;
        if (pointInPoly(cells_[i].poly, mid)) return i;
    }
    // 中点正好落在块间缝隙里时，退回「第一个被这条线完整穿过的容器」
    for (int i = 0; i < (int)cells_.size(); ++i) {
        if (layoutGroupOf(i) >= 0) continue;
        Pt ca, cb;
        if (splitSegmentForCell(i, a, b, ca, cb)) return i;
    }
    return -1;
}

bool ImageEditCollage::nearestSplitLine(int group, const Pt& p, float tol, int& lineIndex) const
{
    lineIndex = -1;
    auto it = splitGroups_.find(group);
    if (it == splitGroups_.end()) return false;
    float best = tol;
    for (int i = 0; i < (int)it->second.lines.size(); ++i) {
        const auto& L = it->second.lines[i];
        const float vx = L.b.x - L.a.x, vy = L.b.y - L.a.y;
        const float len2 = vx * vx + vy * vy;
        if (len2 < 0.01f) continue;
        float t = ((p.x - L.a.x) * vx + (p.y - L.a.y) * vy) / len2;
        t = std::max(0.f, std::min(1.f, t));
        const float qx = L.a.x + vx * t, qy = L.a.y + vy * t;
        const float dist = std::sqrt((p.x - qx) * (p.x - qx) + (p.y - qy) * (p.y - qy));
        if (dist <= best) { best = dist; lineIndex = i; }
    }
    return lineIndex >= 0;
}

void ImageEditCollage::deleteSplitLine(int group, int lineIndex)
{
    auto it = splitGroups_.find(group);
    if (it == splitGroups_.end()) return;
    if (lineIndex < 0 || lineIndex >= (int)it->second.lines.size()) return;
    pushUndo();
    it->second.lines.erase(it->second.lines.begin() + lineIndex);
    rebuildSplitGroup(group);
}

// 分割线持久化：每次重算都从原容器出发，重新裁剪块、重新换算图片切片、按间距分离
void ImageEditCollage::rebuildSplitGroup(int group)
{
    auto it = splitGroups_.find(group);
    if (it == splitGroups_.end()) return;
    auto& grp = it->second;

    std::vector<std::vector<Pt>> pieces{ grp.original };
    for (const auto& L : grp.lines) {
        std::vector<std::vector<Pt>> next;
        for (const auto& poly : pieces) {
            auto p1 = clipHalf(poly, L.a, L.b, true);
            auto p2 = clipHalf(poly, L.a, L.b, false);
            if (p1.size() >= 3 && std::abs(polyArea(p1)) >= 25.f) next.push_back(p1);
            if (p2.size() >= 3 && std::abs(polyArea(p2)) >= 25.f) next.push_back(p2);
        }
        pieces.swap(next);
    }

    std::vector<int> members;
    for (int i = 0; i < (int)cells_.size(); ++i)
        if (cells_[i].group == group) members.push_back(i);
    // 重建前先留一份旧块：用户可能给某个分割块单独加过图片，重建时按位置继承回来
    std::vector<Cell> oldPieces;
    oldPieces.reserve(members.size());
    for (int idx : members) oldPieces.push_back(cells_[idx]);
    int insertAt = members.empty() ? (int)cells_.size() : members.front();
    for (int i = (int)members.size() - 1; i >= 0; --i)
        cells_.erase(cells_.begin() + members[i]);

    if (grp.lines.empty() || pieces.size() < 2) {
        Cell c = grp.base;
        c.poly = grp.original;
        c.group = -1;
        cells_.insert(cells_.begin() + insertAt, c);
        selected_ = insertAt;
        extraCells_.clear();
        splitGroups_.erase(it);
        notifyChanged();
        if (win) win->refresh();
        return;
    }

    // 原图片在切换前的「绝对绘制框」，用于换算每块显示的那一片
    const auto baseB = boundsOf(grp.original);
    const float baseW = std::max(1.f, baseB.right - baseB.left);
    const float baseH = std::max(1.f, baseB.bottom - baseB.top);
    float absW = 0.f, absH = 0.f, baseCx = 0.f, baseCy = 0.f;
    if (!grp.base.image.empty()) {
        const float nw0 = grp.base.stretch ? 1.f : ((grp.base.imgNW > 0.f) ? grp.base.imgNW : 1.f);
        const float nh0 = grp.base.stretch ? 1.f : ((grp.base.imgNH > 0.f) ? grp.base.imgNH : 1.f);
        absW = baseW * nw0 * grp.base.imgScale;
        absH = baseH * nh0 * grp.base.imgScale;
        baseCx = (baseB.left + baseB.right) * 0.5f + grp.base.imgOffX * baseW;
        baseCy = (baseB.top + baseB.bottom) * 0.5f + grp.base.imgOffY * baseH;
    }

    const float g = gap_;
    for (const auto& piece : pieces) {
        // 间距不再整块平移，而是把每块沿分割线往自己一侧「内缩」半个间距：
        // 外侧边保持不动，间距从容器内部让出来，整个分割组不会超出原布局范围。
        std::vector<Pt> shaped = piece;
        float cx = 0.f, cy = 0.f;
        for (const auto& v : piece) { cx += v.x; cy += v.y; }
        if (!piece.empty()) { cx /= (float)piece.size(); cy /= (float)piece.size(); }
        for (const auto& L : grp.lines) {
            const float dx = L.b.x - L.a.x, dy = L.b.y - L.a.y;
            const float len = std::sqrt(dx * dx + dy * dy);
            if (len < 1.f) continue;
            const float side = dx * (cy - L.a.y) - dy * (cx - L.a.x);
            const bool keepPositive = side >= 0.f;
            const float sign = keepPositive ? 1.f : -1.f;
            const float nx = -dy / len, ny = dx / len;   // 法线：指向「正侧」
            // 只沿水平/垂直方向让开（近水平的分割线上下让，近垂直的左右让）
            float ox = 0.f, oy = 0.f;
            if (std::fabs(nx) > std::fabs(ny))
                ox = sign * (g * 0.5f) * (nx >= 0.f ? 1.f : -1.f);
            else
                oy = sign * (g * 0.5f) * (ny >= 0.f ? 1.f : -1.f);
            if (ox == 0.f && oy == 0.f) continue;
            auto clipped = clipHalf(shaped, { L.a.x + ox, L.a.y + oy },
                { L.b.x + ox, L.b.y + oy }, keepPositive);
            if (clipped.size() >= 3 && std::abs(polyArea(clipped)) >= 25.f)
                shaped.swap(clipped);
        }

        Cell c = grp.base;
        // 间距过大把这块吃没了：退回不内缩，保证块还在
        c.poly = (shaped.size() >= 3) ? shaped : piece;
        c.group = group;

        // 这块曾单独加过图（双击选图填进单块）：按几何位置继承，
        // 避免之后改间距、拖分割线触发重算时把图丢掉
        bool ownImage = false;
        if (!oldPieces.empty()) {
            const auto nb = boundsOf(c.poly);
            const Pt mid{ (nb.left + nb.right) * 0.5f, (nb.top + nb.bottom) * 0.5f };
            for (const auto& op : oldPieces) {
                if (op.image.empty()) continue;
                if (!grp.base.image.empty() && op.image == grp.base.image) continue;   // 原图切片不用继承
                if (!pointInPoly(op.poly, mid)) continue;
                c.image = op.image;
                c.stretch = op.stretch;
                c.imgScale = 1.f;
                c.imgOffX = c.imgOffY = 0.f;
                c.imgNW = c.imgNH = 0.f;
                normalizeCellImage(c);
                ownImage = true;
                break;
            }
        }

        const auto pb = boundsOf(c.poly);
        const float tw = std::max(1.f, pb.right - pb.left);
        const float th = std::max(1.f, pb.bottom - pb.top);
        if (!ownImage && !c.image.empty() && absW > 0.f && absH > 0.f) {
            c.stretch = false;
            const float s = (c.imgScale > 0.f) ? c.imgScale : 1.f;
            c.imgScale = s;
            c.imgNW = absW / (tw * s);
            c.imgNH = absH / (th * s);
            c.imgOffX = (baseCx - (pb.left + pb.right) * 0.5f) / tw;
            c.imgOffY = (baseCy - (pb.top + pb.bottom) * 0.5f) / th;
        }

        cells_.insert(cells_.begin() + insertAt, c);
        ++insertAt;
    }
    // 整组默认选中：任意一块都代表整体（框住原容器，显示可拖的分割线）
    std::vector<int> members2;
    for (int i = 0; i < (int)cells_.size(); ++i)
        if (cells_[i].group == group) members2.push_back(i);
    selected_ = members2.empty() ? -1 : members2.front();
    extraCells_.clear();
    if (members2.size() > 1) extraCells_.assign(members2.begin() + 1, members2.end());
    notifyChanged();
    if (win) win->refresh();
}

int ImageEditCollage::selectionGroup() const
{
    if (!hasSelection()) return -1;
    return cells_[selected_].group;
}

bool ImageEditCollage::selectionIsSplitGroup() const
{
    const int g = selectionGroup();
    return g >= 0 && splitGroups_.find(g) != splitGroups_.end();
}

// 选中分割组里任意一块 = 选中整组
void ImageEditCollage::selectCellGroup(int hit)
{
    extraCells_.clear();
    imageAdjust_ = false;
    if (hit < 0 || hit >= (int)cells_.size()) { selected_ = -1; return; }
    selected_ = hit;
    const int g = cells_[hit].group;
    if (g < 0) return;
    for (int i = 0; i < (int)cells_.size(); ++i)
        if (i != hit && cells_[i].group == g) extraCells_.push_back(i);
}

// ---------------- 布局组 --------------

bool ImageEditCollage::isLayoutGroup(int group) const
{
    return group >= 0 && layoutGroups_.find(group) != layoutGroups_.end();
}

int ImageEditCollage::layoutGroupOf(int cellIndex) const
{
    if (cellIndex < 0 || cellIndex >= (int)cells_.size()) return -1;
    const int g = cells_[cellIndex].group;
    return isLayoutGroup(g) ? g : -1;
}

D2D1_RECT_F ImageEditCollage::layoutGroupBounds(int group) const
{
    auto it = layoutGroups_.find(group);
    if (it == layoutGroups_.end()) return D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F rc = it->second.rect;
    if (rc.right > rc.left && rc.bottom > rc.top) return rc;
    // 兜底：外框异常时用成员包围盒
    rc = D2D1::RectF(FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX);
    for (int idx : it->second.members) {
        if (idx < 0 || idx >= (int)cells_.size()) continue;
        auto b = boundsOf(cells_[idx].poly);
        rc.left = std::min(rc.left, b.left);
        rc.top = std::min(rc.top, b.top);
        rc.right = std::max(rc.right, b.right);
        rc.bottom = std::max(rc.bottom, b.bottom);
    }
    if (rc.right <= rc.left || rc.bottom <= rc.top) rc = D2D1::RectF(0, 0, 0, 0);
    return rc;
}

void ImageEditCollage::initLayoutFractions(LayoutGroupState& grp) const
{
    auto even = [](int n) {
        std::vector<float> out;
        if (n > 0) out.assign((size_t)n, 1.f / (float)n);
        return out;
        };
    grp.band = 0.5f;
    if (grp.type == 0) {
        grp.fr1 = even(grp.cols);
        grp.fr2 = even(grp.rows);
    }
    else {
        grp.fr1 = even(grp.rows);   // 上下=上带格数；左右=左带格数
        grp.fr2 = even(grp.cols);   // 上下=下带格数；左右=右带格数
    }
}

std::vector<D2D1_RECT_F> ImageEditCollage::layoutSlotRects(const LayoutGroupState& grp) const
{
    std::vector<D2D1_RECT_F> out;
    const float g = gap_;
    const auto rc = grp.rect;
    const float W = rc.right - rc.left;
    const float H = rc.bottom - rc.top;
    if (W < 4.f || H < 4.f) return out;

    auto cols = [&](const std::vector<float>& fr, float x0, float y0, float h, float totalW) {
        if (fr.empty()) return;
        const float avail = totalW - g * ((float)fr.size() - 1.f);
        if (avail < 4.f) return;
        float x = x0;
        for (size_t i = 0; i < fr.size(); ++i) {
            const float w = avail * fr[i];
            out.push_back(D2D1::RectF(x, y0, x + w, y0 + h));
            x += w + g;
        }
    };
    auto rows = [&](const std::vector<float>& fr, float y0, float x0, float w, float totalH) {
        if (fr.empty()) return;
        const float avail = totalH - g * ((float)fr.size() - 1.f);
        if (avail < 4.f) return;
        float y = y0;
        for (size_t i = 0; i < fr.size(); ++i) {
            const float h = avail * fr[i];
            out.push_back(D2D1::RectF(x0, y, x0 + w, y + h));
            y += h + g;
        }
    };

    if (grp.type == 0) {
        const int r = std::max(0, grp.rows), c = std::max(0, grp.cols);
        if (r <= 0 || c <= 0) { out.push_back(rc); return out; }
        const float availH = H - g * (float)(r - 1);
        if (availH < 4.f) return out;
        float y = rc.top;
        for (int j = 0; j < r && j < (int)grp.fr2.size(); ++j) {
            const float h = availH * grp.fr2[j];
            cols(grp.fr1, rc.left, y, h, W);
            y += h + g;
        }
        return out;
    }
    const int a = std::max(0, grp.rows), b = std::max(0, grp.cols);
    if (a <= 0 && b <= 0) { out.push_back(rc); return out; }
    if (grp.type == 1) {
        // 上和下：两带高度由 band 分
        float hTop = 0.f, hBot = 0.f, yTop = rc.top, yBot = rc.top;
        if (a > 0 && b > 0) {
            const float availH = H - g;
            hTop = availH * grp.band;
            hBot = availH - hTop;
            yBot = rc.top + hTop + g;
        }
        else if (a > 0) hTop = H;
        else { hBot = H; yBot = rc.top; }
        if (a > 0) cols(grp.fr1, rc.left, yTop, hTop, W);
        if (b > 0) cols(grp.fr2, rc.left, yBot, hBot, W);
        return out;
    }
    // 左和右：两带宽度由 band 分
    float wLeft = 0.f, wRight = 0.f, xLeft = rc.left, xRight = rc.left;
    if (a > 0 && b > 0) {
        const float availW = W - g;
        wLeft = availW * grp.band;
        wRight = availW - wLeft;
        xRight = rc.left + wLeft + g;
    }
    else if (a > 0) wLeft = W;
    else { wRight = W; xRight = rc.left; }
    if (a > 0) rows(grp.fr1, rc.top, xLeft, wLeft, H);
    if (b > 0) rows(grp.fr2, rc.top, xRight, wRight, H);
    return out;
}

void ImageEditCollage::rebuildLayoutGroup(int group)
{
    auto it = layoutGroups_.find(group);
    if (it == layoutGroups_.end()) return;
    auto& grp = it->second;
    std::vector<D2D1_RECT_F> rects = layoutSlotRects(grp);

    // 记录每块图片当前的「绝对绘制框」：缝隙拖动 = 裁切窗口变化，图片本身不跟着拉伸
    struct ImgFrame { bool has{ false }; float absW{ 0.f }, absH{ 0.f }, cx{ 0.f }, cy{ 0.f }, scale{ 1.f }; };
    std::vector<ImgFrame> frames(grp.members.size());
    for (size_t k = 0; k < grp.members.size(); ++k) {
        const int idx = grp.members[k];
        if (idx < 0 || idx >= (int)cells_.size()) continue;
        auto& c = cells_[idx];
        if (c.image.empty()) continue;
        auto b = boundsOf(c.poly);
        const float tw = std::max(1.f, b.right - b.left);
        const float th = std::max(1.f, b.bottom - b.top);
        const float nw = c.stretch ? 1.f : ((c.imgNW > 0.f) ? c.imgNW : 1.f);
        const float nh = c.stretch ? 1.f : ((c.imgNH > 0.f) ? c.imgNH : 1.f);
        auto& f = frames[k];
        f.has = true;
        f.scale = (c.imgScale > 0.f) ? c.imgScale : 1.f;
        f.absW = tw * nw * f.scale;
        f.absH = th * nh * f.scale;
        f.cx = (b.left + b.right) * 0.5f + c.imgOffX * tw;
        f.cy = (b.top + b.bottom) * 0.5f + c.imgOffY * th;
    }

    const size_t n = std::min(rects.size(), grp.members.size());
    for (size_t k = 0; k < n; ++k) {
        const int idx = grp.members[k];
        if (idx < 0 || idx >= (int)cells_.size()) continue;
        auto& c = cells_[idx];
        c.group = group;
        const auto& rc = rects[k];
        c.poly = rectPoly(rc.left, rc.top, rc.right, rc.bottom);
        auto& f = frames[k];
        if (f.has && !c.image.empty()) {
            const float tw = std::max(1.f, rc.right - rc.left);
            const float th = std::max(1.f, rc.bottom - rc.top);
            c.imgScale = f.scale;
            c.imgNW = f.absW / (tw * f.scale);
            c.imgNH = f.absH / (th * f.scale);
            c.imgOffX = (f.cx - (rc.left + rc.right) * 0.5f) / tw;
            c.imgOffY = (f.cy - (rc.top + rc.bottom) * 0.5f) / th;
        }
    }
}

// 改布局类型/行列数后重建组：保持外框、按格位顺序保留已有图片，多出的格位清空
void ImageEditCollage::reshapeLayoutGroup(int group)
{
    auto it = layoutGroups_.find(group);
    if (it == layoutGroups_.end()) return;
    pushUndo();
    LayoutGroupState grp = it->second;   // 用副本，后面可能删组

    const int type = std::max(0, std::min(2, layoutType_));
    // 单独填写行或列也立即响应：没填的那一轴按 1 处理
    const int a = std::clamp(rows_ > 0 ? rows_ : 1, 1, 6);
    const int b = std::clamp(cols_ > 0 ? cols_ : 1, 1, 6);
    const int want = (type == 0) ? a * b : a + b;

    // 删格子会改变 cells_ 下标：统一修正所有组/选中记录
    auto eraseCellAt = [this](int idx) {
        if (idx < 0 || idx >= (int)cells_.size()) return;
        cells_.erase(cells_.begin() + idx);
        for (auto& kv : layoutGroups_)
            for (auto& m : kv.second.members)
                if (m > idx) --m;
        if (selected_ > idx) --selected_;
        else if (selected_ == idx) selected_ = -1;
        for (auto& e : extraCells_)
            if (e > idx) --e;
        extraCells_.erase(std::remove(extraCells_.begin(), extraCells_.end(), idx), extraCells_.end());
        };

    grp.type = type;
    grp.rows = a;
    grp.cols = b;

    if (want <= 1) {
        // 收成单格：保留第一格的图片，恢复成普通容器
        layoutGroups_.erase(it);
        int keep = grp.members.empty() ? -1 : grp.members.front();
        for (int i = (int)grp.members.size() - 1; i >= 1; --i) {
            const int idx = grp.members[i];
            eraseCellAt(idx);
            if (keep > idx) --keep;   // eraseCellAt 只修正 map 里的下标，本地 keep 要自己跟
        }
        if (keep >= 0 && keep < (int)cells_.size()) {
            auto& c = cells_[keep];
            c.group = -1;
            c.poly = rectPoly(grp.rect.left, grp.rect.top, grp.rect.right, grp.rect.bottom);
            if (!c.image.empty()) normalizeCellImage(c);
            selected_ = keep;
            extraCells_.clear();
        }
        notifyChanged();
        if (win) win->refresh();
        return;
    }

    // 成员数量增减（多出的格位从尾部删/加）
    while ((int)grp.members.size() > want) {
        const int idx = grp.members.back();
        grp.members.pop_back();
        eraseCellAt(idx);
        // 本地成员下标同步左移（eraseCellAt 只修正 map 里的副本）
        for (auto& m : grp.members)
            if (m > idx) --m;
    }
    while ((int)grp.members.size() < want) {
        Cell empty;
        empty.group = group;
        empty.poly = rectPoly(grp.rect.left, grp.rect.top, grp.rect.right, grp.rect.bottom);
        cells_.push_back(empty);
        grp.members.push_back((int)cells_.size() - 1);
    }

    initLayoutFractions(grp);
    layoutGroups_[group] = grp;
    rebuildLayoutGroup(group);
    selected_ = grp.members.empty() ? -1 : grp.members.front();
    extraCells_.clear();
    if (grp.members.size() > 1) extraCells_.assign(grp.members.begin() + 1, grp.members.end());
    notifyChanged();
    if (win) win->refresh();
}

std::vector<ImageEditCollage::LayoutDivider> ImageEditCollage::layoutDividers(const LayoutGroupState& grp) const
{
    std::vector<LayoutDivider> out;
    const float g = gap_;
    const auto rc = grp.rect;
    const float W = rc.right - rc.left;
    const float H = rc.bottom - rc.top;
    if (W < 4.f || H < 4.f) return out;

    auto verticals = [&](int array, const std::vector<float>& fr, float x0, float y0, float y1, float totalW) {
        if (fr.size() < 2) return;
        const float avail = totalW - g * ((float)fr.size() - 1.f);
        if (avail < 4.f) return;
        float x = x0;
        for (size_t i = 0; i + 1 < fr.size(); ++i) {
            x += avail * fr[i];
            LayoutDivider d;
            d.vertical = true;
            d.array = array;
            d.index = (int)i + 1;
            d.pos = x + g * 0.5f;
            d.lo = y0;
            d.hi = y1;
            out.push_back(d);
            x += g;
        }
    };
    auto horizontals = [&](int array, const std::vector<float>& fr, float y0, float x0, float x1, float totalH) {
        if (fr.size() < 2) return;
        const float avail = totalH - g * ((float)fr.size() - 1.f);
        if (avail < 4.f) return;
        float y = y0;
        for (size_t i = 0; i + 1 < fr.size(); ++i) {
            y += avail * fr[i];
            LayoutDivider d;
            d.vertical = false;
            d.array = array;
            d.index = (int)i + 1;
            d.pos = y + g * 0.5f;
            d.lo = x0;
            d.hi = x1;
            out.push_back(d);
            y += g;
        }
    };

    if (grp.type == 0) {
        verticals(0, grp.fr1, rc.left, rc.top, rc.bottom, W);
        horizontals(1, grp.fr2, rc.top, rc.left, rc.right, H);
    }
    else if (grp.type == 1) {
        const int a = std::max(0, grp.rows), b = std::max(0, grp.cols);
        if (a > 0 && b > 0) {
            const float availH = H - g;
            const float hTop = availH * grp.band;
            LayoutDivider d;
            d.vertical = false;
            d.array = -1;
            d.index = -1;
            d.pos = rc.top + hTop + g * 0.5f;
            d.lo = rc.left;
            d.hi = rc.right;
            out.push_back(d);
            verticals(0, grp.fr1, rc.left, rc.top, rc.top + hTop, W);
            verticals(1, grp.fr2, rc.left, rc.top + hTop + g, rc.bottom, W);
        }
        else if (a > 0) verticals(0, grp.fr1, rc.left, rc.top, rc.bottom, W);
        else if (b > 0) verticals(1, grp.fr2, rc.left, rc.top, rc.bottom, W);
    }
    else {
        const int a = std::max(0, grp.rows), b = std::max(0, grp.cols);
        if (a > 0 && b > 0) {
            const float availW = W - g;
            const float wLeft = availW * grp.band;
            LayoutDivider d;
            d.vertical = true;
            d.array = -1;
            d.index = -1;
            d.pos = rc.left + wLeft + g * 0.5f;
            d.lo = rc.top;
            d.hi = rc.bottom;
            out.push_back(d);
            horizontals(0, grp.fr1, rc.top, rc.left, rc.left + wLeft, H);
            horizontals(1, grp.fr2, rc.top, rc.left + wLeft + g, rc.right, H);
        }
        else if (a > 0) horizontals(0, grp.fr1, rc.top, rc.left, rc.right, H);
        else if (b > 0) horizontals(1, grp.fr2, rc.top, rc.left, rc.right, H);
    }
    return out;
}

bool ImageEditCollage::layoutDividerHit(POINT clientPos, int& group, int& dividerIndex) const
{
    group = -1;
    dividerIndex = -1;
    if (!hasSelection()) return false;
    const int gid = selectionGroup();
    if (!isLayoutGroup(gid)) return false;
    auto it = layoutGroups_.find(gid);
    if (it == layoutGroups_.end()) return false;
    const Pt wp = toWorld(clientPos);
    const float tol = 6.f / std::max(0.01f, viewScale());
    const auto divs = layoutDividers(it->second);
    for (int i = 0; i < (int)divs.size(); ++i) {
        const auto& d = divs[i];
        if (d.vertical) {
            if (std::fabs(wp.x - d.pos) <= tol && wp.y >= d.lo - tol && wp.y <= d.hi + tol) {
                group = gid; dividerIndex = i;
                return true;
            }
        }
        else {
            if (std::fabs(wp.y - d.pos) <= tol && wp.x >= d.lo - tol && wp.x <= d.hi + tol) {
                group = gid; dividerIndex = i;
                return true;
            }
        }
    }
    return false;
}

void ImageEditCollage::paintLayoutGroupChrome(ID2D1RenderTarget* rt, const LayoutGroupState& grp,
    float ox, float oy, float scale, bool withHandles)
{
    if (!rt) return;
    auto art = artRect();
    const float d = (win && win->dpi > 0.f) ? win->dpi : 1.f;
    const auto rc = grp.rect;
    const float l = ox + (rc.left - art.left) * scale;
    const float t = oy + (rc.top - art.top) * scale;
    const float r = ox + (rc.right - art.left) * scale;
    const float b = oy + (rc.bottom - art.top) * scale;

    // 内缝隙：默认灰色细线，悬停/拖动的那条主题绿加粗（表格式控件）
    if (auto gray = solidBrush(rt, 0x7A7A7AFF)) {
        if (auto green = solidBrush(rt, ImageEditTheme::accent)) {
            const auto divs = layoutDividers(grp);
            const int gid = selectionGroup();
            for (int i = 0; i < (int)divs.size(); ++i) {
                const auto& dv = divs[i];
                const bool hot = (draggingDivider_ && dragDividerGroup_ == gid && dragDividerIndex_ == i)
                    || (!draggingDivider_ && hoverDividerGroup_ == gid && hoverDividerIndex_ == i);
                auto* brush = hot ? green.Get() : gray.Get();
                const float th = hot ? 2.f * d : 1.f * d;
                const Pt p0 = dv.vertical ? Pt{ dv.pos, dv.lo } : Pt{ dv.lo, dv.pos };
                const Pt p1 = dv.vertical ? Pt{ dv.pos, dv.hi } : Pt{ dv.hi, dv.pos };
                rt->DrawLine(screenPointOf(p0), screenPointOf(p1), brush, th);
            }
        }
    }
    if (auto brush = solidBrush(rt, ImageEditTheme::accent)) rt->DrawRectangle(D2D1::RectF(l, t, r, b), brush.Get(), 1.f * d);
    if (withHandles) {
        paintHandleDot(rt, l, t, d);
        paintHandleDot(rt, r, t, d);
        paintHandleDot(rt, r, b, d);
        paintHandleDot(rt, l, b, d);
    }
}

// ---------------- 选中对象上方的快捷变换控件 --------------

bool ImageEditCollage::quickActionLayout(float& bgL, float& bgT, float& bgW, float& bgH,
    D2D1_RECT_F btnRects[4], bool& placeBelow) const
{
    if (!hasSelection() || exporting_) return false;
    const int gid = selectionGroup();
    const float s = viewScale();
    float ox = 0.f, oy = 0.f;
    viewOrigin(ox, oy);
    auto art = artRect();
    D2D1_RECT_F b = boundsOf(cells_[selected_].poly);
    if (imageAdjust_ && !cells_[selected_].image.empty()) {
        // 图片调整模式：快捷控件跟着图片外框走（取旋转后四角的包围盒），不再贴着布局
        auto f = imageFrameOf(cells_[selected_]);
        if (f.valid) {
            const float ca = std::cos(f.angleRad), sa = std::sin(f.angleRad);
            const float hw = f.w * 0.5f, hh = f.h * 0.5f;
            const float lxs[4]{ -hw, hw, hw, -hw };
            const float lys[4]{ -hh, -hh, hh, hh };
            D2D1_RECT_F ib{ FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX };
            for (int i = 0; i < 4; ++i) {
                const float wx = f.cx + lxs[i] * ca - lys[i] * sa;
                const float wy = f.cy + lxs[i] * sa + lys[i] * ca;
                ib.left = std::min(ib.left, wx);
                ib.top = std::min(ib.top, wy);
                ib.right = std::max(ib.right, wx);
                ib.bottom = std::max(ib.bottom, wy);
            }
            b = ib;
        }
    }
    else if (isLayoutGroup(gid)) {
        b = layoutGroupBounds(gid);
    }
    else if (gid >= 0) {
        // 分割组也显示快捷控件：按整个原容器的外框定位
        auto it = splitGroups_.find(gid);
        if (it != splitGroups_.end()) b = boundsOf(it->second.original);
    }
    const float l = ox + (b.left - art.left) * s;
    const float t = oy + (b.top - art.top) * s;
    const float r = ox + (b.right - art.left) * s;
    const float bo = oy + (b.bottom - art.top) * s;

    const float d = (win && win->dpi > 0.f) ? win->dpi : 1.f;
    const float bs = 28.f * d, gap = 2.f * d, pad = 4.f * d;
    bgW = bs * 4.f + gap * 3.f + pad * 2.f;
    bgH = bs + pad * 2.f;
    bgL = (l + r) * 0.5f - bgW * 0.5f;
    bgL = std::max(4.f * d, std::min(bgL, w - bgW - 4.f * d));
    placeBelow = false;
    bgT = t - bgH - 8.f * d;
    if (bgT < 4.f * d) {   // 上方放不下 → 放到选中框下方
        bgT = bo + 8.f * d;
        placeBelow = true;
        if (bgT + bgH > h - 4.f * d) bgT = std::max(4.f * d, t - bgH - 8.f * d);
    }
    for (int i = 0; i < 4; ++i) {
        const float bx = bgL + pad + i * (bs + gap);
        btnRects[i] = D2D1::RectF(bx, bgT + pad, bx + bs, bgT + pad + bs);
    }
    return true;
}

int ImageEditCollage::quickActionHit(POINT clientPos) const
{
    float bgL = 0, bgT = 0, bgW = 0, bgH = 0;
    D2D1_RECT_F btns[4]{};
    bool below = false;
    if (!quickActionLayout(bgL, bgT, bgW, bgH, btns, below)) return -1;
    const float lx = (float)clientPos.x - x;
    const float ly = (float)clientPos.y - y;
    for (int i = 0; i < 4; ++i) {
        if (lx >= btns[i].left && lx <= btns[i].right && ly >= btns[i].top && ly <= btns[i].bottom)
            return i;
    }
    return -1;
}

void ImageEditCollage::rotateSelectedContent(int dir)
{
    if (!hasSelection()) return;
    auto& c = cells_[selected_];
    if (!c.image.empty()) {
        c.rotation = std::fmod(c.rotation + (float)dir * 90.f, 360.f);
        if (c.rotation < 0.f) c.rotation += 360.f;
        return;
    }
    // 空容器：围绕中心换向（宽高互换）
    auto b = boundsOf(c.poly);
    const float cx = (b.left + b.right) * 0.5f;
    const float cy = (b.top + b.bottom) * 0.5f;
    for (auto& p : c.poly) {
        const float dx = p.x - cx, dy = p.y - cy;
        if (dir > 0) { p.x = cx - dy; p.y = cy + dx; }
        else { p.x = cx + dy; p.y = cy - dx; }
    }
}

void ImageEditCollage::mirrorSelectedContent(bool horizontal)
{
    if (!hasSelection()) return;
    auto& c = cells_[selected_];
    if (c.image.empty()) return;
    if (horizontal) {
        c.flipH = !c.flipH;
        c.imgOffX = -c.imgOffX;
    }
    else {
        c.flipV = !c.flipV;
        c.imgOffY = -c.imgOffY;
    }
}

void ImageEditCollage::rotateSelectedContainer(int dir)
{
    for (int idx : selectedCellIndices()) {
        if (idx < 0 || idx >= (int)cells_.size()) continue;
        auto& c = cells_[idx];
        auto b = boundsOf(c.poly);
        const float cx = (b.left + b.right) * 0.5f;
        const float cy = (b.top + b.bottom) * 0.5f;
        const float w = std::max(1.f, b.right - b.left);
        const float h = std::max(1.f, b.bottom - b.top);
        Pt imgCenter{ cx + c.imgOffX * w, cy + c.imgOffY * h };
        // 旋转前图片的绝对绘制尺寸；转完后按新容器反算归一化尺寸，
        // 这样容器和图片一起转 90°，图片不会被新宽高二次拉伸。
        // 原本是拉伸填充的图片，也固定成旋转前的实际尺寸，再跟着容器转。
        const float absW = c.stretch
            ? w
            : w * (c.imgNW > 0.f ? c.imgNW : 1.f) * c.imgScale;
        const float absH = c.stretch
            ? h
            : h * (c.imgNH > 0.f ? c.imgNH : 1.f) * c.imgScale;
        const bool wasStretch = c.stretch;
        for (auto& p : c.poly) {
            const float dx = p.x - cx, dy = p.y - cy;
            if (dir > 0) { p.x = cx - dy; p.y = cy + dx; }
            else { p.x = cx + dy; p.y = cy - dx; }
        }
        if (c.image.empty()) continue;
        c.rotation = std::fmod(c.rotation + (float)dir * 90.f, 360.f);
        if (c.rotation < 0.f) c.rotation += 360.f;
        if (c.flipH != c.flipV) std::swap(c.flipH, c.flipV);
        const auto nb = boundsOf(c.poly);
        const float nw = std::max(1.f, nb.right - nb.left);
        const float nh = std::max(1.f, nb.bottom - nb.top);
        if (wasStretch) c.stretch = false;
        if (!c.stretch && c.imgScale > 0.f) {
            c.imgNW = absW / (nw * c.imgScale);
            c.imgNH = absH / (nh * c.imgScale);
        }
        c.imgOffX = (imgCenter.x - (nb.left + nb.right) * 0.5f) / nw;
        c.imgOffY = (imgCenter.y - (nb.top + nb.bottom) * 0.5f) / nh;
    }
}

void ImageEditCollage::mirrorSelectedContainer(bool horizontal)
{
    for (int idx : selectedCellIndices()) {
        if (idx < 0 || idx >= (int)cells_.size()) continue;
        auto& c = cells_[idx];
        auto b = boundsOf(c.poly);
        const float cx = (b.left + b.right) * 0.5f;
        const float cy = (b.top + b.bottom) * 0.5f;
        for (auto& p : c.poly) {
            if (horizontal) p.x = cx - (p.x - cx);
            else p.y = cy - (p.y - cy);
        }
        if (c.image.empty()) continue;
        if (horizontal) {
            c.flipH = !c.flipH;
            c.imgOffX = -c.imgOffX;
        }
        else {
            c.flipV = !c.flipV;
            c.imgOffY = -c.imgOffY;
        }
    }
}

void ImageEditCollage::rotateLayoutGroup(int group, int dir)
{
    auto it = layoutGroups_.find(group);
    if (it == layoutGroups_.end()) return;
    auto& g = it->second;
    // 整组带着格内图片一起换向。
    for (int idx : g.members) {
        if (idx < 0 || idx >= (int)cells_.size()) continue;
        auto& c = cells_[idx];
        if (c.image.empty()) continue;
        c.rotation = std::fmod(c.rotation + (float)dir * 90.f, 360.f);
        if (c.rotation < 0.f) c.rotation += 360.f;
    }
    // 组外框宽高互换（中心不动）
    const float cx = (g.rect.left + g.rect.right) * 0.5f;
    const float cy = (g.rect.top + g.rect.bottom) * 0.5f;
    const float W = g.rect.right - g.rect.left;
    const float H = g.rect.bottom - g.rect.top;
    g.rect = D2D1::RectF(cx - H * 0.5f, cy - W * 0.5f, cx + H * 0.5f, cy + W * 0.5f);
    if (g.type == 0) {
        std::swap(g.rows, g.cols);
        std::swap(g.fr1, g.fr2);
    }
    else {
        g.type = (g.type == 1) ? 2 : 1;
        std::swap(g.rows, g.cols);
        g.band = 1.f - g.band;
        std::swap(g.fr1, g.fr2);
    }
    rebuildLayoutGroup(group);
}

void ImageEditCollage::mirrorLayoutGroup(int group, bool horizontal)
{
    auto it = layoutGroups_.find(group);
    if (it == layoutGroups_.end()) return;
    auto& g = it->second;

    for (int idx : g.members) {
        if (idx < 0 || idx >= (int)cells_.size()) continue;
        auto& c = cells_[idx];
        if (c.image.empty()) continue;
        if (horizontal) { c.flipH = !c.flipH; c.imgOffX = -c.imgOffX; }
        else { c.flipV = !c.flipV; c.imgOffY = -c.imgOffY; }
    }

    if (g.type == 0) {
        const int r = g.rows, c = g.cols;
        if (r <= 0 || c <= 0) { rebuildLayoutGroup(group); return; }
        if (horizontal) {
            std::reverse(g.fr1.begin(), g.fr1.end());
            for (int j = 0; j < r; ++j)
                std::reverse(g.members.begin() + j * c, g.members.begin() + (j + 1) * c);
        }
        else {
            std::reverse(g.fr2.begin(), g.fr2.end());
            std::reverse(g.members.begin(), g.members.end());
        }
    }
    else if (g.type == 1) {
        const int top = std::max(0, g.rows), bot = std::max(0, g.cols);
        if (horizontal) {
            std::reverse(g.fr1.begin(), g.fr1.end());
            std::reverse(g.fr2.begin(), g.fr2.end());
            if (top > 0) std::reverse(g.members.begin(), g.members.begin() + top);
            if (bot > 0) std::reverse(g.members.begin() + top, g.members.end());
        }
        else {
            // 上下两带互换
            std::swap(g.rows, g.cols);
            std::swap(g.fr1, g.fr2);
            g.band = 1.f - g.band;
            if (top > 0 && bot > 0)
                std::rotate(g.members.begin(), g.members.begin() + top, g.members.end());
        }
    }
    else {
        const int left = std::max(0, g.rows), right = std::max(0, g.cols);
        if (horizontal) {
            // 左右两带互换
            std::swap(g.rows, g.cols);
            std::swap(g.fr1, g.fr2);
            g.band = 1.f - g.band;
            if (left > 0 && right > 0)
                std::rotate(g.members.begin(), g.members.begin() + left, g.members.end());
        }
        else {
            std::reverse(g.fr1.begin(), g.fr1.end());
            std::reverse(g.fr2.begin(), g.fr2.end());
            if (left > 0) std::reverse(g.members.begin(), g.members.begin() + left);
            if (right > 0) std::reverse(g.members.begin() + left, g.members.end());
        }
    }
    rebuildLayoutGroup(group);
}

void ImageEditCollage::applyQuickAction(int action)
{
    if (!hasSelection()) return;
    const int gid = selectionGroup();
    // 图片调整模式下控件贴在图片上：操作对象是这张图片，而不是整个布局/分割组
    const bool adjustImg = imageAdjust_ && !cells_[selected_].image.empty();
    pushUndo();
    if (adjustImg) {
        if (action == 0) rotateSelectedContent(1);
        else if (action == 1) rotateSelectedContent(-1);
        else mirrorSelectedContent(action == 2);
    }
    else if (isLayoutGroup(gid)) {
        if (action == 0) rotateLayoutGroup(gid, 1);         // 向右旋转
        else if (action == 1) rotateLayoutGroup(gid, -1);   // 向左旋转
        else mirrorLayoutGroup(gid, action == 2);           // 2=水平镜像，3=垂直镜像
    }
    else if (gid >= 0) {
        transformSplitGroup(gid, action);                   // 分割组整组变换
    }
    else {
        if (action == 0) rotateSelectedContainer(1);
        else if (action == 1) rotateSelectedContainer(-1);
        else mirrorSelectedContainer(action == 2);
    }
    notifyChanged();
    if (win) win->refresh();
}

void ImageEditCollage::transformSplitGroup(int group, int action)
{
    auto it = splitGroups_.find(group);
    if (it == splitGroups_.end()) return;
    auto& grp = it->second;
    const auto ob = boundsOf(grp.original);
    const float cx = (ob.left + ob.right) * 0.5f;
    const float cy = (ob.top + ob.bottom) * 0.5f;
    auto mapPt = [&](Pt& q) {
        const float dx = q.x - cx, dy = q.y - cy;
        switch (action) {
        case 0: q.x = cx - dy; q.y = cy + dx; break;   // 右转 90
        case 1: q.x = cx + dy; q.y = cy - dx; break;   // 左转 90
        case 2: q.x = cx - dx; break;                  // 水平镜像
        default: q.y = cy - dy; break;                 // 垂直镜像
        }
    };

    const bool rotate90 = (action == 0 || action == 1);
    // 单个容器：几何点 + 块内图片的旋转/翻转/偏移一起变换
    auto transformCell = [&](Cell& c) {
        auto b = boundsOf(c.poly);
        const float w = std::max(1.f, b.right - b.left);
        const float h = std::max(1.f, b.bottom - b.top);
        Pt imgCenter{ (b.left + b.right) * 0.5f + c.imgOffX * w,
                      (b.top + b.bottom) * 0.5f + c.imgOffY * h };
        for (auto& q : c.poly) mapPt(q);
        if (c.image.empty()) return;
        mapPt(imgCenter);
        if (rotate90) {
            c.rotation = std::fmod(c.rotation + (action == 0 ? 90.f : -90.f), 360.f);
            if (c.rotation < 0.f) c.rotation += 360.f;
            // 90° 旋转会把横/竖翻转互换（两者同时存在时保持不变）
            if (c.flipH != c.flipV) std::swap(c.flipH, c.flipV);
            std::swap(c.imgNW, c.imgNH);   // 块宽高互换，图片比例同步互换
        }
        else {
            if (action == 2) c.flipH = !c.flipH;
            else c.flipV = !c.flipV;
            c.rotation = -c.rotation;
        }
        const auto nb = boundsOf(c.poly);
        const float nw = std::max(1.f, nb.right - nb.left);
        const float nh = std::max(1.f, nb.bottom - nb.top);
        c.imgOffX = (imgCenter.x - (nb.left + nb.right) * 0.5f) / nw;
        c.imgOffY = (imgCenter.y - (nb.top + nb.bottom) * 0.5f) / nh;
        };

    for (int idx = 0; idx < (int)cells_.size(); ++idx) {
        if (cells_[idx].group == group) transformCell(cells_[idx]);
    }
    // 切块基准（原容器）也一起变换：之后改间距/拖分割线触发重算时图片朝向不丢
    transformCell(grp.base);

    for (auto& L : grp.lines) { mapPt(L.a); mapPt(L.b); }
    for (auto& q : grp.original) mapPt(q);
}

void ImageEditCollage::paintQuickActions(ID2D1RenderTarget* rt)
{
    if (!rt || !win) return;
    float bgL = 0, bgT = 0, bgW = 0, bgH = 0;
    D2D1_RECT_F btns[4]{};
    bool below = false;
    if (!quickActionLayout(bgL, bgT, bgW, bgH, btns, below)) return;
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    const float d = (win->dpi > 0.f) ? win->dpi : 1.f;

    const D2D1_ROUNDED_RECT bg = D2D1::RoundedRect(
        D2D1::RectF(bgL, bgT, bgL + bgW, bgT + bgH), 10.f * d, 10.f * d);   // 与工具栏外框圆角一致
    if (auto brush = solidBrush(rt, 0x232323F2)) rt->FillRoundedRectangle(bg, brush.Get());

    for (int i = 0; i < 4; ++i) {
        if (quickHoverAction_ == i) {
            if (auto brush = solidBrush(rt, 0x3A3A3AFF)) {
                // 内层按钮与外框同心：圆角 = 外圆角 - 内边距（10 - 4）
                const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(btns[i], 6.f * d, 6.f * d);
                rt->FillRoundedRectangle(rr, brush.Get());
            }
        }
    }

    auto* d2d = D2D::get();
    if (!d2d) return;
    static const wchar_t* glyphs[4]{
        Icon::RotateRight, Icon::RotateLeft, Icon::MirrorHorizontal, Icon::MirrorVertical
    };
    static Microsoft::WRL::ComPtr<IDWriteTextLayout> layouts[4];
    static D2D* layoutOwner = nullptr;
    static float layoutDpi = 0.f;
    if (!layouts[0] || layoutOwner != d2d || layoutDpi != d) {
        layoutOwner = d2d;
        layoutDpi = d;
        // 必须用自定义字体集合的 TextFormat 建 layout：系统集合里没有 font_family，
        // SetFontFamilyName 换不到族（图标会退化成方块）。见 D2D::getTextFormat 的说明。
        auto* fmt = d2d->getTextFormat(L"font_family");
        for (int i = 0; i < 4; ++i) {
            layouts[i].Reset();
            if (!fmt || !d2d->dwriteFactory) continue;
            const std::wstring glyph{ glyphs[i] };
            d2d->dwriteFactory->CreateTextLayout(glyph.c_str(), (UINT32)glyph.size(), fmt,
                28.f * d, 28.f * d, layouts[i].GetAddressOf());
            if (layouts[i]) {
                layouts[i]->SetFontSize(16.f * d, { 0, INT_MAX });
                layouts[i]->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                layouts[i]->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
        }
    }
    if (auto brush = solidBrush(rt, 0xFAFAFAFF)) {
        for (int i = 0; i < 4; ++i) {
            if (!layouts[i]) continue;
            rt->DrawTextLayout(D2D1::Point2F(btns[i].left, btns[i].top), layouts[i].Get(),
                brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        }
    }
}

// 分割线的命中：端点优先，其次线身
bool ImageEditCollage::splitLineHit(POINT clientPos, int& group, int& line, int& endpoint) const
{
    group = selectionGroup();
    line = endpoint = -1;
    auto it = splitGroups_.find(group);
    if (group < 0 || it == splitGroups_.end()) return false;
    const Pt wp = toWorld(clientPos);
    const float vs = std::max(0.01f, viewScale());
    const float dotTol = 10.f / vs;
    const float bodyTol = 6.f / vs;
    for (int i = 0; i < (int)it->second.lines.size(); ++i) {
        const auto& L = it->second.lines[i];
        if (std::hypot(wp.x - L.a.x, wp.y - L.a.y) <= dotTol) { line = i; endpoint = 0; return true; }
        if (std::hypot(wp.x - L.b.x, wp.y - L.b.y) <= dotTol) { line = i; endpoint = 1; return true; }
        const float vx = L.b.x - L.a.x, vy = L.b.y - L.a.y;
        const float len2 = vx * vx + vy * vy;
        if (len2 < 1.f) continue;
        float t = ((wp.x - L.a.x) * vx + (wp.y - L.a.y) * vy) / len2;
        t = std::max(0.f, std::min(1.f, t));
        const float qx = L.a.x + vx * t, qy = L.a.y + vy * t;
        if (std::hypot(wp.x - qx, wp.y - qy) <= bodyTol) { line = i; endpoint = -1; return true; }
    }
    return false;
}

// 选中分割组时显示分割线：线画满原容器（只用于显示），两个可拖圆点就是「两点式」的两端
void ImageEditCollage::paintSplitLines(ID2D1RenderTarget* rt, float, float, float)
{
    const int g = selectionGroup();
    if (!rt || g < 0) return;
    auto it = splitGroups_.find(g);
    if (it == splitGroups_.end()) return;
    const float d = (win && win->dpi > 0.f) ? win->dpi : 1.f;
    auto lineBrush = solidBrush(rt, ImageEditTheme::accent);
    const auto bb = boundsOf(it->second.original);
    if (lineBrush) {
        for (const auto& L : it->second.lines) {
            const float dx = L.b.x - L.a.x, dy = L.b.y - L.a.y;
            if (std::fabs(dx) < 1e-3f && std::fabs(dy) < 1e-3f) continue;
            float t0 = -FLT_MAX, t1 = FLT_MAX;
            auto clampT = [&](float p0, float dir, float lo, float hi) {
                if (std::fabs(dir) < 1e-4f) return;
                float ta = (lo - p0) / dir, tb = (hi - p0) / dir;
                if (ta > tb) std::swap(ta, tb);
                t0 = std::max(t0, ta);
                t1 = std::min(t1, tb);
            };
            clampT(L.a.x, dx, bb.left, bb.right);
            clampT(L.a.y, dy, bb.top, bb.bottom);
            if (t0 >= t1) continue;
            const Pt p0{ L.a.x + dx * t0, L.a.y + dy * t0 };
            const Pt p1{ L.a.x + dx * t1, L.a.y + dy * t1 };
            rt->DrawLine(screenPointOf(p0), screenPointOf(p1), lineBrush.Get(), 1.f * d);
        }
    }
    for (const auto& L : it->second.lines) {
        auto pa = screenPointOf(L.a);
        auto pb = screenPointOf(L.b);
        paintHandleDot(rt, pa.x, pa.y, d);
        paintHandleDot(rt, pb.x, pb.y, d);
    }
}

// ---------------- 图片 ----------------

bool ImageEditCollage::imageSizeOf(const std::wstring& path, int& imgW, int& imgH) const
{
    imgW = imgH = 0;
    if (path.empty()) return false;
    // 尺寸缓存：拖动/缩放时每帧都会走这里，重开解码器会明显卡顿
    auto cached = imageSizeCache_.find(path);
    if (cached != imageSizeCache_.end()) {
        imgW = cached->second.first;
        imgH = cached->second.second;
        return imgW > 0 && imgH > 0;
    }
    auto wic = wicFactory();
    if (!wic) return false;
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, &decoder))) return false;
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return false;
    UINT ww = 0, hh = 0;
    frame->GetSize(&ww, &hh);
    imgW = (int)ww;
    imgH = (int)hh;
    if (imgW > 0 && imgH > 0) imageSizeCache_[path] = { imgW, imgH };
    return imgW > 0 && imgH > 0;
}

// 图片在容器内的绘制尺寸按比例存：容器放大/缩小，图片整体跟着缩放，构图不变
void ImageEditCollage::normalizeCellImage(Cell& c) const
{
    c.imgNW = c.imgNH = 1.f;
    if (c.image.empty()) return;
    auto b = boundsOf(c.poly);
    const float tw = std::max(1.f, b.right - b.left);
    const float th = std::max(1.f, b.bottom - b.top);
    if (c.stretch) return;   // Shift 拖入 = 直接铺满容器
    int iw = 0, ih = 0;
    if (!imageSizeOf(c.image, iw, ih)) return;
    const float effW = (float)iw, effH = (float)ih;
    // 默认按原始比例「铺满」容器（短边留白裁掉）
    const float cover = std::max(tw / effW, th / effH);
    c.imgNW = (effW * cover) / tw;
    c.imgNH = (effH * cover) / th;
}

void ImageEditCollage::setCellImage(int index, const std::wstring& path, bool stretch)
{
    if (index < 0 || index >= (int)cells_.size() || path.empty()) return;
    pushUndo();
    cells_[index].image = path;
    cells_[index].stretch = stretch;
    cells_[index].imgScale = 1.f;
    cells_[index].imgOffX = cells_[index].imgOffY = 0.f;
    normalizeCellImage(cells_[index]);
    selectCellGroup(index);   // 组内任意一格 = 选中整组
    notifyChanged();
    if (win) win->refresh();
}

void ImageEditCollage::addImageBlock(const std::wstring& path, const Pt& at, bool useNaturalSize)
{
    int iw = 0, ih = 0;
    if (useNaturalSize) imageSizeOf(path, iw, ih);
    if (iw <= 0 || ih <= 0) { iw = 240; ih = 180; }

    Cell cell;
    cell.image = path;
    cell.poly = rectPoly(at.x, at.y, at.x + iw, at.y + ih);
    normalizeCellImage(cell);
    cells_.push_back(cell);
    selected_ = (int)cells_.size() - 1;
}

void ImageEditCollage::pasteCells()
{
    if (cellClipboard_.empty()) return;
    pushUndo();
    ++pasteCount_;
    const float off = 24.f * (float)pasteCount_;
    const int start = (int)cells_.size();
    for (auto c : cellClipboard_) {
        translateCell(c, off, off);
        cells_.push_back(c);
    }
    selected_ = start;
    extraCells_.clear();
    for (int i = start + 1; i < (int)cells_.size(); ++i) extraCells_.push_back(i);

    // 粘贴结果落在画板外时整体挪回画板内容区
    const auto art = artRect();
    const float b = border_;
    D2D1_RECT_F pb{ FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (int i = start; i < (int)cells_.size(); ++i) {
        auto bb = boundsOf(cells_[i].poly);
        pb.left = std::min(pb.left, bb.left);
        pb.top = std::min(pb.top, bb.top);
        pb.right = std::max(pb.right, bb.right);
        pb.bottom = std::max(pb.bottom, bb.bottom);
    }
    float shx = 0.f, shy = 0.f;
    if (pb.left < art.left + b) shx = art.left + b - pb.left;
    if (pb.top < art.top + b) shy = art.top + b - pb.top;
    if (shx > 0.f || shy > 0.f)
        for (int i = start; i < (int)cells_.size(); ++i) translateCell(cells_[i], shx, shy);

    notifyChanged();
    if (win) win->refresh();
}

void ImageEditCollage::handleDrop(const std::vector<std::wstring>& paths, POINT clientPos)
{
    std::vector<std::wstring> imgs;
    for (const auto& p : paths)
        if (hasImageExt(p)) imgs.push_back(p);
    if (imgs.empty()) return;

    Pt wp;
    int index = -1;
    if (toWorldIfInside(clientPos, wp)) index = cellAt(wp);
    if (index >= 0) {
        // 拖到某个容器上：直接替换该容器的图片
        pushUndo();
        cells_[index].image = imgs.front();
        cells_[index].stretch = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        cells_[index].imgScale = 1.f;
        cells_[index].imgOffX = cells_[index].imgOffY = 0.f;
        normalizeCellImage(cells_[index]);
        selectCellGroup(index);   // 组内任意一格 = 选中整组
        notifyChanged();
        if (win) win->refresh();
        return;
    }

    if (cells_.empty()) {
        pushUndo();
        // 第一张图：已有画板（手动尺寸）就铺满画板内容区（等价 1×1 布局）；
        // 没有画板就按原图尺寸创建画板 + 容器
        const auto art = artRect();
        const float b = border_;
        const float l = art.left + b, t = art.top + b;
        const float r = art.right - b, bo = art.bottom - b;
        if (r - l > 8.f && bo - t > 8.f) {
            Cell cell;
            cell.image = imgs.front();
            cell.poly = rectPoly(l, t, r, bo);
            normalizeCellImage(cell);
            cells_.push_back(cell);
            selected_ = 0;
        }
        else {
            addImageBlock(imgs.front(), { 0.f, 0.f }, true);
        }
        // 拖入首图之后固定画板尺寸：之后只有容器变大才会把画板撑大，容器缩小不会缩回
        const auto fixed = artRect();
        manualL_ = (int)std::lround(fixed.left);
        manualT_ = (int)std::lround(fixed.top);
        manualW_ = (int)std::lround(fixed.right - fixed.left);
        manualH_ = (int)std::lround(fixed.bottom - fixed.top);
        captureContentExtents();
        fitToView();
    } else {
        // 后续图片：按原图尺寸放在落点，但必须落在画板内（画板已固定，不会被图片撑出去）
        if (!toWorldIfInside(clientPos, wp)) {
            // 丢在画布之外（工具栏/素材条等）：忽略，避免凭空在原点复制一个容器
            if (win) win->refresh();
            return;
        }
        pushUndo();
        const int startNew = (int)cells_.size();
        for (const auto& p : imgs) {
            int iw = 0, ih = 0;
            if (!imageSizeOf(p, iw, ih)) { iw = 240; ih = 180; }
            const auto art = artRect();
            const float b = border_;
            const float minX = art.left + b, minY = art.top + b;
            const float maxX = std::max(minX, art.right - b - (float)iw);
            const float maxY = std::max(minY, art.bottom - b - (float)ih);
            const Pt at{ std::min(std::max(wp.x, minX), maxX),
                         std::min(std::max(wp.y, minY), maxY) };
            addImageBlock(p, at, true);
        }
        for (int i = startNew; i < (int)cells_.size(); ++i)
            snapCellToNeighbors(i);
    }
    // 拖入图片后取消选中：尺寸行回到「画板尺寸」
    selected_ = -1;
    extraCells_.clear();
    notifyChanged();
    if (win) win->refresh();
}

std::wstring ImageEditCollage::pickImageFile()
{
    if (!win) return {};
    return win->openFileDialog(imageFilters());
}

void ImageEditCollage::pickImageForSelected()
{
    if (!hasSelection()) return;
    auto path = pickImageFile();
    if (!path.empty()) {
        bool stretch = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        setCellImage(selected_, path, stretch);
    }
}

void ImageEditCollage::replaceCellImage(int index)
{
    if (index < 0 || index >= (int)cells_.size()) return;
    auto path = pickImageFile();
    if (!path.empty()) setCellImage(index, path, false);
}

// ---------------- 素材条 ----------------

std::vector<ImageEditCollage::Thumb> ImageEditCollage::thumbnails() const
{
    std::vector<Thumb> out;
    for (int i = 0; i < (int)cells_.size(); ++i) {
        if (cells_[i].image.empty()) continue;
        out.push_back({ cells_[i].image, selected_ == i, i });
    }
    return out;
}

void ImageEditCollage::selectCell(int index)
{
    if (index < 0 || index >= (int)cells_.size()) return;
    imageAdjust_ = false;
    selectCellGroup(index);
    notifyChanged(false);
    if (win) win->refresh();
}

// ---------------- 鼠标 ----------------

void ImageEditCollage::onMouseDown(POINT pos, bool right)
{
    if (panning_) return;
    if (uiHitTest && uiHitTest(pos)) {
        if (dragging_ || resizing_) { dragging_ = resizing_ = false; if (win) win->refresh(); }
        return;
    }
    if (!isPosIn(pos)) return;

    // 快捷变换按钮优先（左键、任何工具状态下都生效）
    if (!right && !spaceHeld_) {
        const int act = quickActionHit(pos);
        if (act >= 0) {
            applyQuickAction(act);
            return;
        }
    }

    // 空格 + 左键 = 平移
    if (spaceHeld_) {
        panning_ = true;
        panStart_ = pos;
        panStartDX_ = panDX_;
        panStartDY_ = panDY_;
        SetCapture(win->hwnd);
        return;
    }

    // 标注工具：鼠标交给标注宿主（含右键：退出文字编辑/清选中）
    if (annotating()) {
        if (annot_) annot_->forwardDown(pos, right);
        return;
    }

    // 右键逐层退出：分割工具下先删掉最近的分割线
    if (right) {
        if (tool_ == Tool::Split) {
            const Pt wp = toWorld(pos);
            int hit = cellAt(wp);
            if (hit < 0) hit = selected_;
            if (hit >= 0 && hit < (int)cells_.size() && cells_[hit].group >= 0) {
                int li = -1;
                const float tol = 12.f / std::max(0.01f, viewScale());
                if (nearestSplitLine(cells_[hit].group, wp, tol, li)) {
                    deleteSplitLine(cells_[hit].group, li);
                    return;
                }
            }
        }
        if (imageAdjust_) {
            imageAdjust_ = false;
        } else if (hasSelection() || !extraCells_.empty()) {
            clearCellSelection();
        } else if (annot_ && annot_->hasAnyAnnotSelection()) {
            annot_->clearAnnotSelectionPublic();
        } else if (onRequestExitTool) {
            onRequestExitTool();   // 退到最后一层：收起当前工具与属性栏
        }
        notifyChanged(false);
        if (win) win->refresh();
        return;
    }

    Pt p = toWorld(pos);
    bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

    // 图片调整模式的图片控件不限制当前工具：布局/分割工具下双击进入后也能直接旋转/缩放图片
    if (imageAdjust_ && hasSelection() && !cells_[selected_].image.empty()) {
        Handle ih = Handle::None;
        int ci = -1;
        const auto imgHit = imageHitAt(pos, ih, ci);
        auto& c = cells_[selected_];
        auto f = imageFrameOf(c);
        if (f.valid && imgHit == ImgHit::Rotate) {
            pushUndo();
            rotatingImg_ = true;
            dragStart_ = dragCur_ = p;
            Pt center{ f.cx, f.cy };
            rotStartAngle_ = std::atan2(p.y - center.y, p.x - center.x);
            rotStartDeg_ = c.rotation;
            dragShift_ = shift;
            if (win) win->refresh();
            return;
        }
        if (f.valid && (imgHit == ImgHit::Corner || imgHit == ImgHit::Edge)) {
            pushUndo();
            imgResizing_ = true;
            imgHandle_ = ih;
            imgStartScale_ = c.imgScale;
            imgStartNW_ = (c.imgNW > 0.f) ? c.imgNW : 1.f;
            imgStartNH_ = (c.imgNH > 0.f) ? c.imgNH : 1.f;
            const float ca = std::cos(f.angleRad), sa = std::sin(f.angleRad);
            const float dx = p.x - f.cx, dy = p.y - f.cy;
            imgStartLocal_ = D2D1::Point2F(dx * ca + dy * sa, -dx * sa + dy * ca);
            if (win) win->refresh();
            return;
        }
    }

    if (tool_ == Tool::None) {
        // 选中分割组时：先命中分割线（端点优先，其次整条线）
        {
            int sg = -1, sl = -1, se = -1;
            if (splitLineHit(pos, sg, sl, se)) {
                pushUndo();
                adjustingSplit_ = true;
                dragging_ = true;
                dragGroup_ = sg;
                dragLineIndex_ = sl;
                splitEnd_ = se;
                dragStart_ = dragCur_ = p;
                if (win) win->refresh();
                return;
            }
        }

        // 布局组：按下内部缝隙 = 拖这条缝（表格式调相邻格的高/宽）
        {
            int dg = -1, di = -1;
            if (layoutDividerHit(pos, dg, di)) {
                auto it = layoutGroups_.find(dg);
                if (it != layoutGroups_.end()) {
                    pushUndo();
                    draggingDivider_ = true;
                    dragDividerGroup_ = dg;
                    dragDividerIndex_ = di;
                    dragDividerStart_ = it->second;
                    dragDividerStartPos_ = p;
                    hoverDividerGroup_ = dg;
                    hoverDividerIndex_ = di;
                    if (win) win->refresh();
                    return;
                }
            }
        }

        // 先判选中容器的缩放控点（图片调整模式下控件只在图片上，容器控点不响应）
        const bool adjustingImg = imageAdjust_ && hasSelection() && !cells_[selected_].image.empty();
        auto handle = adjustingImg ? Handle::None : handleAt(pos);
        if (handle != Handle::None) {
            resizeHandle_ = handle;
            resizing_ = true;
            auto b = boundsOf(cells_[selected_].poly);
            switch (handle) {
            case Handle::TL: resizePivot_ = { b.right, b.bottom }; break;
            case Handle::T:  resizePivot_ = { b.left, b.bottom }; break;
            case Handle::TR: resizePivot_ = { b.left, b.bottom }; break;
            case Handle::R:  resizePivot_ = { b.left, b.top }; break;
            case Handle::BR: resizePivot_ = { b.left, b.top }; break;
            case Handle::B:  resizePivot_ = { b.left, b.top }; break;
            case Handle::BL: resizePivot_ = { b.right, b.top }; break;
            case Handle::L:  resizePivot_ = { b.right, b.top }; break;
            default: break;
            }
            pushUndo();
            dragStart_ = p;
            dragShift_ = shift;
            return;
        }

        int hit = cellAt(p);
        auto now = GetTickCount();
        if (hit >= 0) {
            bool dbl = lastClickCell_ == hit && (now - lastClickTick_) < GetDoubleClickTime()
                && std::abs((int)(pos.x - lastClickPos_.x)) < 4
                && std::abs((int)(pos.y - lastClickPos_.y)) < 4;
            // Shift 点选：追加 / 取消追加（分割块按整组参与）
            if (shift && !dbl) {
                if (cells_[hit].group >= 0) {
                    // 分割组：点一下 = 选中整组；再点 = 整组取消
                    if (isCellSelected(hit)) {
                        clearCellSelection();
                    } else {
                        selectCellGroup(hit);
                    }
                    notifyChanged(false);
                    if (win) win->refresh();
                    return;
                }
                if (isCellSelected(hit)) {
                    if (selected_ == hit) {
                        if (extraCells_.empty()) {
                            selected_ = -1;
                        } else {
                            selected_ = extraCells_.front();
                            extraCells_.erase(extraCells_.begin());
                        }
                    } else {
                        extraCells_.erase(std::remove(extraCells_.begin(), extraCells_.end(), hit),
                            extraCells_.end());
                    }
                } else {
                    if (!hasSelection()) selected_ = hit;
                    else extraCells_.push_back(hit);
                }
                imageAdjust_ = false;
                notifyChanged(false);
                if (win) win->refresh();
                return;
            }
            const bool keepAdjust = imageAdjust_ && selected_ == hit && cells_[hit].group < 0;
            // 按在「已经选中的容器」上：保留当前多选，直接整体拖动（框选后一起移动）
            const bool wasSelected = isCellSelected(hit);
            if (!wasSelected) selectCellGroup(hit);
            if (keepAdjust) imageAdjust_ = true;
            if (dbl) {
                lastClickCell_ = -1;
                imgDrag_ = false;
                if (cells_[hit].image.empty()) {
                    // 空格子双击选图：分割块/布局块也只填被双击的这一块
                    if (selected_ != hit) selectCellGroup(hit);
                    pickImageForSelected();
                } else if (layoutGroupOf(hit) < 0) {
                    // 双击进入「容器内图片缩放」模式；分割块也可单独调整，
                    // 布局块仍按整组选中，不进单块图片调整
                    if (selected_ != hit) selectCellGroup(hit);
                    imageAdjust_ = true;
                }
            } else if (imageAdjust_ && hit == selected_) {
                // 图片调整模式：拖动平移容器内的图片
                imgDrag_ = true;
                dragging_ = true;
                dragStart_ = dragCur_ = p;
                dragAccum_ = {};
                imgOffStartX_ = cells_[selected_].imgOffX;
                imgOffStartY_ = cells_[selected_].imgOffY;
                pushUndo();
            } else {
                lastClickCell_ = hit;
                lastClickTick_ = now;
                lastClickPos_ = pos;
                imageAdjust_ = false;
                imgDrag_ = false;
                dragging_ = true;
                dragStart_ = dragCur_ = p;
                dragShift_ = shift;
                pushUndo();
                // Alt + 拖拽 = 复制一份拖走（原容器留在原地，和设计软件一致）
                if (GetKeyState(VK_MENU) & 0x8000) {
                    auto sel = selectedCellIndices();
                    std::vector<int> copies;
                    for (int idx : sel) {
                        if (idx < 0 || idx >= (int)cells_.size()) continue;
                        Cell c = cells_[idx];
                        c.group = -1;   // 复制体独立，不继承分割组
                        cells_.push_back(c);
                        copies.push_back((int)cells_.size() - 1);
                    }
                    if (!copies.empty()) {
                        selected_ = copies.front();
                        extraCells_.assign(copies.begin() + 1, copies.end());
                    }
                }
                // 记录按下时的原始几何：此后按「累计位移 + 吸附」重设位置，
                // 避免每帧增量吸附把容器钉在参考线上（表现为拖不动）
                dragAccum_ = {};
                dragOrigPolys_.clear();
                dragOrigPolys_.reserve(cells_.size());
                for (const auto& cc : cells_) dragOrigPolys_.push_back(cc.poly);
                dragOrigSplitLines_.clear();
                dragOrigSplitOriginal_.clear();
                for (const auto& [gid, sg] : splitGroups_) {
                    dragOrigSplitLines_[gid] = sg.lines;
                    dragOrigSplitOriginal_[gid] = sg.original;
                }
            }
            notifyChanged(false);
        } else {
            // 空工具下也能单击选中标注：命中就交给标注宿主（之后拖动/抬手也转发）
            if (annot_ && annot_->hitAnnotAtClient(pos)) {
                annotDragging_ = true;
                annot_->forwardDown(pos, false);
                hoverDividerGroup_ = hoverDividerIndex_ = -1;
                notifyChanged(false);
                if (win) win->refresh();
                return;
            }
            // 空白处拖框：橡皮筋多选（容器 + 标注）
            if (!shift) {
                clearCellSelection();
                if (annot_) annot_->clearAnnotSelectionPublic();
            }
            marqueeSelecting_ = true;
            marqueeAdditive_ = shift;
            marqueeStart_ = marqueeCur_ = p;
            notifyChanged(false);
        }
        if (win) win->refresh();
        return;
    }

    if (tool_ == Tool::Split) {
        // 双击不受工具限制：空格子选图 / 有图进入图片调整，和空工具一致
        const int hit = cellAt(p);
        if (hit >= 0) {
            auto now = GetTickCount();
            const bool dbl = lastClickCell_ == hit && (now - lastClickTick_) < GetDoubleClickTime()
                && std::abs((int)(pos.x - lastClickPos_.x)) < 4
                && std::abs((int)(pos.y - lastClickPos_.y)) < 4;
            if (dbl) {
                lastClickCell_ = -1;
                imgDrag_ = false;
                if (selected_ != hit) selectCellGroup(hit);
                // 分割工具下双击只为「选图」（空格子，含分割块），有图的容器仍保持画分割线
                if (cells_[hit].image.empty()) pickImageForSelected();
                notifyChanged(false);
                if (win) win->refresh();
                return;
            }
            lastClickCell_ = hit;
            lastClickTick_ = now;
            lastClickPos_ = pos;
            // 已在图片调整模式：按住图片是平移图片，不画分割线
            if (imageAdjust_ && hit == selected_ && !cells_[hit].image.empty()) {
                imgDrag_ = true;
                dragging_ = true;
                dragStart_ = dragCur_ = p;
                dragAccum_ = {};
                imgOffStartX_ = cells_[selected_].imgOffX;
                imgOffStartY_ = cells_[selected_].imgOffY;
                pushUndo();
                if (win) win->refresh();
                return;
            }
        }
        // 和线段工具一致：在画布上按住画一条线，松手后由这条线与容器边缘的两个交点决定分割。
        // 不从「按下点 + 角度」发射无限长直线，也就不再需要按下时先选中容器
        dragging_ = true;
        dragCur_ = dragStart_ = p;
        dragShift_ = shift;
        dragLineIndex_ = -1;
        adjustingSplit_ = false;
        notifyChanged(false);
        if (win) win->refresh();
        return;
    }

    if (tool_ == Tool::Layout) {
        // 和矩形等绘制工具同一套交互：落笔先命中已有对象 → 选中并拖动；
        // 只有从空白处起拖才创建新布局（不能在已有布局/容器上再画一个布局）
        const int hit = cellAt(p);
        if (hit >= 0) {
            drawingNewLayout_ = false;
            if (!isCellSelected(hit)) selectCellGroup(hit);
            auto now = GetTickCount();
            const bool dbl = lastClickCell_ == hit && (now - lastClickTick_) < GetDoubleClickTime()
                && std::abs((int)(pos.x - lastClickPos_.x)) < 4
                && std::abs((int)(pos.y - lastClickPos_.y)) < 4;
            // 双击选图 / 进入图片调整：与空工具一致，不受当前工具限制
            if (dbl) {
                lastClickCell_ = -1;
                imgDrag_ = false;
                if (cells_[hit].image.empty()) {
                    if (selected_ != hit) selectCellGroup(hit);
                    pickImageForSelected();   // 空格子只填被双击的这一块
                } else if (layoutGroupOf(hit) < 0) {
                    if (selected_ != hit) selectCellGroup(hit);
                    imageAdjust_ = true;
                }
                notifyChanged(false);
                if (win) win->refresh();
                return;
            }
            // 图片调整模式：拖动平移容器内的图片
            if (imageAdjust_ && hit == selected_) {
                imgDrag_ = true;
                dragging_ = true;
                dragStart_ = dragCur_ = p;
                dragAccum_ = {};
                imgOffStartX_ = cells_[selected_].imgOffX;
                imgOffStartY_ = cells_[selected_].imgOffY;
                pushUndo();
                if (win) win->refresh();
                return;
            }
            lastClickCell_ = hit;
            lastClickTick_ = now;
            lastClickPos_ = pos;
            imageAdjust_ = false;
            imgDrag_ = false;
            dragging_ = true;
            dragStart_ = dragCur_ = p;
            dragShift_ = shift;
            pushUndo();
            dragAccum_ = {};
            dragOrigPolys_.clear();
            dragOrigPolys_.reserve(cells_.size());
            for (const auto& cc : cells_) dragOrigPolys_.push_back(cc.poly);
            dragOrigSplitLines_.clear();
            dragOrigSplitOriginal_.clear();
            for (const auto& [gid, sg] : splitGroups_) {
                dragOrigSplitLines_[gid] = sg.lines;
                dragOrigSplitOriginal_[gid] = sg.original;
            }
            notifyChanged(false);
            if (win) win->refresh();
            return;
        }
        drawingNewLayout_ = true;
        dragging_ = true;
        adjustingSplit_ = false;
        dragStart_ = dragCur_ = p;
        dragShift_ = shift;
        if (win) win->refresh();
    }
}

void ImageEditCollage::onMouseMove(POINT pos)
{
    if (panning_) {
        panDX_ = panStartDX_ + (pos.x - panStart_.x);
        panDY_ = panStartDY_ + (pos.y - panStart_.y);
        if (win) win->refresh();
        return;
    }
    if (uiHitTest && uiHitTest(pos)) {
        if (dragging_ || resizing_) {
            dragging_ = resizing_ = false;
            if (win) win->refresh();
        }
        return;
    }
    if (!isPosIn(pos)) return;
    // 容器悬停提示：不限制当前工具（标注工具激活时也显示，和标注的悬停反馈保持一致）
    {
        const bool idle = !dragging_ && !resizing_ && !draggingDivider_ && !panning_
            && !marqueeSelecting_ && !imageAdjust_ && !rotatingImg_ && !imgResizing_;
        const int hit = idle ? cellAt(toWorld(pos)) : -1;
        if (hit != hoverCell_) {
            hoverCell_ = hit;
            if (win) win->refresh();
        }
        const int qa = idle ? quickActionHit(pos) : -1;
        if (qa != quickHoverAction_) {
            quickHoverAction_ = qa;
            if (win) win->refresh();
        }
    }
    if (annotating()) {
        if (annot_) annot_->forwardMove(pos);
        return;
    }
    // 空工具下拖动/点选标注：移动转发给标注宿主
    if (annotDragging_) {
        if (annot_) annot_->forwardMove(pos);
        return;
    }
    Pt p = toWorld(pos);
    dragCur_ = p;

    // 布局组缝隙拖动：改相邻两格占比（表格式），整组外框不变
    if (draggingDivider_ && dragDividerGroup_ >= 0) {
        auto it = layoutGroups_.find(dragDividerGroup_);
        if (it != layoutGroups_.end()) {
            auto& cur = it->second;
            const auto& start = dragDividerStart_;
            const auto divs = layoutDividers(start);
            if (dragDividerIndex_ >= 0 && dragDividerIndex_ < (int)divs.size()) {
                const auto d = divs[dragDividerIndex_];
                const float W = start.rect.right - start.rect.left;
                const float H = start.rect.bottom - start.rect.top;
                const float delta = d.vertical ? (p.x - dragDividerStartPos_.x)
                    : (p.y - dragDividerStartPos_.y);
                if (d.array < 0) {
                    // 两带之间的缝：改带比例
                    const float avail = (d.vertical ? W : H) - gap_;
                    if (avail > 1.f)
                        cur.band = std::clamp(start.band + delta / avail, 0.1f, 0.9f);
                }
                else {
                    const auto& fr = (d.array == 0) ? start.fr1 : start.fr2;
                    const int n = (int)fr.size();
                    const int idx = d.index;
                    if (idx > 0 && idx < n) {
                        const float avail = (d.vertical ? W : H) - gap_ * (float)(n - 1);
                        if (avail > 1.f) {
                            const float minFrac = std::min(0.5f, 24.f / avail);   // 每格至少 24px
                            const float sum = fr[idx - 1] + fr[idx];
                            const float f0 = std::clamp(fr[idx - 1] + delta / avail,
                                minFrac, sum - minFrac);
                            std::vector<float> nf = fr;
                            nf[idx - 1] = f0;
                            nf[idx] = sum - f0;
                            if (d.array == 0) cur.fr1 = nf;
                            else cur.fr2 = nf;
                        }
                    }
                }
            }
            rebuildLayoutGroup(dragDividerGroup_);
            notifyChanged(false);
            if (win) win->refresh();
        }
        return;
    }

    // 分割线：拖端点改变方向/位置，拖线身整体平移
    if (tool_ == Tool::None && adjustingSplit_ && dragging_ && dragGroup_ >= 0) {
        auto it = splitGroups_.find(dragGroup_);
        if (it != splitGroups_.end() && dragLineIndex_ >= 0
            && dragLineIndex_ < (int)it->second.lines.size()) {
            auto& L = it->second.lines[dragLineIndex_];
            // 分割控点只能在原容器边缘上滑动：按所在边做约束，不能拖到边缘之外
            const auto bb = boundsOf(it->second.original);
            auto edgeOf = [&bb](const Pt& q) {
                const float d[4]{ std::fabs(q.y - bb.top), std::fabs(q.y - bb.bottom),
                                  std::fabs(q.x - bb.left), std::fabs(q.x - bb.right) };
                int e = 0;
                for (int i = 1; i < 4; ++i) if (d[i] < d[e]) e = i;
                return e;
            };
            auto onEdge = [&bb](int e, const Pt& q) {
                switch (e) {
                case 0: return Pt{ std::clamp(q.x, bb.left, bb.right), bb.top };
                case 1: return Pt{ std::clamp(q.x, bb.left, bb.right), bb.bottom };
                case 2: return Pt{ bb.left, std::clamp(q.y, bb.top, bb.bottom) };
                default: return Pt{ bb.right, std::clamp(q.y, bb.top, bb.bottom) };
                }
            };
            if (splitEnd_ == 0) L.a = onEdge(edgeOf(L.a), p);
            else if (splitEnd_ == 1) L.b = onEdge(edgeOf(L.b), p);
            else {
                const int ea = edgeOf(L.a), eb = edgeOf(L.b);
                const float dx = p.x - dragStart_.x, dy = p.y - dragStart_.y;
                L.a = onEdge(ea, { L.a.x + dx, L.a.y + dy });
                L.b = onEdge(eb, { L.b.x + dx, L.b.y + dy });
                dragStart_ = p;
            }
            rebuildSplitGroup(dragGroup_);
            if (win) win->refresh();
        }
        return;
    }

    if (marqueeSelecting_) {
        marqueeCur_ = p;
        if (win) win->refresh();
        return;
    }

    if (rotatingImg_ && hasSelection()) {
        auto& c = cells_[selected_];
        auto f = imageFrameOf(c);
        if (f.valid) {
            const float ang = std::atan2(p.y - f.cy, p.x - f.cx);
            float deg = rotStartDeg_ + (ang - rotStartAngle_) * 180.f / 3.14159265f;
            if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) deg = std::round(deg / 15.f) * 15.f;
            c.rotation = deg;
            notifyChanged(false);
            if (win) win->refresh();
        }
        return;
    }

    if (imgResizing_ && hasSelection()) {
        auto& c = cells_[selected_];
        auto f = imageFrameOf(c);
        if (f.valid) {
            const float ca = std::cos(f.angleRad), sa = std::sin(f.angleRad);
            const float dx = p.x - f.cx, dy = p.y - f.cy;
            const float lx = dx * ca + dy * sa;
            const float ly = -dx * sa + dy * ca;
            const bool isCorner = (imgHandle_ == Handle::TL || imgHandle_ == Handle::TR
                || imgHandle_ == Handle::BR || imgHandle_ == Handle::BL);
            // 与布局相反：图片默认锁比例缩放；按住 Shift 才解锁自由拉伸
            const bool freeRatio = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (!freeRatio) {
                float d0 = 0.f, d1 = 0.f;
                if (isCorner) {
                    d0 = std::sqrt(imgStartLocal_.x * imgStartLocal_.x + imgStartLocal_.y * imgStartLocal_.y);
                    d1 = std::sqrt(lx * lx + ly * ly);
                }
                else if (imgHandle_ == Handle::L || imgHandle_ == Handle::R) {
                    d0 = std::fabs(imgStartLocal_.x);
                    d1 = std::fabs(lx);
                }
                else {
                    d0 = std::fabs(imgStartLocal_.y);
                    d1 = std::fabs(ly);
                }
                if (d0 > 1.f) c.imgScale = std::clamp(imgStartScale_ * (d1 / d0), 0.05f, 10.f);
            }
            else if (isCorner) {
                // 自由拉伸：横竖各自按拖拽距离缩放
                const float d0x = std::fabs(imgStartLocal_.x), d0y = std::fabs(imgStartLocal_.y);
                if (d0x > 1.f) c.imgNW = std::clamp(imgStartNW_ * (std::fabs(lx) / d0x), 0.05f, 12.f);
                if (d0y > 1.f) c.imgNH = std::clamp(imgStartNH_ * (std::fabs(ly) / d0y), 0.05f, 12.f);
            }
            else if (imgHandle_ == Handle::L || imgHandle_ == Handle::R) {
                const float d0 = std::fabs(imgStartLocal_.x);
                if (d0 > 1.f) c.imgNW = std::clamp(imgStartNW_ * (std::fabs(lx) / d0), 0.05f, 12.f);
            }
            else if (imgHandle_ == Handle::T || imgHandle_ == Handle::B) {
                const float d0 = std::fabs(imgStartLocal_.y);
                if (d0 > 1.f) c.imgNH = std::clamp(imgStartNH_ * (std::fabs(ly) / d0), 0.05f, 12.f);
            }
            notifyChanged(false);
            if (win) win->refresh();
        }
        return;
    }

    if (resizing_ && hasSelection()) {
        Pt w = p;
        snapGuides_.clear();
        {
            // 拖边/拖角缩放时，正在拖的边吸附到其它容器、画板边缘，以及图片自身的边缘。
            const float tolW = kSnapTol / std::max(0.01f, viewScale());
            const auto h = resizeHandle_;
            const bool doX = (h == Handle::TL || h == Handle::L || h == Handle::BL
                || h == Handle::TR || h == Handle::R || h == Handle::BR);
            const bool doY = (h == Handle::TL || h == Handle::T || h == Handle::TR
                || h == Handle::BL || h == Handle::B || h == Handle::BR);
            const std::vector<int> ignore{ selected_ };
            auto addImageTargets = [&](std::vector<float>& targets, bool verticalAxis) {
                for (const auto& c : cells_) {
                    if (c.image.empty()) continue;
                    const auto f = imageFrameOf(c);
                    if (!f.valid) continue;
                    const float hw = f.w * 0.5f, hh = f.h * 0.5f;
                    const float ca = std::cos(f.angleRad), sa = std::sin(f.angleRad);
                    float lo = std::numeric_limits<float>::max();
                    float hi = std::numeric_limits<float>::lowest();
                    for (int sx = -1; sx <= 1; sx += 2) {
                        for (int sy = -1; sy <= 1; sy += 2) {
                            const float x = f.cx + sx * hw * ca - sy * hh * sa;
                            const float y = f.cy + sx * hw * sa + sy * hh * ca;
                            const float v = verticalAxis ? x : y;
                            lo = std::min(lo, v);
                            hi = std::max(hi, v);
                        }
                    }
                    if (lo <= hi) {
                        targets.push_back(lo);
                        targets.push_back(hi);
                    }
                }
            };
            float g = 0.f, d = 0.f;
            bool s = false;
            if (doX) {
                auto tx = snapTargets(true, ignore);
                addImageTargets(tx, true);
                d = snapEdge(w.x, w.x, tx, tolW, g, s);
                if (s) { w.x += d; snapGuides_.push_back({ true, g }); }
            }
            if (doY) {
                auto ty = snapTargets(false, ignore);
                addImageTargets(ty, false);
                d = snapEdge(w.y, w.y, ty, tolW, g, s);
                if (s) { w.y += d; snapGuides_.push_back({ false, g }); }
            }
        }
        // 默认自由缩放（拖边/拖角不限制比例）；按住 Shift 才保持原比例
        const bool keepRatio = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        resizeSelected(resizeHandle_, w, keepRatio);
        notifyChanged(false);
        if (win) win->refresh();
        return;
    }

    if (dragging_ && (tool_ == Tool::None || (tool_ == Tool::Layout && !drawingNewLayout_))
        && hasSelection()) {
        if (imgDrag_ && imageAdjust_) {
            // 容器内图片：拖动平移（偏移按容器宽高的比例存，缩放时仍保持构图）
            auto b = boundsOf(cells_[selected_].poly);
            const float tw = std::max(1.f, b.right - b.left);
            const float th = std::max(1.f, b.bottom - b.top);
            auto& c = cells_[selected_];
            // 按「按下时的偏移 + 累计位移」重算，吸附每帧都从原始位置判断，不会被钉住
            dragAccum_.x += p.x - dragStart_.x;
            dragAccum_.y += p.y - dragStart_.y;
            c.imgOffX = imgOffStartX_ + dragAccum_.x / tw;
            c.imgOffY = imgOffStartY_ + dragAccum_.y / th;
            snapGuides_.clear();
            // 图片（未旋转时）四边吸附到容器四边：图片和容器之间也能自由对齐
            if (!dragShift_ && std::fabs(c.rotation) < 0.01f) {
                auto f = imageFrameOf(c);
                if (f.valid) {
                    const float tolW = kSnapTol / std::max(0.01f, viewScale());
                    const std::vector<float> tx{ b.left, b.right };
                    const std::vector<float> ty{ b.top, b.bottom };
                    float gx = 0.f, gy = 0.f;
                    bool sx = false, sy = false;
                    const float sdx = snapEdge(f.cx - f.w * 0.5f, f.cx + f.w * 0.5f, tx, tolW, gx, sx);
                    const float sdy = snapEdge(f.cy - f.h * 0.5f, f.cy + f.h * 0.5f, ty, tolW, gy, sy);
                    if (sx) { c.imgOffX += sdx / tw; snapGuides_.push_back({ true, gx }); }
                    if (sy) { c.imgOffY += sdy / th; snapGuides_.push_back({ false, gy }); }
                }
            }
            const float lim = 0.5f + (1.f - 1.f / std::max(0.2f, c.imgScale)) * 0.5f;
            c.imgOffX = std::max(-lim, std::min(lim, c.imgOffX));
            c.imgOffY = std::max(-lim, std::min(lim, c.imgOffY));
            dragStart_ = p;
            if (win) win->refresh();
            return;
        }
        // 累计未吸附位移；吸附只影响这一次的落点，下一帧仍从原始位置算
        dragAccum_.x += p.x - dragStart_.x;
        dragAccum_.y += p.y - dragStart_.y;
        float dx = dragAccum_.x;
        float dy = dragAccum_.y;
        // 多选/编组整体移动：先算出这次一起移动的容器集合，再用整体包围盒做吸附
        std::vector<int> moveSet = selectedCellIndices();
        for (int idx : selectedCellIndices()) {
            const int g = cells_[idx].group;
            if (g < 0) continue;
            for (int k = 0; k < (int)cells_.size(); ++k)
                if (cells_[k].group == g
                    && std::find(moveSet.begin(), moveSet.end(), k) == moveSet.end())
                    moveSet.push_back(k);
        }
        snapGuides_.clear();
        if (!dragShift_) {
            const float tolW = kSnapTol / std::max(0.01f, viewScale());
            D2D1_RECT_F mv{ FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX };
            for (int idx : moveSet) {
                const auto& src = (idx < (int)dragOrigPolys_.size())
                    ? dragOrigPolys_[idx] : cells_[idx].poly;
                auto bb = boundsOf(src);
                mv.left = std::min(mv.left, bb.left);
                mv.top = std::min(mv.top, bb.top);
                mv.right = std::max(mv.right, bb.right);
                mv.bottom = std::max(mv.bottom, bb.bottom);
            }
            const auto tx = snapTargets(true, moveSet);
            const auto ty = snapTargets(false, moveSet);
            float gx = 0.f, gy = 0.f;
            bool sx = false, sy = false;
            const float sdx = snapEdge(mv.left + dx, mv.right + dx, tx, tolW, gx, sx);
            const float sdy = snapEdge(mv.top + dy, mv.bottom + dy, ty, tolW, gy, sy);
            if (sx) { dx += sdx; snapGuides_.push_back({ true, gx }); }
            if (sy) { dy += sdy; snapGuides_.push_back({ false, gy }); }
        }
        for (int idx : moveSet) {
            if (idx < 0 || idx >= (int)cells_.size()) continue;
            if (idx < (int)dragOrigPolys_.size()) {
                auto poly = dragOrigPolys_[idx];
                for (auto& v : poly) { v.x += dx; v.y += dy; }
                cells_[idx].poly = std::move(poly);
            }
            else {
                translateCell(cells_[idx], dx, dy);
            }
        }
        // 布局组：外框跟着整组平移（拖缝/重建都以它为基准）
        {
            std::vector<int> movedGroups;
            for (int idx : moveSet) {
                const int gid = layoutGroupOf(idx);
                if (gid >= 0 && std::find(movedGroups.begin(), movedGroups.end(), gid) == movedGroups.end())
                    movedGroups.push_back(gid);
            }
            for (int gid : movedGroups) {
                auto& rect = layoutGroups_[gid].rect;
                rect.left += dx; rect.right += dx;
                rect.top += dy; rect.bottom += dy;
            }
        }
        // 分割组：分割线与控点跟着容器一起移动（用按下时的原始线 + 累计位移重算）
        {
            std::vector<int> movedSplitGroups;
            for (int idx : moveSet) {
                const int gid = cells_[idx].group;
                if (gid < 0 || layoutGroupOf(idx) >= 0) continue;
                if (std::find(movedSplitGroups.begin(), movedSplitGroups.end(), gid) != movedSplitGroups.end())
                    continue;
                auto itg = splitGroups_.find(gid);
                if (itg == splitGroups_.end()) continue;
                movedSplitGroups.push_back(gid);
                auto orig = dragOrigSplitLines_.find(gid);
                if (orig != dragOrigSplitLines_.end()) itg->second.lines = orig->second;
                auto origPoly = dragOrigSplitOriginal_.find(gid);
                if (origPoly != dragOrigSplitOriginal_.end()) itg->second.original = origPoly->second;
                for (auto& L : itg->second.lines) {
                    L.a.x += dx; L.a.y += dy;
                    L.b.x += dx; L.b.y += dy;
                }
                // 原容器同样跟着走：之后改间距/重算分割时仍以当前容器位置为准
                for (auto& q : itg->second.original) { q.x += dx; q.y += dy; }
            }
        }
        dragStart_ = p;
        if (win) win->refresh();
        return;
    }

    if (tool_ == Tool::Layout && dragShift_ && drawingNewLayout_) {
        float dx = p.x - dragStart_.x, dy = p.y - dragStart_.y;
        float ratio = (float)cols_ / (float)std::max(1, rows_);
        if (std::abs(dx) > std::abs(dy) * ratio)
            dragCur_.y = dragStart_.y + (dy < 0 ? -1.f : 1.f) * std::abs(dx) / ratio;
        else
            dragCur_.x = dragStart_.x + (dx < 0 ? -1.f : 1.f) * std::abs(dy) * ratio;
    }
    if (tool_ == Tool::Split && dragShift_) {
        float dx = p.x - dragStart_.x, dy = p.y - dragStart_.y;
        float ang = std::atan2(dy, dx);
        float snap = std::round(ang / (3.14159265f / 4.f)) * (3.14159265f / 4.f);
        float len = std::sqrt(dx * dx + dy * dy);
        dragCur_.x = dragStart_.x + std::cos(snap) * len;
        dragCur_.y = dragStart_.y + std::sin(snap) * len;
    }
    // 拖拽布局：尺寸行实时显示这次拖出的区域尺寸
    if (tool_ == Tool::Layout && dragging_ && drawingNewLayout_ && onLiveSize) {
        const int lw = (int)std::lround(std::fabs(dragCur_.x - dragStart_.x));
        const int lh = (int)std::lround(std::fabs(dragCur_.y - dragStart_.y));
        onLiveSize(lw, lh, true);
    }
    // 悬停布局组内部缝隙：高亮该缝（表格式控件提示）
    if (tool_ == Tool::None && hasSelection() && !dragging_ && !resizing_ && !draggingDivider_) {
        int hg = -1, hi = -1;
        layoutDividerHit(pos, hg, hi);
        if (hg != hoverDividerGroup_ || hi != hoverDividerIndex_) {
            hoverDividerGroup_ = hg;
            hoverDividerIndex_ = hi;
            if (win) win->refresh();
        }
    }
    // 空工具下移到标注上时，把悬停交给标注宿主（单击点选由 onMouseDown 命中处理）
    if (tool_ == Tool::None && hoverCell_ < 0 && annot_ && annot_->hasAnyShapes())
        annot_->forwardMove(pos);
    if (win) win->refresh();
}

void ImageEditCollage::onMouseUp(POINT pos, bool right)
{
    clearSnapGuides();
    if (onLiveSize) onLiveSize(0, 0, false);   // 结束拖拽布局预览：尺寸行回到选中/画板尺寸
    if (panning_) {
        panning_ = false;
        ReleaseCapture();
        return;
    }
    if (uiHitTest && uiHitTest(pos)) {
        if (dragging_ || resizing_) {
            dragging_ = resizing_ = false;
            if (win) win->refresh();
        }
        return;
    }
    if (annotating()) {
        if (annot_) annot_->forwardUp(pos, right);
        return;
    }
    if (right) return;

    // 空工具下点选/拖动标注结束
    if (annotDragging_) {
        annotDragging_ = false;
        if (annot_) annot_->forwardUp(pos, false);
        notifyChanged(false);
        if (win) win->refresh();
        return;
    }

    // 布局组缝隙拖动结束
    if (draggingDivider_) {
        draggingDivider_ = false;
        dragDividerGroup_ = -1;
        dragDividerIndex_ = -1;
        notifyChanged();
        if (win) win->refresh();
        return;
    }

    if (marqueeSelecting_) {
        marqueeSelecting_ = false;
        const float x0 = std::min(marqueeStart_.x, marqueeCur_.x);
        const float y0 = std::min(marqueeStart_.y, marqueeCur_.y);
        const float x1 = std::max(marqueeStart_.x, marqueeCur_.x);
        const float y1 = std::max(marqueeStart_.y, marqueeCur_.y);
        if (!marqueeAdditive_) clearCellSelection();
        for (int i = 0; i < (int)cells_.size(); ++i) {
            auto b = boundsOf(cells_[i].poly);
            if (b.right < x0 || b.left > x1 || b.bottom < y0 || b.top > y1) continue;
            // 分割块框到任意一块 = 整组一起选中
            const int g = cells_[i].group;
            if (g >= 0) {
                for (int k = 0; k < (int)cells_.size(); ++k) {
                    if (cells_[k].group != g || isCellSelected(k)) continue;
                    if (!hasSelection()) selected_ = k;
                    else extraCells_.push_back(k);
                }
                continue;
            }
            if (isCellSelected(i)) continue;
            if (!hasSelection()) selected_ = i;
            else extraCells_.push_back(i);
        }
        // 标注：同一块矩形换算成画板像素后一起框选
        if (annot_) {
            auto art = artRect();
            D2D1_RECT_F ar{ x0 - art.left, y0 - art.top, x1 - art.left, y1 - art.top };
            annot_->selectShapesInRect(ar, marqueeAdditive_);
        }
        notifyChanged(false);
        if (win) win->refresh();
        return;
    }

    if (imgResizing_ || rotatingImg_) {
        imgResizing_ = rotatingImg_ = false;
        notifyChanged();
        if (win) win->refresh();
        return;
    }

    if (resizing_) {
        resizing_ = false;
        notifyChanged();
        if (win) win->refresh();
        return;
    }
    // 分割线拖拽结束
    if (tool_ == Tool::None && adjustingSplit_) {
        adjustingSplit_ = false;
        dragging_ = false;
        dragGroup_ = -1;
        dragLineIndex_ = -1;
        splitEnd_ = -1;
        notifyChanged();
        if (win) win->refresh();
        return;
    }
    if (!dragging_) return;
    dragging_ = false;
    if (tool_ == Tool::Split) {
        // 松手才判定：只有这条线完整穿过某个容器（两个边交点）才分割
        const int target = splitTargetForSegment(dragStart_, dragCur_);
        if (target >= 0) commitSplit(target, dragStart_, dragCur_);
        adjustingSplit_ = false;
        dragGroup_ = -1;
        dragLineIndex_ = -1;
    } else if (tool_ == Tool::Layout) {
        if (drawingNewLayout_) {   // 只有空白起拖才建新布局（点在已有布局上是选中/拖动）
            D2D1_RECT_F rc{ dragStart_.x, dragStart_.y, dragCur_.x, dragCur_.y };
            createLayoutInRect(rc);
        }
        drawingNewLayout_ = false;
    } else {
        notifyChanged();
    }
    if (win) win->refresh();
}

void ImageEditCollage::onMouseWheel(POINT pos, float space)
{
    if (uiHitTest && uiHitTest(pos)) {
        if (dragging_) { dragging_ = false; adjustingSplit_ = false; if (win) win->refresh(); }
        return;
    }
    if (space == 0.f || !isPosIn(pos)) return;

    // 标注工具：滚轮按截图口径调笔迹粗细
    if (annotating()) {
        if (annot_) annot_->forwardWheel(space > 0.f ? 1 : -1);
        return;
    }

    // 双击进入图片调整后：滚轮缩放容器内的图片
    if (imageAdjust_ && hasSelection() && cells_[selected_].image.size()) {
        float factor = space > 0.f ? 1.1f : 0.9f;
        auto& c = cells_[selected_];
        c.imgScale = std::max(0.2f, std::min(5.f, c.imgScale * factor));
        if (win) win->refresh();
        return;
    }
    if (tool_ != Tool::None) return;

    float s1 = viewScale();
    float ox = 0.f, oy = 0.f;
    viewOrigin(ox, oy);
    auto art = artRect();
    float lx = (float)pos.x - x;
    float ly = (float)pos.y - y;
    Pt before{ (lx - ox) / s1 + art.left, (ly - oy) / s1 + art.top };

    float factor = space > 0.f ? 1.1f : 0.9f;
    zoomFactor_ = std::max(kZoomMin, std::min(kZoomMax, zoomFactor_ * factor));
    float s2 = viewScale();
    // 以鼠标下的世界点为锚点缩放：世界0 的屏幕位置补偿
    panDX_ = lx - w * 0.5f - before.x * s2;
    panDY_ = ly - h * 0.5f - before.y * s2;

    if (onZoomChanged) onZoomChanged();
    if (win) win->refresh();
}

void ImageEditCollage::onKeyDown(UINT key)
{
    if (key == VK_SPACE) { spaceHeld_ = true; return; }

    // 删除键通用：标注（含文字编辑中的文字）与容器/布局都删，不依赖当前工具状态
    if (key == VK_DELETE || key == VK_BACK) {
        const bool annotUsed = annot_ ? annot_->forwardDeleteKey(key) : false;
        auto sel = selectedCellIndices();
        if (!sel.empty()) {
            pushUndo();
            // 删到分割组/布局组里的任意一块：整组一起删，并清掉组记录
            std::vector<int> drop = sel;
            for (int idx : sel) {
                if (idx < 0 || idx >= (int)cells_.size()) continue;
                const int g = cells_[idx].group;
                if (g < 0) continue;
                for (int k = 0; k < (int)cells_.size(); ++k)
                    if (cells_[k].group == g
                        && std::find(drop.begin(), drop.end(), k) == drop.end())
                        drop.push_back(k);
                splitGroups_.erase(g);
                layoutGroups_.erase(g);
            }
            std::sort(drop.begin(), drop.end());
            for (int i = (int)drop.size() - 1; i >= 0; --i) {
                const int idx = drop[i];
                if (idx >= 0 && idx < (int)cells_.size()) cells_.erase(cells_.begin() + idx);
            }
            selected_ = -1;
            extraCells_.clear();
            hoverCell_ = -1;
            notifyChanged();
        }
        else if (annotUsed) {
            notifyChanged();   // 标注侧已删：刷新画布
        }
        if (win) win->refresh();
        return;
    }

    // 标注工具：其余键盘（撤销/重做/退出文字编辑）交给标注宿主
    if (annotating()) {
        if (annot_) annot_->forwardKey(key);
        return;
    }
    // 空工具但选中了标注：Ctrl+Z 撤销的也是标注，不能被拼图撤销栈吃掉
    if (annot_ && annot_->hasAnyAnnotSelection()
        && key == 'Z' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        annot_->forwardKey(key);
        return;
    }
    if (key == VK_ESCAPE) {
        if (dragging_ || resizing_) {
            dragging_ = resizing_ = false;
            if (win) win->refresh();
        } else if (imageAdjust_) {
            imageAdjust_ = false;
            if (win) win->refresh();
        } else {
            // Esc 通用：容器/布局选中与标注选中一起取消
            bool cleared = false;
            if (hasSelection() || !extraCells_.empty()) {
                clearCellSelection();
                cleared = true;
            }
            if (annot_ && annot_->hasAnyAnnotSelection()) {
                annot_->clearAnnotSelectionPublic();
                cleared = true;
            }
            if (cleared) {
                notifyChanged(false);
                if (win) win->refresh();
            }
        }
    } else if (key == 'Z' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        // 与「系统按键」约定一致：Ctrl+Z 撤销、Ctrl+Shift+Z 重做
        if (GetKeyState(VK_SHIFT) & 0x8000) redo();
        else undo();
    } else if (key == 'C' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        // 复制选中的容器（图片路径 + 容器内图片的位置/缩放一起带走）
        cellClipboard_.clear();
        for (int idx : selectedCellIndices()) {
            if (idx < 0 || idx >= (int)cells_.size()) continue;
            Cell c = cells_[idx];
            c.group = -1;
            cellClipboard_.push_back(c);
        }
    } else if (key == 'V' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        pasteCells();
    }
}

void ImageEditCollage::onKeyUp(UINT key)
{
    if (key == VK_SPACE) spaceHeld_ = false;
}

void ImageEditCollage::onMiddleDown(POINT pos)
{
    if (!isPosIn(pos)) return;
    panning_ = true;
    panStart_ = pos;
    panStartDX_ = panDX_;
    panStartDY_ = panDY_;
    SetCapture(win->hwnd);
}

void ImageEditCollage::onMiddleUp(POINT)
{
    if (!panning_) return;
    panning_ = false;
    ReleaseCapture();
}

void ImageEditCollage::onCursor(bool* handled)
{
    if (!win) return;
    POINT pos{};
    GetCursorPos(&pos);
    ScreenToClient(win->hwnd, &pos);
    if (!isPosIn(pos)) return;
    // 工具栏 / 顶部胶囊上的按钮优先：Button 自己的 onCursor 已经给了手型，
    // 画布不要再拿工具光标（十字等）盖掉它 —— 截图栏里工具条是独立窗口，本来就不会互相覆盖。
    if (handled && *handled) return;
    if (uiHitTest && uiHitTest(pos)) return;

    // 快捷变换按钮：悬停手型
    if (!panning_ && !spaceHeld_ && !dragging_ && quickActionHit(pos) >= 0) {
        SetCursor(LoadCursor(nullptr, IDC_HAND));
        if (handled) *handled = true;
        return;
    }

    // 任意工具状态下，鼠标悬停到容器/布局上都显示手型（和标注的悬停选中逻辑一致）
    if (tool_ != Tool::None && !dragging_ && !resizing_) {
        const Pt wp = toWorld(pos);
        if (cellAt(wp) >= 0) {
            SetCursor(LoadCursor(nullptr, IDC_HAND));
            if (handled) *handled = true;
            return;
        }
    }
    if (panning_ || spaceHeld_) {
        SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
        if (handled) *handled = true;
        return;
    }
    // 标注工具：光标与截图一致（控件、十字、工字型…）
    if (annotating() && annot_) {
        annot_->applyAnnotCursor();
        if (handled) *handled = true;
        return;
    }
    if (tool_ == Tool::Split) {
        SetCursor(LoadCursor(nullptr, IDC_CROSS));
        if (handled) *handled = true;
        return;
    }
    if (tool_ == Tool::Layout) {
        // 布局工具下：悬停在已有布局/容器上出「手型」提示可点选；空白处保持十字准备拖新布局
        if (!dragging_) {
            const Pt wp = toWorld(pos);
            if (cellAt(wp) >= 0) {
                SetCursor(LoadCursor(nullptr, IDC_HAND));
                if (handled) *handled = true;
                return;
            }
        }
        SetCursor(LoadCursor(nullptr, IDC_CROSS));
        if (handled) *handled = true;
        return;
    }
    // 分割组选中：悬停分割线/端点显示可拖动光标
    if (selectionGroup() >= 0) {
        // 布局组内部缝隙：竖缝左右拖、横缝上下拖
        {
            int dg = -1, di = -1;
            if (layoutDividerHit(pos, dg, di)) {
                bool vertical = true;
                auto it = layoutGroups_.find(dg);
                if (it != layoutGroups_.end()) {
                    const auto divs = layoutDividers(it->second);
                    if (di >= 0 && di < (int)divs.size()) vertical = divs[di].vertical;
                }
                SetCursor(LoadCursor(nullptr, vertical ? IDC_SIZEWE : IDC_SIZENS));
                if (handled) *handled = true;
                return;
            }
        }
        int g = -1, l = -1, e = -1;
        if (splitLineHit(pos, g, l, e)) {
            // 分割控点：手型（点按可拖）；分割线线身：按可移动方向显示上下/左右箭头
            if (e >= 0) {
                SetCursor(LoadCursor(nullptr, IDC_HAND));
                if (handled) *handled = true;
                return;
            }
            bool horizontalLine = true;
            auto it = splitGroups_.find(g);
            if (it != splitGroups_.end() && l >= 0 && l < (int)it->second.lines.size()) {
                const auto& L = it->second.lines[l];
                horizontalLine = std::fabs(L.b.x - L.a.x) >= std::fabs(L.b.y - L.a.y);
            }
            SetCursor(LoadCursor(nullptr, horizontalLine ? IDC_SIZENS : IDC_SIZEWE));
            if (handled) *handled = true;
            return;
        }
    }
    // 图片调整：虚线控件（旋转/缩放）优先
    if (imageAdjust_ && hasSelection() && !cells_[selected_].image.empty()) {
        Handle ih = Handle::None;
        int ci = -1;
        const auto h = imageHitAt(pos, ih, ci);
        if (h == ImgHit::Rotate) {
            // cornerIndex 是 0..3（TL/TR/BR/BL），AnnotCursor::rotate 用的是 0/2/4/6 角序号
            static const int kRotCorner[4]{ 0, 2, 4, 6 };
            const int corner = (ci >= 0 && ci < 4) ? kRotCorner[ci] : 0;
            if (HCURSOR cur = AnnotCursor::rotate(corner, imageFrameOf(cells_[selected_]).angleRad))
                SetCursor(cur);
            if (handled) *handled = true;
            return;
        }
        if (h == ImgHit::Corner || h == ImgHit::Edge) {
            switch (ih) {
            case Handle::TL: case Handle::BR:
                SetCursor(LoadCursor(nullptr, IDC_SIZENWSE)); break;
            case Handle::TR: case Handle::BL:
                SetCursor(LoadCursor(nullptr, IDC_SIZENESW)); break;
            case Handle::T: case Handle::B:
                SetCursor(LoadCursor(nullptr, IDC_SIZENS)); break;
            default:
                SetCursor(LoadCursor(nullptr, IDC_SIZEWE)); break;
            }
            if (handled) *handled = true;
            return;
        }
        if (h == ImgHit::Inside) {
            SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
            if (handled) *handled = true;
            return;
        }
    }

    // 选中容器的缩放控点：给对应的缩放光标
    if (hasSelection()) {
        switch (handleAt(pos)) {
        case Handle::TL: case Handle::BR:
            SetCursor(LoadCursor(nullptr, IDC_SIZENWSE)); if (handled) *handled = true; return;
        case Handle::TR: case Handle::BL:
            SetCursor(LoadCursor(nullptr, IDC_SIZENESW)); if (handled) *handled = true; return;
        case Handle::T: case Handle::B:
            SetCursor(LoadCursor(nullptr, IDC_SIZENS)); if (handled) *handled = true; return;
        case Handle::L: case Handle::R:
            SetCursor(LoadCursor(nullptr, IDC_SIZEWE)); if (handled) *handled = true; return;
        default: break;
        }
    }
    const Pt wp = toWorld(pos);
    if (cellAt(wp) >= 0) {
        SetCursor(LoadCursor(nullptr, imageAdjust_ ? IDC_SIZEALL : IDC_HAND));
        if (handled) *handled = true;
        return;
    }
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
    if (handled) *handled = true;
}

void ImageEditCollage::resizeSelected(Handle h, const Pt& world, bool keepRatio)
{
    if (!hasSelection()) return;
    const int selGroup = selectionGroup();
    const bool layoutResize = isLayoutGroup(selGroup);
    auto splitIt = splitGroups_.find(selGroup);
    const bool splitResize = !layoutResize && selGroup >= 0 && splitIt != splitGroups_.end();
    auto b = layoutResize
        ? layoutGroups_[selGroup].rect
        : (splitResize ? boundsOf(splitIt->second.original)
            : boundsOf(cells_[selected_].poly));
    const bool left = (h == Handle::TL || h == Handle::L || h == Handle::BL);
    const bool right = (h == Handle::TR || h == Handle::R || h == Handle::BR);
    const bool top = (h == Handle::TL || h == Handle::T || h == Handle::TR);
    const bool bottom = (h == Handle::BL || h == Handle::B || h == Handle::BR);

    float nl = b.left, nr = b.right, nt = b.top, nb = b.bottom;
    if (left) nl = std::min(world.x, nr - 8.f);
    if (right) nr = std::max(world.x, nl + 8.f);
    if (top) nt = std::min(world.y, nb - 8.f);
    if (bottom) nb = std::max(world.y, nt + 8.f);

    if (keepRatio) {
        const float ratio = (b.right - b.left) / std::max(1.f, b.bottom - b.top);
        if ((left || right) && (top || bottom)) {
            const float ww = nr - nl, hh = nb - nt;
            if (ww / std::max(1.f, hh) > ratio) {
                nb = nt + ww / ratio;
                if (bottom) nb = std::max(nb, nt + 8.f);
            } else {
                nr = nl + hh * ratio;
                if (right) nr = std::max(nr, nl + 8.f);
            }
        }
        else if (left || right) {
            // 拖左右边：高按原比例跟随，纵向以原中心为基准
            const float ww = std::max(8.f, nr - nl);
            const float nh = ww / ratio;
            const float cy = (b.top + b.bottom) * 0.5f;
            nt = cy - nh * 0.5f;
            nb = cy + nh * 0.5f;
        }
        else if (top || bottom) {
            // 拖上下边：宽按原比例跟随，横向以原中心为基准
            const float hh = std::max(8.f, nb - nt);
            const float nw = hh * ratio;
            const float cx = (b.left + b.right) * 0.5f;
            nl = cx - nw * 0.5f;
            nr = cx + nw * 0.5f;
        }
    }

    const float sx = (b.right - b.left) > 0.01f ? (nr - nl) / (b.right - b.left) : 1.f;
    const float sy = (b.bottom - b.top) > 0.01f ? (nb - nt) / (b.bottom - b.top) : 1.f;

    // 拖容器边只改容器，图片保持原来的绝对大小和中心（多出来的部分由容器裁剪）。
    // 只有双击进入图片模式后按 Shift 才允许单独拉伸图片。
    struct ImageSnap { float cx = 0, cy = 0, w = 0, h = 0; bool valid = false; };
    auto snapOf = [this](const Cell& c) {
        ImageSnap s;
        if (c.image.empty()) return s;
        auto cb = boundsOf(c.poly);
        const float cw = std::max(1.f, cb.right - cb.left);
        const float ch = std::max(1.f, cb.bottom - cb.top);
        const float nw = c.stretch ? 1.f : (c.imgNW > 0.f ? c.imgNW : 1.f);
        const float nh = c.stretch ? 1.f : (c.imgNH > 0.f ? c.imgNH : 1.f);
        s.cx = (cb.left + cb.right) * 0.5f + c.imgOffX * cw;
        s.cy = (cb.top + cb.bottom) * 0.5f + c.imgOffY * ch;
        s.w = cw * nw * c.imgScale;
        s.h = ch * nh * c.imgScale;
        s.valid = s.w > 0.f && s.h > 0.f;
        return s;
    };
    auto applySnap = [this](Cell& c, const ImageSnap& s) {
        if (!s.valid || c.image.empty()) return;
        c.stretch = false;   // 固定成当前实际尺寸，不再跟着容器重新铺满
        auto cb = boundsOf(c.poly);
        const float cw = std::max(1.f, cb.right - cb.left);
        const float ch = std::max(1.f, cb.bottom - cb.top);
        if (c.imgScale <= 0.f) return;
        c.imgNW = s.w / (cw * c.imgScale);
        c.imgNH = s.h / (ch * c.imgScale);
        c.imgOffX = (s.cx - (cb.left + cb.right) * 0.5f) / cw;
        c.imgOffY = (s.cy - (cb.top + cb.bottom) * 0.5f) / ch;
    };

    if (layoutResize) {
        // 整组缩放：所有格子按外框映射（比例不变）
        auto& grp = layoutGroups_[selGroup];
        std::vector<std::pair<int, ImageSnap>> snaps;
        for (int idx : grp.members) {
            if (idx < 0 || idx >= (int)cells_.size()) continue;
            snaps.emplace_back(idx, snapOf(cells_[idx]));
            for (auto& p : cells_[idx].poly) {
                p.x = nl + (p.x - b.left) * sx;
                p.y = nt + (p.y - b.top) * sy;
            }
        }
        grp.rect = D2D1::RectF(nl, nt, nr, nb);
        for (auto& [idx, snap] : snaps) applySnap(cells_[idx], snap);
        return;
    }
    if (splitResize) {
        // 分割组整体缩放：原容器、分割线、各块一起按外框映射
        auto& grp = splitIt->second;
        std::vector<std::pair<int, ImageSnap>> snaps;
        for (int idx : selectedCellIndices()) {
            if (idx < 0 || idx >= (int)cells_.size()) continue;
            if (cells_[idx].group != selGroup) continue;
            snaps.emplace_back(idx, snapOf(cells_[idx]));
        }
        auto mapPt = [&](Pt& q) {
            q.x = nl + (q.x - b.left) * sx;
            q.y = nt + (q.y - b.top) * sy;
            };
        for (auto& q : grp.original) mapPt(q);
        for (auto& L : grp.lines) { mapPt(L.a); mapPt(L.b); }
        for (auto& [idx, snap] : snaps) {
            for (auto& q : cells_[idx].poly) mapPt(q);
            applySnap(cells_[idx], snap);
        }
        return;
    }
    auto& c = cells_[selected_];
    const ImageSnap snap = snapOf(c);
    for (auto& p : c.poly) {
        p.x = nl + (p.x - b.left) * sx;
        p.y = nt + (p.y - b.top) * sy;
    }
    applySnap(c, snap);
}

// ---------------- 图片位图 ----------------

ID2D1Bitmap* ImageEditCollage::bitmapFor(ID2D1RenderTarget* rt, const std::wstring& path, int& imgW, int& imgH)
{
    imgW = imgH = 0;
    if (!rt || path.empty()) return nullptr;
    // D2D 位图只属于创建它的渲染目标：目标一换（设备重建 / 导出）就整体重做，
    // 否则沿用旧位图会让容器里「有控件框但没有图」
    if (bitmapRT_ != rt) {
        for (auto& [key, bmp] : bitmapCache_)
            if (bmp) bmp->Release();
        bitmapCache_.clear();
        bitmapSize_.clear();
        bitmapRT_ = rt;
    }
    auto it = bitmapCache_.find(path);
    if (it != bitmapCache_.end()) {
        auto sz = bitmapSize_[path];
        imgW = sz.first;
        imgH = sz.second;
        return it->second;
    }
    auto wic = wicFactory();
    if (!wic) return nullptr;

    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, &decoder))) return nullptr;
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return nullptr;
    Microsoft::WRL::ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(&conv))) return nullptr;
    if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone, nullptr, 0.f, WICBitmapPaletteTypeCustom))) return nullptr;

    UINT ww = 0, hh = 0;
    if (FAILED(conv->GetSize(&ww, &hh)) || ww == 0 || hh == 0) return nullptr;

    // 超过 D2D 常用位图上限时先等比缩小再建位图；绘制端按目标矩形贴图，缩图不影响构图
    constexpr UINT kMaxSide = 8192;
    IWICBitmapSource* src = conv.Get();
    UINT ow = ww, oh = hh;
    Microsoft::WRL::ComPtr<IWICBitmapScaler> scaler;
    if (ww > kMaxSide || hh > kMaxSide) {
        const float ratio = std::min((float)kMaxSide / (float)ww, (float)kMaxSide / (float)hh);
        ow = std::max(1u, (UINT)std::lround((float)ww * ratio));
        oh = std::max(1u, (UINT)std::lround((float)hh * ratio));
        if (FAILED(wic->CreateBitmapScaler(&scaler))) return nullptr;
        if (FAILED(scaler->Initialize(conv.Get(), ow, oh, WICBitmapInterpolationModeFant))) return nullptr;
        src = scaler.Get();
    }

    ID2D1Bitmap* bmp = nullptr;
    if (FAILED(rt->CreateBitmapFromWicBitmap(src, nullptr, &bmp))) return nullptr;
    imgW = (int)ow;
    imgH = (int)oh;
    bitmapCache_[path] = bmp;
    bitmapSize_[path] = { imgW, imgH };
    return bmp;
}

// 图片与「图片控件框」共用同一个矩形：先按容器形状裁剪，再把位图整张贴进 f 的矩形。
// 这样无论位图像素尺寸/缓存如何，画出来的图片永远正好落在虚线控件内。
void ImageEditCollage::paintCellImage(ID2D1RenderTarget* rt, const Cell& c,
    const D2D1_MATRIX_3X2_F& world, float opacity, const std::vector<Pt>* clipPoly)
{
    if (!rt || c.image.empty()) return;
    int iw = 0, ih = 0;
    auto bmp = bitmapFor(rt, c.image, iw, ih);
    if (!bmp) return;
    auto f = imageFrameOf(c);
    if (!f.valid) return;

    Microsoft::WRL::ComPtr<ID2D1Geometry> clipGeo;
    if (clipPoly && clipPoly->size() >= 3) {
        // 填充图片同样按圆角裁剪（分割块也生效），不再只对空容器画圆角
        if (radius_ > 0.f) clipGeo = buildRoundedPolyGeometry(d2dFactory(), *clipPoly, radius_);
        if (!clipGeo) clipGeo = buildPolyGeometry(d2dFactory(), *clipPoly);
    }

    // 裁剪用图层几何遮罩，坐标与当前世界变换一致
    if (clipGeo) {
        D2D1_LAYER_PARAMETERS params{};
        params.contentBounds = D2D1::InfiniteRect();
        params.geometricMask = clipGeo.Get();
        params.maskAntialiasMode = D2D1_ANTIALIAS_MODE_PER_PRIMITIVE;
        params.maskTransform = D2D1::Matrix3x2F::Identity();
        params.opacity = 1.f;
        params.opacityBrush = nullptr;
        params.layerOptions = D2D1_LAYER_OPTIONS_NONE;
        rt->PushLayer(params, nullptr);
    }

    const D2D1_RECT_F dest = D2D1::RectF(f.cx - f.w * 0.5f, f.cy - f.h * 0.5f,
        f.cx + f.w * 0.5f, f.cy + f.h * 0.5f);
    const D2D1_POINT_2F center{ f.cx, f.cy };
    const auto flip = D2D1::Matrix3x2F::Scale(
        c.flipH ? -1.f : 1.f, c.flipV ? -1.f : 1.f, center);
    rt->SetTransform(D2D1::Matrix3x2F::Rotation(c.rotation, center) * flip * world);
    rt->DrawBitmap(bmp, dest, opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    rt->SetTransform(world);

    if (clipGeo) rt->PopLayer();
}

// ---------------- 绘制 ----------------

void ImageEditCollage::paintWorld(ID2D1RenderTarget* rt, float ox, float oy, float scale, bool chrome)
{
    if (!rt) return;
    rt->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale)
        * D2D1::Matrix3x2F::Translation(ox, oy));
    const auto world = D2D1::Matrix3x2F::Scale(scale, scale)
        * D2D1::Matrix3x2F::Translation(ox, oy);

    auto cellBrush = solidBrush(rt, ImageEditTheme::cellEmpty);

    for (int i = 0; i < (int)cells_.size(); ++i) {
        const auto& poly = cells_[i].poly;
        if (poly.size() < 3) continue;

        Microsoft::WRL::ComPtr<ID2D1Geometry> geo;
        auto rc = boundsOf(poly);
        // 圆角对任意形状生效（矩形/分割后的三角梯形），不再只认轴对齐矩形
        if (radius_ > 0.f) geo = buildRoundedPolyGeometry(d2dFactory(), poly, radius_);
        if (!geo) geo = buildPolyGeometry(d2dFactory(), poly);
        if (!geo) continue;

        if (cells_[i].image.empty()) {
            if (cellBrush) rt->FillGeometry(geo.Get(), cellBrush.Get());
            // 空容器：屏幕上给一圈很淡的边，分割/布局后能看清每一格（导出不带）
            if (chrome) {
                if (auto line = solidBrush(rt, ImageEditTheme::cellEdge)) rt->DrawGeometry(geo.Get(), line.Get(), 1.f);
                // 每个空格子都提示：+ 与「拖拽或选择图片」
                D2D1_MATRIX_3X2_F old{};
                rt->GetTransform(&old);
                rt->SetTransform(D2D1::Matrix3x2F::Identity());
                const float d = (win && win->dpi > 0.f) ? win->dpi : 1.f;
                auto p0 = screenPointOf({ rc.left, rc.top });
                auto p1 = screenPointOf({ rc.right, rc.bottom });
                paintEmptyCellHint(rt, D2D1::RectF(p0.x, p0.y, p1.x, p1.y), d);
                rt->SetTransform(old);
            }
        } else {
            paintCellImage(rt, cells_[i], world, 1.f, &poly);
        }
    }
    // 图片调整模式：先半透明露出整张图（容器外也能看到），再把容器内的部分重新画实
    if (chrome && imageAdjust_ && hasSelection() && !cells_[selected_].image.empty()) {
        auto& c = cells_[selected_];
        paintCellImage(rt, c, world, 0.42f, nullptr);
        paintCellImage(rt, c, world, 1.f, &c.poly);
    }
    if (chrome) rt->SetTransform(D2D1::Matrix3x2F::Identity());
}

void ImageEditCollage::drawGrid(ID2D1RenderTarget* rt)
{
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    auto brush = solidBrush(rt, ImageEditTheme::gridLine);
    if (!brush) return;
    const float d = (win && win->dpi > 0.f) ? win->dpi : 1.f;
    float sp = 40.f * viewScale();
    if (sp < 8.f) sp = 8.f;
    if (sp > 80.f) sp = 80.f;
    // 网格对齐世界原点：先找屏幕内第一个交点
    float ox = 0.f, oy = 0.f;
    viewOrigin(ox, oy);
    auto art = artRect();
    float startX = std::fmod(ox - art.left * viewScale(), sp);
    if (startX < 0.f) startX += sp;
    float startY = std::fmod(oy - art.top * viewScale(), sp);
    if (startY < 0.f) startY += sp;
    const float arm = 2.f * d;
    for (float gx = startX; gx < w; gx += sp) {
        for (float gy = startY; gy < h; gy += sp) {
            rt->DrawLine(D2D1::Point2F(gx - arm, gy), D2D1::Point2F(gx + arm, gy), brush.Get(), 1.f * d);
            rt->DrawLine(D2D1::Point2F(gx, gy - arm), D2D1::Point2F(gx, gy + arm), brush.Get(), 1.f * d);
        }
    }
}

void ImageEditCollage::paintSelection(ID2D1RenderTarget* rt, float ox, float oy, float scale)
{
    if (!hasSelection() || exporting_) return;
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    auto art = artRect();
    auto b = boundsOf(cells_[selected_].poly);
    const float l = ox + (b.left - art.left) * scale;
    const float t = oy + (b.top - art.top) * scale;
    const float r = ox + (b.right - art.left) * scale;
    const float bo = oy + (b.bottom - art.top) * scale;

    const float d = (win && win->dpi > 0.f) ? win->dpi : 1.f;
    const int grp = selectionGroup();
    // 图片调整模式：控件只画在图片上，容器/分割组的框与控点先收起
    const bool adjustingImage = imageAdjust_ && !cells_[selected_].image.empty();
    if (!adjustingImage) {
        // 布局组：外框 + 四角控点 + 组内缝隙（悬停缝隙高亮）
        if (isLayoutGroup(grp)) {
            auto it = layoutGroups_.find(grp);
            if (it != layoutGroups_.end())
                paintLayoutGroupChrome(rt, it->second, ox, oy, scale, true);
        }
        // 分割组：框住原容器整体 + 4 角圆点（控件与矩形标注一致）
        else if (grp >= 0) {
            auto it = splitGroups_.find(grp);
            auto ob = (it != splitGroups_.end()) ? boundsOf(it->second.original) : b;
            const D2D1_RECT_F fr{
                ox + (ob.left - art.left) * scale, oy + (ob.top - art.top) * scale,
                ox + (ob.right - art.left) * scale, oy + (ob.bottom - art.top) * scale };
            if (auto brush = solidBrush(rt, ImageEditTheme::accent)) rt->DrawRectangle(fr, brush.Get(), 1.f * d);
            paintHandleDot(rt, fr.left, fr.top, d);
            paintHandleDot(rt, fr.right, fr.top, d);
            paintHandleDot(rt, fr.right, fr.bottom, d);
            paintHandleDot(rt, fr.left, fr.bottom, d);
        }
        else {
            // 容器选中：主题色实线 + 4 圆点控点
            if (auto brush = solidBrush(rt, ImageEditTheme::accent)) rt->DrawRectangle(D2D1::RectF(l, t, r, bo), brush.Get(), 1.f * d);
            paintHandleDot(rt, l, t, d);
            paintHandleDot(rt, r, t, d);
            paintHandleDot(rt, r, bo, d);
            paintHandleDot(rt, l, bo, d);
        }

        // 框选到的其他容器：同样画主题色实线框（不画控点；同一分割组的块不重复画框）
        if (auto brush = solidBrush(rt, ImageEditTheme::accent)) {
            for (int idx : extraCells_) {
                if (idx < 0 || idx >= (int)cells_.size()) continue;
                if (grp >= 0 && cells_[idx].group == grp) continue;
                auto eb = boundsOf(cells_[idx].poly);
                rt->DrawRectangle(D2D1::RectF(
                    ox + (eb.left - art.left) * scale, oy + (eb.top - art.top) * scale,
                    ox + (eb.right - art.left) * scale, oy + (eb.bottom - art.top) * scale),
                    brush.Get(), 1.f * d);
            }
        }

        // 分割组选中时显示分割线（线 + 两个可拖圆点）
        if (grp >= 0) paintSplitLines(rt, ox, oy, scale);
    }

    // 图片：主题色虚线框 + 4 圆点（和容器区分开）
    if (adjustingImage)
        paintImageFrame(rt, ox, oy, scale);
}

void ImageEditCollage::paintImageFrame(ID2D1RenderTarget* rt, float, float, float)
{
    if (!rt || !hasSelection()) return;
    auto f = imageFrameOf(cells_[selected_]);
    if (!f.valid) return;

    const float ca = std::cos(f.angleRad), sa = std::sin(f.angleRad);
    const float hw = f.w * 0.5f, hh = f.h * 0.5f;
    D2D1_POINT_2F pts[4];
    const float lxs[4]{ -hw, hw, hw, -hw };
    const float lys[4]{ -hh, -hh, hh, hh };
    for (int i = 0; i < 4; ++i) {
        float wx = f.cx + lxs[i] * ca - lys[i] * sa;
        float wy = f.cy + lxs[i] * sa + lys[i] * ca;
        pts[i] = screenPointOf({ wx, wy });
    }

    const float d = (win && win->dpi > 0.f) ? win->dpi : 1.f;
    // 图片控件：主题色虚线
    if (auto dashBrush = solidBrush(rt, ImageEditTheme::accent)) {
        auto* style = dashStyle();
        for (int i = 0; i < 4; ++i)
            rt->DrawLine(pts[i], pts[(i + 1) % 4], dashBrush.Get(), 1.f * d, style);
    }
    // 图片控件：与标注一致的圆点
    for (int i = 0; i < 4; ++i)
        paintHandleDot(rt, pts[i].x, pts[i].y, d);
}

void ImageEditCollage::paintScreen(ID2D1DeviceContext* rt)
{
    if (!rt) return;
    rt->Clear(Color(0x00000000).getD2DColor());

    drawGrid(rt);

    const float s = viewScale();
    float ox = 0.f, oy = 0.f;
    viewOrigin(ox, oy);
    auto art = artRect();

    // 画板：绿色 1px 框（类似截图框选、不带控件）
    const float aw = art.right - art.left;
    const float ah = art.bottom - art.top;
    if (aw > 0.f && ah > 0.f) {
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        D2D1_RECT_F frame{ ox, oy, ox + aw * s, oy + ah * s };
        // 画板底色
        if (auto bgBrush = solidBrush(rt, bgColor_)) rt->FillRectangle(frame, bgBrush.Get());
        // 画板描边（导出不包含）：无色 = 虚线示意画板位置；
        // 浅色模式下白色画板和白色工作区同色，补一圈实线把边界画出来
        if (!exporting_) {
            const float dpi = (win && win->dpi > 0.f) ? win->dpi : 1.f;
            if (bgColor_ == 0u) {
                if (auto line = solidBrush(rt, ImageEditTheme::boardHint))
                    rt->DrawRectangle(frame, line.Get(), 1.f * dpi, dashStyle());
            }
            else if (bgColor_ == 0xFFFFFFFFu && !ImageEditTheme::dark) {
                if (auto line = solidBrush(rt, ImageEditTheme::boardEdge))
                    rt->DrawRectangle(frame, line.Get(), 1.f * dpi);
            }
        }
    }

    paintWorld(rt, ox - art.left * s, oy - art.top * s, s, true);

    // 标注：坐标是画板内像素，按同一套「画板 → 屏幕」变换画在图片之上。
    // 画板像素 (0,0) 对应画板左上角 (art.left)，所以这里只需要 Scale(s) + Translation(ox, oy)，
    // 不能再多减一次 art.left（否则标注会整体偏离鼠标，表现为「不跟手」）。
    // 空画板也要画：序号工具的跟手圆、框选框等是「工具预览」，不属于已提交的形状。
    if (annot_ && (annot_->hasAnyShapes() || annot_->annotating())) {
        rt->SetTransform(D2D1::Matrix3x2F::Scale(s, s)
            * D2D1::Matrix3x2F::Translation(ox, oy));
        annot_->paintOnScreen(rt);
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
    }

    paintSelection(rt, ox, oy, s);

    // 选中对象上方的快捷变换控件（左旋/垂直镜像/水平镜像/右旋）
    if (!exporting_) paintQuickActions(rt);

    // 悬停容器：未选中时给一圈很淡的主题色取景框（任何工具状态下都提示可点选，和标注一致）
    if (!exporting_ && hoverCell_ >= 0
        && hoverCell_ < (int)cells_.size() && !isCellSelected(hoverCell_)) {
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        auto hb = boundsOf(cells_[hoverCell_].poly);
        const int hg = layoutGroupOf(hoverCell_);
        if (hg >= 0) {
            hb = layoutGroupBounds(hg);
        }
        else {
            // 分割组按整体停悬：淡绿取景框框住整个原容器，而不是只框单个分割块
            const int sg = cells_[hoverCell_].group;
            auto it = (sg >= 0) ? splitGroups_.find(sg) : splitGroups_.end();
            if (it != splitGroups_.end()) hb = boundsOf(it->second.original);
        }
        const float d = (win && win->dpi > 0.f) ? win->dpi : 1.f;
        if (auto brush = solidBrush(rt, 0x34C75966)) {
            rt->DrawRectangle(D2D1::RectF(
                ox + (hb.left - art.left) * s, oy + (hb.top - art.top) * s,
                ox + (hb.right - art.left) * s, oy + (hb.bottom - art.top) * s),
                brush.Get(), 1.f * d);
        }
    }

    // 吸附参考线：拖动/缩放/图片对齐时，命中的对齐边画一条贯穿画布的细线
    if (!snapGuides_.empty() && !exporting_) {
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        const float d = (win && win->dpi > 0.f) ? win->dpi : 1.f;
        auto brush = solidBrush(rt, ImageEditTheme::snapGuide);
        if (brush) {
            for (const auto& g : snapGuides_) {
                if (g.vertical) {
                    const float gx = ox + (g.pos - art.left) * s;
                    rt->DrawLine(D2D1::Point2F(gx, 0.f), D2D1::Point2F(gx, h), brush.Get(), 1.f * d);
                }
                else {
                    const float gy = oy + (g.pos - art.top) * s;
                    rt->DrawLine(D2D1::Point2F(0.f, gy), D2D1::Point2F(w, gy), brush.Get(), 1.f * d);
                }
            }
        }
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
    }

    // 橡皮筋框选
    if (marqueeSelecting_) {
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        auto p0 = screenPointOf(marqueeStart_);
        auto p1 = screenPointOf(marqueeCur_);
        const float dpi = (win && win->dpi > 0.f) ? win->dpi : 1.f;
        D2D1_RECT_F mr{ std::min(p0.x, p1.x), std::min(p0.y, p1.y),
                        std::max(p0.x, p1.x), std::max(p0.y, p1.y) };
        if (auto b = solidBrush(rt, ImageEditTheme::fg)) rt->DrawRectangle(mr, b.Get(), 1.f * dpi);
    }

    rt->SetTransform(D2D1::Matrix3x2F::Identity());

    // 分割 / 布局拖拽预览
    if (dragging_ && !exporting_ && tool_ != Tool::None) {
        Microsoft::WRL::ComPtr<ID2D1StrokeStyle> dash;
        D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
            D2D1_LINE_JOIN_MITER, 10.f, D2D1_DASH_STYLE_DASH, 0.f);
        if (auto* f = d2dFactory()) f->CreateStrokeStyle(&sp, nullptr, 0, &dash);
        const float d = (win && win->dpi > 0.f) ? win->dpi : 1.f;
        if (tool_ == Tool::Split) {
            // 和线段工具一样：绿色线段就是用户画的那条线；
            // 只有当它与容器边缘有两个交点（穿过整个容器）时，才在交点处提示分割控点
            if (auto green = solidBrush(rt, ImageEditTheme::accent)) {
                rt->DrawLine(screenPointOf(dragStart_), screenPointOf(dragCur_),
                    green.Get(), 1.5f * d);
                const int target = splitTargetForSegment(dragStart_, dragCur_);
                Pt ca, cb;
                if (target >= 0 && splitSegmentForCell(target, dragStart_, dragCur_, ca, cb)) {
                    const auto sa = screenPointOf(ca);
                    const auto sb = screenPointOf(cb);
                    paintHandleDot(rt, sa.x, sa.y, d);
                    paintHandleDot(rt, sb.x, sb.y, d);
                }
            }
        }
        else if (auto brush = solidBrush(rt, ImageEditTheme::accent)) {
            const float st = 1.f * d;
            if (tool_ == Tool::Layout && drawingNewLayout_) {
                float l = std::min(dragStart_.x, dragCur_.x);
                float t = std::min(dragStart_.y, dragCur_.y);
                float r = std::max(dragStart_.x, dragCur_.x);
                float b = std::max(dragStart_.y, dragCur_.y);
                D2D1_RECT_F rc{ ox + (l - art.left) * s, oy + (t - art.top) * s,
                                ox + (r - art.left) * s, oy + (b - art.top) * s };
                rt->DrawRectangle(rc, brush.Get(), st, dash.Get());
                for (int c = 1; c < cols_; ++c) {
                    float cx = rc.left + (rc.right - rc.left) * c / cols_;
                    rt->DrawLine(D2D1::Point2F(cx, rc.top), D2D1::Point2F(cx, rc.bottom), brush.Get(), st, dash.Get());
                }
                for (int rr = 1; rr < rows_; ++rr) {
                    float cy = rc.top + (rc.bottom - rc.top) * rr / rows_;
                    rt->DrawLine(D2D1::Point2F(rc.left, cy), D2D1::Point2F(rc.right, cy), brush.Get(), st, dash.Get());
                }
                // 拖拽布局时每个格子都提示可以拖入/选择图片
                const int nc = std::max(1, cols_);
                const int nr = std::max(1, rows_);
                for (int r2 = 0; r2 < nr; ++r2) {
                    for (int c2 = 0; c2 < nc; ++c2) {
                        const D2D1_RECT_F cell{
                            rc.left + (rc.right - rc.left) * (float)c2 / (float)nc,
                            rc.top + (rc.bottom - rc.top) * (float)r2 / (float)nr,
                            rc.left + (rc.right - rc.left) * (float)(c2 + 1) / (float)nc,
                            rc.top + (rc.bottom - rc.top) * (float)(r2 + 1) / (float)nr };
                        paintEmptyCellHint(rt, cell, d);
                    }
                }
            }
        }
    }

    // 内容区左上角：缩放比例胶囊（样式复用贴图左下角的缩放提示）
    if (!exporting_) {
        const float d = (win && win->dpi > 0.f) ? win->dpi : 1.f;
        auto* d2d = D2D::get();
        if (d2d) {
            auto bg = solidBrush(rt, 0x00000075u);
            auto fg = solidBrush(rt, 0xFFFFFFFFu);
            auto layout = d2d->makeTextLayout(zoomText(), 12.f * d, FLT_MAX, FLT_MAX);
            if (bg && fg && layout) {
                DWRITE_TEXT_METRICS tm{};
                layout->GetMetrics(&tm);
                const float padX = 7.f * d;
                const float padY = 3.f * d;
                const float margin = 12.f * d;
                const float tipW = tm.width + padX * 2.f;
                const float tipH = tm.height + padY * 2.f;
                const D2D1_RECT_F bgRc{ margin, margin, margin + tipW, margin + tipH };
                rt->FillRoundedRectangle(D2D1::RoundedRect(bgRc, tipH * 0.5f, tipH * 0.5f), bg.Get());
                rt->DrawTextLayout(D2D1::Point2F(bgRc.left + padX, bgRc.top + padY),
                    layout.Get(), fg.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            }
        }
    }
}

// ---------------- 导出 ----------------

bool ImageEditCollage::exportPixels(std::vector<BYTE>& pixels, int& width, int& height)
{
    auto art = artRect();
    width = (int)std::lround(art.right - art.left);
    height = (int)std::lround(art.bottom - art.top);
    if (width <= 0 || height <= 0) return false;

    auto wic = wicFactory();
    auto d2d = D2D::getNoDevice();
    if (!wic || !d2d || !d2d->d2dFactory) return false;

    Microsoft::WRL::ComPtr<IWICBitmap> bmp;
    if (FAILED(wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA,
        WICBitmapCacheOnLoad, &bmp))) return false;

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties();
    props.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    props.dpiX = 96.f;
    props.dpiY = 96.f;

    Microsoft::WRL::ComPtr<ID2D1RenderTarget> rt;
    if (FAILED(d2d->d2dFactory->CreateWicBitmapRenderTarget(bmp.Get(), props, &rt))) return false;

    // 导出用的位图缓存与屏幕缓存分开，避免设备不兼容
    auto screenCache = bitmapCache_;
    auto screenSizes = bitmapSize_;
    auto screenRT = bitmapRT_;
    bitmapCache_.clear();
    bitmapSize_.clear();
    exporting_ = true;

    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0.f, 0.f, 0.f, 0.f));

    // 画板底色
    if (auto bgBrush = solidBrush(rt.Get(), bgColor_))
        rt->FillRectangle(D2D1::RectF(0.f, 0.f, (float)width, (float)height), bgBrush.Get());

    paintWorld(rt.Get(), -art.left, -art.top, 1.f, false);

    HRESULT hr = rt->EndDraw();
    exporting_ = false;

    // 释放导出用的位图，还原屏幕缓存（渲染目标指针也要还原：
    // 否则下一次屏幕绘制会认为「设备换了」而清空缓存、重新解码所有图片，
    // 拖动分割线时下方图片格子就会反复重建、闪烁）
    for (auto& [k, v] : bitmapCache_)
        if (v) v->Release();
    bitmapCache_ = screenCache;
    bitmapSize_ = screenSizes;
    bitmapRT_ = screenRT;

    if (FAILED(hr)) return false;

    Microsoft::WRL::ComPtr<IWICBitmapLock> lock;
    WICRect rc{ 0, 0, width, height };
    if (FAILED(bmp->Lock(&rc, WICBitmapLockRead, &lock))) return false;
    UINT stride = 0;
    BYTE* data = nullptr;
    UINT size = 0;
    if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&size, &data))) return false;

    pixels.resize((size_t)width * height * 4);
    for (int y = 0; y < height; ++y)
        memcpy(pixels.data() + (size_t)y * width * 4, data + (size_t)y * stride, (size_t)width * 4);
    return true;
}
