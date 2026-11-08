#include "pch.h"
#include "ImageEditMerge.h"
#include "ImageEditTheme.h"
#include "../Util.h"
#include <wincodec.h>
#include <shobjidl.h>
#include <algorithm>
#include <cmath>

using namespace Ling;

namespace {
    // 工作区卡片网格：宽 130 / 高 160，列间距 36、行间距 54，四周留 29
    constexpr float kCardW{ 130.f };
    constexpr float kCardH{ 160.f };
    constexpr float kCardStepX{ kCardW + 36.f };
    constexpr float kCardGapY{ 54.f };
    constexpr float kGridPad{ 29.f };
    constexpr float kTitleGap{ 9.f };    // 图片与文件名之间的间距
    constexpr float kTitleH{ 20.f };

    Microsoft::WRL::ComPtr<IWICImagingFactory> wicFactory()
    {
        static Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
        if (!factory)
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
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

    // ID2D1Resource::GetFactory 是带出参的版本（GetFactory(&f)），这里统一取进程内的 D2D 工厂
    ID2D1Factory* d2dFactory()
    {
        auto* d2d = D2D::get();
        return d2d ? d2d->d2dFactory.Get() : nullptr;
    }

    std::vector<std::wstring> pickImages(HWND owner)
    {
        std::vector<std::wstring> out;
        Microsoft::WRL::ComPtr<IFileOpenDialog> dlg;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&dlg)))) return out;

        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
        const COMDLG_FILTERSPEC filters[]{
            { L"图片文件", L"*.png;*.jpg;*.jpeg;*.webp;*.bmp;*.gif;*.tif;*.tiff" },
            { L"所有文件", L"*.*" },
        };
        dlg->SetFileTypes(2, filters);
        if (FAILED(dlg->Show(owner))) return out;

        Microsoft::WRL::ComPtr<IShellItemArray> arr;
        if (FAILED(dlg->GetResults(&arr))) return out;
        DWORD count = 0;
        arr->GetCount(&count);
        for (DWORD i = 0; i < count; ++i) {
            Microsoft::WRL::ComPtr<IShellItem> item;
            if (FAILED(arr->GetItemAt(i, &item))) continue;
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                out.emplace_back(path);
                CoTaskMemFree(path);
            }
        }
        return out;
    }

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> solidBrush(ID2D1RenderTarget* rt, uint32_t rgba)
    {
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
        if (rt && SUCCEEDED(rt->CreateSolidColorBrush(Color(rgba).getD2DColor(), &brush))) return brush;
        return nullptr;
    }

    // 文件名中间省略：保留开头 + 扩展名（后缀），中间用「…」代替，宽度按当前字号实测
    std::wstring elideMiddle(const std::wstring& text, float fontSize, float maxW)
    {
        if (maxW <= 0.f || text.empty()) return text;
        auto widthOf = [fontSize, maxW](const std::wstring& s) {
            auto layout = D2D::makeTextLayout(s, fontSize);
            DWRITE_TEXT_METRICS m{};
            if (!layout || FAILED(layout->GetMetrics(&m))) return maxW;
            return m.width;
        };
        if (widthOf(text) <= maxW) return text;

        const auto dot = text.find_last_of(L'.');
        const bool hasExt = dot != std::wstring::npos && dot > 0 && dot + 1 < text.size();
        const std::wstring suffix = hasExt ? text.substr(dot) : L"";
        const std::wstring stem = hasExt ? text.substr(0, dot) : text;
        const std::wstring ell = L"…";
        // 二分找能放下的最长前缀
        std::wstring best = ell + suffix;
        size_t lo = 0, hi = stem.size();
        while (lo <= hi) {
            const size_t mid = lo + (hi - lo) / 2;
            std::wstring cand = stem.substr(0, mid) + ell + suffix;
            if (widthOf(cand) <= maxW) {
                best = cand;
                if (mid == stem.size()) break;
                lo = mid + 1;
            }
            else {
                if (mid == 0) break;
                hi = mid - 1;
            }
        }
        return best;
    }
}

