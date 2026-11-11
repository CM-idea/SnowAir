#pragma once
/**
 * 光标位图 → straight BGRA（alpha 保留）。
 *
 * 为什么不用 DrawIconEx 直接往 32bpp DIB 上画：
 *   · 单色光标（工字 I 型、沙漏、部分手型）画出来 alpha 全是 0 —— 之前只能靠掩码去"猜"，
 *     猜不中就整个光标消失，这正是「某些情况下不显示光标」的主因；
 *   · 彩色光标自带 alpha 时，DrawIconEx 是往不透明底上合成，alpha 也丢了。
 * 这里直接从 ICONINFO 的两张位图取数：
 *   · 32bpp 带 alpha 的彩色光标 —— 原样用；
 *   · 彩色但没 alpha（24bpp）—— alpha 由 AND 掩码补；
 *   · 单色 —— AND=1 保留屏幕（透明），AND=0 时 XOR=1 画白、XOR=0 画黑。
 * 纯 Win32，可被 --selftest 直接调用（LoadCursor(IDC_IBEAM) 等系统光标）。
 */

#include <algorithm>
#include <cstring>
#include <vector>
#include <windows.h>

namespace CursorImage {

// 光标位图尺寸：优先彩色位图，否则掩码高度的一半（掩码是 AND/XOR 两半叠起来的）
inline bool ChooseSize(const ICONINFO& ii, int* width, int* height)
{
    if (!width || !height) return false;
    BITMAP bm{};
    if (ii.hbmColor && GetObjectW(ii.hbmColor, sizeof(bm), &bm) && bm.bmWidth > 0 && bm.bmHeight > 0) {
        *width = bm.bmWidth;
        *height = bm.bmHeight;
    }
    else if (ii.hbmMask && GetObjectW(ii.hbmMask, sizeof(bm), &bm) && bm.bmWidth > 0 && bm.bmHeight > 1) {
        *width = bm.bmWidth;
        *height = bm.bmHeight / 2;
    }
    else {
        return false;
    }
    *width = std::clamp(*width, 1, 256);
    *height = std::clamp(*height, 1, 256);
    return true;
}

// 读 1bpp 掩码，按位展开成 w*h（1 = 掩码位为 1）。
// rowOffset：掩码里从第几行开始取（单色光标 AND=0、XOR=h；彩色光标 AND=0）。
inline bool ReadMask(HDC dc, HBITMAP mask, int w, int h, int rowOffset, std::vector<BYTE>& bits)
{
    if (!mask || w <= 0 || h <= 0) return false;
    BITMAP bm{};
    if (!GetObjectW(mask, sizeof(bm), &bm) || bm.bmWidth < w || bm.bmHeight < rowOffset + h) return false;

    const int stride = ((bm.bmWidth + 31) / 32) * 4;
    std::vector<BYTE> buf((size_t)stride * bm.bmHeight, 0);
    BITMAPINFO mi{};
    mi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    mi.bmiHeader.biWidth = bm.bmWidth;
    mi.bmiHeader.biHeight = bm.bmHeight;   // bottom-up
    mi.bmiHeader.biPlanes = 1;
    mi.bmiHeader.biBitCount = 1;
    mi.bmiHeader.biCompression = BI_RGB;
    if (GetDIBits(dc, mask, 0, bm.bmHeight, buf.data(), &mi, DIB_RGB_COLORS) == 0) return false;

    bits.assign((size_t)w * h, 0);
    for (int y = 0; y < h; ++y) {
        const int bufRow = bm.bmHeight - 1 - (rowOffset + y);
        if (bufRow < 0 || bufRow >= bm.bmHeight) continue;
        const BYTE* row = buf.data() + (size_t)bufRow * stride;
        for (int x = 0; x < w; ++x)
            bits[(size_t)y * w + x] = (row[x >> 3] >> (7 - (x & 7))) & 1;
    }
    return true;
}

// 成功时 out 是 w*h 的 BGRA（非预乘）；返回 true 表示至少有一个不透明像素。
// xorMask（可选）：单色光标里"把背景反色"画出来的像素（AND=1、XOR=1）会被标 1，
//   调用方拿真实背景做反色才能画出工字光标的本色（白底上是黑线、黑底上是白线）；
//   不处理时这些像素按白色给出，至少不会是透明的。
inline bool Extract(HDC dc, const ICONINFO& ii, int w, int h, std::vector<BYTE>& out,
    std::vector<BYTE>* xorMask = nullptr)
{
    if (!dc || w <= 0 || h <= 0) return false;
    out.assign((size_t)w * h * 4, 0);
    if (xorMask) xorMask->clear();

    if (ii.hbmColor) {
        std::vector<BYTE> color((size_t)w * h * 4, 0);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;   // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        if (GetDIBits(dc, ii.hbmColor, 0, h, color.data(), &bi, DIB_RGB_COLORS) == 0) return false;

        bool anyAlpha = false;
        for (size_t i = 3; i < color.size(); i += 4)
            if (color[i]) { anyAlpha = true; break; }

        if (!anyAlpha) {
            // 24bpp / 没有 alpha 的彩色光标：透明与否全看 AND 掩码
            std::vector<BYTE> andBits;
            const bool haveMask = ReadMask(dc, ii.hbmMask, w, h, 0, andBits);
            for (int i = 0; i < w * h; ++i) {
                BYTE* p = color.data() + (size_t)i * 4;
                if (haveMask && andBits[i]) { p[0] = p[1] = p[2] = p[3] = 0; }
                else p[3] = 255;   // 掩码读不到时宁可画成实心，也别整个消失
            }
        }
        out.swap(color);
    }
    else {
        // 单色光标：掩码上半 = AND、下半 = XOR，实际合成是 dest = (dest AND andMask) XOR xorMask
        //   ① AND=1、XOR=0：dest 原样保留 → 透明
        //   ② AND=1、XOR=1：dest 取反 → 靠背景反色画出来（工字光标就是这种）
        //   ③ AND=0：dest 先清零再 XOR → XOR=1 白、XOR=0 黑
        std::vector<BYTE> andBits, xorBits;
        if (!ReadMask(dc, ii.hbmMask, w, h, 0, andBits)) return false;
        if (!ReadMask(dc, ii.hbmMask, w, h, h, xorBits)) return false;
        if (xorMask) xorMask->assign((size_t)w * h, 0);
        for (int i = 0; i < w * h; ++i) {
            BYTE* p = out.data() + (size_t)i * 4;
            if (andBits[i]) {
                if (!xorBits[i]) continue;          // 透明
                if (xorMask) (*xorMask)[(size_t)i] = 1;
                p[0] = p[1] = p[2] = p[3] = 255;    // 先给白色，调用方可用背景反色覆盖
            }
            else {
                const BYTE v = xorBits[i] ? 255 : 0;
                p[0] = p[1] = p[2] = v;
                p[3] = 255;
            }
        }
    }

    for (size_t i = 3; i < out.size(); i += 4)
        if (out[i]) return true;   // 至少有一个不透明像素才算取到光标
    return false;
}

} // namespace CursorImage