ImageEditMerge::ImageEditMerge(WinBase* win) :Canvas(win)
{
    // 与拼图画布一致：拖拽时高频整帧重绘，需双缓冲避免合成器采样到中间态
    enableSwapChain();
    setWidthPercent(100.f);
    setHeightPercent(100.f);
    setFlexGrow(1.f);

    tokDown_ = win->onMouseDown.add([this](POINT pos, bool right) { onMouseDown(pos, right); });
    tokMove_ = win->onMouseMove.add([this](POINT pos) { onMouseMove(pos); });
    tokUp_ = win->onMouseUp.add([this](POINT pos, bool right) { onMouseUp(pos, right); });
    tokWheel_ = win->onMouseWheel.add([this](POINT pos, float space) { onMouseWheel(pos, space); });
}

ImageEditMerge::~ImageEditMerge()
{
    if (win) {
        win->onMouseDown.remove(tokDown_);
        win->onMouseMove.remove(tokMove_);
        win->onMouseUp.remove(tokUp_);
        win->onMouseWheel.remove(tokWheel_);
    }
    for (auto& [k, b] : bitmapCache_)
        if (b) b->Release();
    bitmapCache_.clear();
}

void ImageEditMerge::layout()
{
    Canvas::layout();
    if (w <= 0.f || h <= 0.f) return;
    auto ctx = startPaint();
    if (!ctx) return;
    ctx->Clear(Color(0x00000000).getD2DColor());
    paintScene(ctx);
    finishPaint();
}

bool ImageEditMerge::addImagesDialog()
{
    if (!win) return false;
    auto paths = pickImages(win->hwnd);
    if (paths.empty()) return false;
    addPaths(paths);
    return true;
}

void ImageEditMerge::addPaths(const std::vector<std::wstring>& paths)
{
    std::vector<std::wstring> imgs;
    for (const auto& p : paths)
        if (hasImageExt(p)) imgs.push_back(p);
    if (imgs.empty()) return;

    for (const auto& p : imgs) {
        auto sz = ::Util::imageSize(p);
        if (sz.first <= 0 || sz.second <= 0) continue;
        items_.push_back({ p, sz.first, sz.second });
    }
    notifyChanged();
    if (win) win->refresh();
}

void ImageEditMerge::handleDrop(const std::vector<std::wstring>& paths, POINT)
{
    addPaths(paths);
}

void ImageEditMerge::clear()
{
    items_.clear();
    border_ = 0.f;
    bgColor_ = 0xFFFFFFFF;
    pageW_ = pageH_ = 0;
    scrollY_ = 0.f;
    dragging_ = false;
    dragMoved_ = false;
    dragFrom_ = dragTo_ = -1;
    selected_.clear();
    marquee_ = false;
    notifyChanged();
    if (win) win->refresh();
}

void ImageEditMerge::setBorder(float value)
{
    border_ = std::max(0.f, std::min(400.f, value));
    if (win) win->refresh();
}

void ImageEditMerge::setBackgroundColor(uint32_t rgba)
{
    bgColor_ = rgba;
    if (win) win->refresh();
}

void ImageEditMerge::setPageSize(int w, int h)
{
    pageW_ = std::max(0, std::min(10000, w));
    pageH_ = std::max(0, std::min(10000, h));
    scrollY_ = 0.f;   // 卡片高度变了，滚动位置重新从顶部开始
    if (win) win->refresh();
}

void ImageEditMerge::setFormat(int format)
{
    format_ = format == 1 ? 1 : 0;
}

void ImageEditMerge::notifyChanged()
{
    if (onChanged) onChanged();
}

// ---------------- 卡片布局 ----------------

int ImageEditMerge::columns() const
{
    // 列数随画布宽度自适应：左右留 29、卡片 130、列距 166（间距 36）
    const float d = win ? win->dpi : 1.f;
    const float pad = kGridPad * d;
    const float cardW = kCardW * d;
    const float stepX = kCardStepX * d;
    const float avail = (float)w - pad * 2.f;
    if (avail <= cardW) return 1;
    return std::max(1, (int)((avail + (stepX - cardW)) / stepX));
}

float ImageEditMerge::cardPreviewHeight() const
{
    // 设了页宽高：预览按页面比例缩放（宽固定 130，1920×1080 → 130×73）
    if (pageW_ > 0 && pageH_ > 0)
        return std::clamp(kCardW * (float)pageH_ / (float)pageW_, 40.f, 400.f);
    return kCardH;
}

D2D1_RECT_F ImageEditMerge::cardRect(int index) const
{
    float dpi = win ? win->dpi : 1.f;
    float pad = kGridPad * dpi;
    float cardW = kCardW * dpi;
    float cardH = cardPreviewHeight() * dpi;
    float stepX = kCardStepX * dpi;
    float stepY = (cardPreviewHeight() + kCardGapY) * dpi;
    const int cols = columns();
    int col = index % cols;
    int row = index / cols;
    float l = pad + col * stepX;
    float t = pad + row * stepY - scrollY_;
    return D2D1::RectF(l, t, l + cardW, t + cardH);
}

int ImageEditMerge::indexAt(POINT clientPos)
{
    if (!isPosIn(clientPos)) return -1;
    float dpi = win ? win->dpi : 1.f;
    float lx = (float)clientPos.x - x;
    float ly = (float)clientPos.y - y + scrollY_;
    float pad = kGridPad * dpi;
    if (lx < pad || ly < pad) return -1;
    const int cols = columns();
    int col = (int)((lx - pad) / (kCardStepX * dpi));
    int row = (int)((ly - pad) / ((cardPreviewHeight() + kCardGapY) * dpi));
    if (col < 0 || col >= cols) return -1;
    int idx = row * cols + col;
    if (idx < 0) return 0;
    if (idx > (int)items_.size()) return (int)items_.size();
    return idx;
}

bool ImageEditMerge::isSelected(int index) const
{
    return std::find(selected_.begin(), selected_.end(), index) != selected_.end();
}

bool ImageEditMerge::deleteSelectedCards()
{
    if (selected_.empty() || items_.empty()) return false;
    std::vector<int> idx = selected_;
    std::sort(idx.begin(), idx.end());
    idx.erase(std::unique(idx.begin(), idx.end()), idx.end());
    // 从后往前删，避免前面的下标失效
    for (auto it = idx.rbegin(); it != idx.rend(); ++it) {
        if (*it >= 0 && *it < (int)items_.size())
            items_.erase(items_.begin() + *it);
    }
    selected_.clear();
    dragging_ = false;
    dragMoved_ = false;
    dragFrom_ = dragTo_ = -1;
    notifyChanged();
    if (win) win->refresh();
    return true;
}

// ---------------- 绘制 ----------------

ID2D1Bitmap* ImageEditMerge::bitmapFor(ID2D1RenderTarget* rt, const std::wstring& path, int& imgW, int& imgH)
{
    auto it = bitmapCache_.find(path);
    if (it != bitmapCache_.end()) {
        auto sz = bitmapSize_[path];
        imgW = sz.first;
        imgH = sz.second;
        return it->second;
    }
    auto wic = wicFactory();
    if (!wic || path.empty()) return nullptr;

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
    conv->GetSize(&ww, &hh);
    imgW = (int)ww;
    imgH = (int)hh;

    ID2D1Bitmap* bmp = nullptr;
    if (FAILED(rt->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &bmp)) ) return nullptr;
    bitmapCache_[path] = bmp;
    bitmapSize_[path] = { imgW, imgH };
    return bmp;
}

void ImageEditMerge::paintScene(ID2D1RenderTarget* rt)
{
    if (!rt) return;
    float dpi = win ? win->dpi : 1.f;

    auto cardBrush = solidBrush(rt, ImageEditTheme::cardBg);
    auto textBrush = solidBrush(rt, ImageEditTheme::cardText);
    auto accentBrush = solidBrush(rt, ImageEditTheme::accent);

    int total = (int)items_.size();
    for (int i = 0; i < total; ++i) {
        auto rc = cardRect(i);
        if (rc.bottom < 0.f || rc.top > h) continue;
        float radius = 20.f * dpi;

        Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> geo;
        if (auto* f = d2dFactory())
            f->CreateRoundedRectangleGeometry(D2D1::RoundedRect(rc, radius, radius), &geo);
        if (geo && cardBrush) rt->FillGeometry(geo.Get(), cardBrush.Get());

        int iw = 0, ih = 0;
        auto bmp = bitmapFor(rt, items_[i].path, iw, ih);
        if (bmp && iw > 0 && ih > 0) {
            auto layer = Microsoft::WRL::ComPtr<ID2D1Layer>();
            rt->CreateLayer(nullptr, &layer);
            rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), geo.Get()), layer.Get());
            float cw = rc.right - rc.left, ch = rc.bottom - rc.top;
            // 等比完整显示（不裁剪填满）；设了页宽高时按导出规则先扣掉外边框
            float inner = 0.f;
            if (pageW_ > 0 && pageH_ > 0) inner = border_ * (cw / (float)pageW_);
            const float availW = std::max(1.f, cw - inner * 2.f);
            const float availH = std::max(1.f, ch - inner * 2.f);
            float s = std::min(availW / iw, availH / ih);
            float dw = iw * s, dh = ih * s;
            auto dest = D2D1::RectF(rc.left + (cw - dw) * 0.5f, rc.top + (ch - dh) * 0.5f,
                rc.left + (cw + dw) * 0.5f, rc.top + (ch + dh) * 0.5f);
            rt->DrawBitmap(bmp, dest, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            rt->PopLayer();
        }

        // 文件名：中间省略、保留后缀（扩展名）
        auto slash = items_[i].path.find_last_of(L"\\/");
        std::wstring name = slash == std::wstring::npos ? items_[i].path : items_[i].path.substr(slash + 1);
        const float titleW = rc.right - rc.left;
        const float titleTop = rc.bottom + kTitleGap * dpi;
        auto layoutText = D2D::makeTextLayout(elideMiddle(name, 12.f * dpi, titleW),
            12.f * dpi, titleW, kTitleH * dpi);
        if (layoutText && textBrush) {
            layoutText->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            auto clip = D2D1::RectF(rc.left, titleTop, rc.right, titleTop + kTitleH * dpi);
            rt->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            rt->DrawTextLayout(D2D1::Point2F(rc.left, titleTop),
                layoutText.Get(), textBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            rt->PopAxisAlignedClip();
        }

        // 选中：绿色边框（支持框选多张）
        if (isSelected(i) && accentBrush && geo)
            rt->DrawGeometry(geo.Get(), accentBrush.Get(), 2.f * dpi);
    }

    // 框选矩形
    if (marquee_) {
        const float l = std::min(marqueeX0_, marqueeX1_);
        const float t = std::min(marqueeY0_, marqueeY1_);
        const float r = std::max(marqueeX0_, marqueeX1_);
        const float b = std::max(marqueeY0_, marqueeY1_);
        auto fillBrush = solidBrush(rt, 0x34C7591Fu);
        if (fillBrush) rt->FillRectangle(D2D1::RectF(l, t, r, b), fillBrush.Get());
        if (accentBrush) rt->DrawRectangle(D2D1::RectF(l, t, r, b), accentBrush.Get());
    }

    // 拖拽排序的插入提示（仅拖拽时绘制）
    if (dragMoved_ && dragTo_ >= 0) {
        const int cols = columns();
        int col = dragTo_ % cols;
        int row = dragTo_ / cols;
        float lx = kGridPad * dpi + col * kCardStepX * dpi - 8.f * dpi;
        float ly = kGridPad * dpi + row * (cardPreviewHeight() + kCardGapY) * dpi - scrollY_;
        if (accentBrush)
            rt->FillRectangle(D2D1::RectF(lx, ly, lx + 3.f * dpi, ly + cardPreviewHeight() * dpi),
                accentBrush.Get());
    }

    // 拖拽预览：被拖的图片跟随光标显示
    if (dragMoved_ && dragFrom_ >= 0 && dragFrom_ < (int)items_.size()) {
        const float cw = kCardW * dpi, ch = cardPreviewHeight() * dpi;
        auto rc = D2D1::RectF(dragX_ - cw * 0.5f, dragY_ - ch * 0.5f,
            dragX_ + cw * 0.5f, dragY_ + ch * 0.5f);
        Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> geo;
        if (auto* f = d2dFactory())
            f->CreateRoundedRectangleGeometry(D2D1::RoundedRect(rc, 20.f * dpi, 20.f * dpi), &geo);
        // 底色与图片一起走半透明图层，否则实心底色会遮住下方
        auto layer = Microsoft::WRL::ComPtr<ID2D1Layer>();
        rt->CreateLayer(nullptr, &layer);
        D2D1_LAYER_PARAMETERS lp = D2D1::LayerParameters(D2D1::InfiniteRect(),
            geo ? static_cast<ID2D1Geometry*>(geo.Get()) : nullptr,
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), 0.65f);
        if (layer) rt->PushLayer(lp, layer.Get());
        if (geo && cardBrush) rt->FillGeometry(geo.Get(), cardBrush.Get());
        int iw = 0, ih = 0;
        auto bmp = bitmapFor(rt, items_[dragFrom_].path, iw, ih);
        if (bmp && iw > 0 && ih > 0) {
            float s = std::min(cw / iw, ch / ih);
            float dw = iw * s, dh = ih * s;
            rt->DrawBitmap(bmp, D2D1::RectF(rc.left + (cw - dw) * 0.5f, rc.top + (ch - dh) * 0.5f,
                rc.left + (cw + dw) * 0.5f, rc.top + (ch + dh) * 0.5f),
                1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
        if (layer) rt->PopLayer();
        // 边框同样半透明
        auto dragBorder = solidBrush(rt, 0x34C759AAu);
        if (geo && dragBorder) rt->DrawGeometry(geo.Get(), dragBorder.Get(), 2.f * dpi);
    }
}

// ---------------- 鼠标 ----------------

void ImageEditMerge::onMouseDown(POINT pos, bool right)
{
    if (right || !isPosIn(pos)) return;
    int idx = indexAt(pos);
    bool onCard = false;
    if (idx >= 0 && idx < (int)items_.size()) {
        auto rc = cardRect(idx);
        onCard = pos.x >= rc.left && pos.x <= rc.right && pos.y >= rc.top && pos.y <= rc.bottom;
    }
    if (onCard) {
        selected_.clear();
        selected_.push_back(idx);   // 单击选中：卡片加绿色边框
        dragging_ = true;
        dragMoved_ = false;
        dragFrom_ = idx;
        dragTo_ = idx;
        dragX_ = dragStartX_ = (float)pos.x - x;
        dragY_ = dragStartY_ = (float)pos.y - y;
    }
    else {
        // 空白处按下 = 开始框选
        selected_.clear();
        marquee_ = true;
        marqueeX0_ = marqueeX1_ = (float)pos.x - x;
        marqueeY0_ = marqueeY1_ = (float)pos.y - y;
    }
    if (win) win->refresh();
}

void ImageEditMerge::onMouseMove(POINT pos)
{
    if (marquee_) {
        marqueeX1_ = (float)pos.x - x;
        marqueeY1_ = (float)pos.y - y;
        const float l = std::min(marqueeX0_, marqueeX1_);
        const float t = std::min(marqueeY0_, marqueeY1_);
        const float r = std::max(marqueeX0_, marqueeX1_);
        const float b = std::max(marqueeY0_, marqueeY1_);
        selected_.clear();
        for (int i = 0; i < (int)items_.size(); ++i) {
            auto rc = cardRect(i);
            if (rc.right < l || rc.left > r || rc.bottom < t || rc.top > b) continue;
            selected_.push_back(i);
        }
        if (win) win->refresh();
        return;
    }
    if (!dragging_) return;
    dragX_ = (float)pos.x - x;
    dragY_ = (float)pos.y - y;
    if (!dragMoved_) {
        // 超过 4px 才进入拖拽，区分单击与拖动
        const float th = 4.f * (win ? win->dpi : 1.f);
        if (std::abs(dragX_ - dragStartX_) < th && std::abs(dragY_ - dragStartY_) < th) return;
        dragMoved_ = true;
    }
    int idx = indexAt(pos);
    if (idx >= 0) dragTo_ = idx;
    if (win) win->refresh();
}

void ImageEditMerge::onMouseUp(POINT pos, bool right)
{
    if (right) return;
    if (marquee_) {
        marquee_ = false;
        if (win) win->refresh();
        return;
    }
    if (!dragging_) return;
    dragging_ = false;
    if (dragMoved_ && dragFrom_ >= 0 && dragTo_ >= 0 && dragFrom_ != dragTo_
        && dragFrom_ < (int)items_.size() && dragTo_ <= (int)items_.size()) {
        Item it = items_[dragFrom_];
        items_.erase(items_.begin() + dragFrom_);
        int to = dragTo_;
        if (to > dragFrom_) --to;
        to = std::max(0, std::min((int)items_.size(), to));
        items_.insert(items_.begin() + to, it);
        selected_.clear();
        selected_.push_back(to);
        notifyChanged();
    }
    dragFrom_ = dragTo_ = -1;
    dragMoved_ = false;
    if (win) win->refresh();
}

void ImageEditMerge::onMouseWheel(POINT pos, float space)
{
    if (!isPosIn(pos) || space == 0.f) return;
    float dpi = win ? win->dpi : 1.f;
    const int cols = columns();
    int rows = ((int)items_.size() + cols - 1) / cols;
    float contentH = kGridPad * dpi + rows * (cardPreviewHeight() + kCardGapY) * dpi;
    float maxScroll = std::max(0.f, contentH - h);
    scrollY_ = std::max(0.f, std::min(maxScroll, scrollY_ - space * 60.f * dpi));
    if (win) win->refresh();
}

// ---------------- 页面导出 ----------------

bool ImageEditMerge::renderPage(int index, std::vector<BYTE>& pixels, int& width, int& height)
{
    if (index < 0 || index >= (int)items_.size()) return false;
    const auto& it = items_[index];

    int pw = pageW_ > 0 ? pageW_ : it.w;
    int ph = pageH_ > 0 ? pageH_ : it.h;
    pw = std::max(1, std::min(10000, pw));
    ph = std::max(1, std::min(10000, ph));
    width = pw;
    height = ph;

    auto wic = wicFactory();
    auto d2d = D2D::getNoDevice();
    if (!wic || !d2d || !d2d->d2dFactory) return false;

    Microsoft::WRL::ComPtr<IWICBitmap> wbmp;
    if (FAILED(wic->CreateBitmap(pw, ph, GUID_WICPixelFormat32bppPBGRA,
        WICBitmapCacheOnLoad, &wbmp))) return false;

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties();
    props.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    props.dpiX = 96.f;
    props.dpiY = 96.f;

    Microsoft::WRL::ComPtr<ID2D1RenderTarget> rt;
    if (FAILED(d2d->d2dFactory->CreateWicBitmapRenderTarget(wbmp.Get(), props, &rt))) return false;

    auto solid = solidBrush(rt.Get(), bgColor_);
    auto createBmp = [&](const std::wstring& path, int& iw, int& ih) -> Microsoft::WRL::ComPtr<ID2D1Bitmap> {
        Microsoft::WRL::ComPtr<ID2D1Bitmap> out;
        auto [w0, h0, data] = ::Util::loadImagePixels(path);
        if (w0 <= 0 || h0 <= 0 || data.empty()) return out;
        iw = w0; ih = h0;
        Microsoft::WRL::ComPtr<IWICBitmap> src;
        if (FAILED(wic->CreateBitmapFromMemory(w0, h0, GUID_WICPixelFormat32bppPBGRA,
            (UINT)((size_t)w0 * 4), (UINT)((size_t)w0 * 4 * h0),
            (BYTE*)data.data(), &src))) return out;
        rt->CreateBitmapFromWicBitmap(src.Get(), nullptr, &out);
        return out;
    };

    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0.f, 0.f, 0.f, 0.f));

    if (solid) rt->FillRectangle(D2D1::RectF(0.f, 0.f, (float)pw, (float)ph), solid.Get());

    int iw = 0, ih = 0;
    auto bmp = createBmp(it.path, iw, ih);
    if (bmp && iw > 0 && ih > 0) {
        float pad = border_;
        float availW = std::max(1.f, pw - pad * 2.f);
        float availH = std::max(1.f, ph - pad * 2.f);
        float s = std::min(availW / iw, availH / ih);
        if (pageW_ == 0 && pageH_ == 0) s = std::min(1.f, s);
        float dw = iw * s, dh = ih * s;
        auto dest = D2D1::RectF(pad + (availW - dw) * 0.5f, pad + (availH - dh) * 0.5f,
            pad + (availW + dw) * 0.5f, pad + (availH + dh) * 0.5f);
        rt->DrawBitmap(bmp.Get(), dest, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

    HRESULT hr = rt->EndDraw();
    if (FAILED(hr)) return false;

    Microsoft::WRL::ComPtr<IWICBitmapLock> lock;
    WICRect rc{ 0, 0, pw, ph };
    if (FAILED(wbmp->Lock(&rc, WICBitmapLockRead, &lock))) return false;
    UINT stride = 0;
    BYTE* data = nullptr;
    UINT size = 0;
    if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&size, &data))) return false;

    pixels.resize((size_t)pw * ph * 4);
    for (int y = 0; y < ph; ++y)
        memcpy(pixels.data() + (size_t)y * pw * 4, data + (size_t)y * stride, (size_t)pw * 4);
    return true;
}
