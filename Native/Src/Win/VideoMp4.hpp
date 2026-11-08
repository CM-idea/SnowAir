#pragma once

#include "pch.h"
#include <wininet.h>
#include <process.h>
#include <commctrl.h>
#include <stdio.h>
#include <mutex>
#include <queue>
#include <sstream>
#include <thread>
#include <tchar.h>
#include <memory>
#include <functional>
#include "../compat/CComPtr.h"
#include <codecapi.h>
#include <mfidl.h>
#include <mfapi.h>
#include <mfreadwrite.h>
#include <dshow.h>
#include <effects.h>
#include <Wmcodecdsp.h>
#include <setjmp.h>
#include <Mferror.h>
#include <optional>
#include <d2d1_2.h>
#include <d2d1_3.h>
#include <dxgi.h>
#include <wincodec.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>
#include <d3d11.h>
#include <shlobj.h>
#include <propvarutil.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <chrono>
#include <audiosessiontypes.h>
#include <atomic>
#include <array>
#include <cmath>
#include <cstring>


namespace VideoMp4{
// —— HDR(scRGB) → SDR sRGB 的统一转换 ——
// 截图与录屏共用同一套口径：scRGB 1.0 = 80 尼特，按当前显示器的 SDR 白点缩放后
// 再做 linear→sRGB。录屏出口固定为 8 位 sRGB，不再保留 10 位浮点通路。
inline float halfToFloat(unsigned short h)
{
    const unsigned int sign = (unsigned int)(h & 0x8000u) << 16;
    unsigned int exp = (h >> 10) & 0x1F;
    unsigned int mant = h & 0x3FF;
    unsigned int bits = 0;
    if (exp == 0) {
        if (mant == 0) bits = sign;
        else {
            exp = 127 - 15 + 1;
            while ((mant & 0x400u) == 0) { mant <<= 1; --exp; }
            mant &= 0x3FFu;
            bits = sign | (exp << 23) | (mant << 13);
        }
    }
    else if (exp == 31) {
        bits = sign | 0x7F800000u | (mant << 13);
    }
    else {
        bits = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
    }
    float f = 0.f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

inline unsigned char linearToSrgbByte(float v)
{
    if (!(v > 0.f)) return 0;
    if (v >= 1.f) return 255;
    const float s = (v <= 0.0031308f) ? v * 12.92f : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f;
    const int n = (int)std::lround(s * 255.f);
    return (unsigned char)(n < 0 ? 0 : (n > 255 ? 255 : n));
}

// 高光滚降：拐点以下按原值输出，SDR 内容（含接近白点的界面）保持原样；
// 超过拐点的部分用双曲曲线压进剩余码值，避免 HDR 高光在 8 位输出里被整片裁成纯白。
// 曲线在拐点处值与斜率都连续（斜率 1），所以过渡不会出现台阶或色带；
// 拐点取 0.9 后，SDR 白落到约 249，余下的 6 个码值留给高光层次；两者都在
// 肉眼不易察觉的范围内。输入已按「SDR 白点归一化」，1.0 即屏幕上的 SDR 白。
inline float softHighlightKnee(float v)
{
    constexpr float knee = 0.9f;
    if (!(v > knee)) return v;
    const float t = (v - knee) / (1.f - knee);
    return knee + (1.f - knee) * (t / (1.f + t));
}

// 当前显示器的"SDR 内容亮度"（尼特）。非高级颜色 / 查询失败按 80 尼特。
inline float querySdrWhiteNits(HMONITOR mon)
{
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!mon || !GetMonitorInfoW(mon, &mi)) return 80.f;

    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
        return 80.f;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS)
        return 80.f;

    for (UINT32 i = 0; i < pathCount; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof(src);
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
        if (_wcsicmp(src.viewGdiDeviceName, mi.szDevice) != 0) continue;

        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO color{};
        color.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
        color.header.size = sizeof(color);
        color.header.adapterId = paths[i].targetInfo.adapterId;
        color.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&color.header) != ERROR_SUCCESS || !color.advancedColorEnabled)
            return 80.f;

        DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
        white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        white.header.size = sizeof(white);
        white.header.adapterId = paths[i].targetInfo.adapterId;
        white.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS && white.SDRWhiteLevel > 0)
            return (float)white.SDRWhiteLevel * 80.f / 1000.f;
        return 80.f;
    }
    return 80.f;
}

class AHANDLE
{
private:
    HANDLE hX = INVALID_HANDLE_VALUE;

public:

    AHANDLE()
    {
        hX = INVALID_HANDLE_VALUE;
    }
    ~AHANDLE()
    {
        Close();
    }
    AHANDLE(const AHANDLE& h)
    {
        DuplicateHandle(GetCurrentProcess(), h.hX, GetCurrentProcess(), &hX, 0, 0, DUPLICATE_SAME_ACCESS);
    }
    AHANDLE(AHANDLE&& h)
    {
        hX = h.hX;
        h.hX = INVALID_HANDLE_VALUE;
    }
    AHANDLE(HANDLE hY)
    {
        hX = hY;
    }
    AHANDLE& operator =(const AHANDLE& h)
    {
        Close();
        DuplicateHandle(GetCurrentProcess(), h.hX, GetCurrentProcess(), &hX, 0, 0, DUPLICATE_SAME_ACCESS);
        return *this;
    }
    AHANDLE& operator =(AHANDLE&& h)
    {
        Close();
        hX = h.hX;
        h.hX = INVALID_HANDLE_VALUE;
        return *this;
    }

    void Close()
    {
        if (hX != INVALID_HANDLE_VALUE)
            CloseHandle(hX);
        hX = INVALID_HANDLE_VALUE;
    }

    operator bool() const
    {
        if (hX == INVALID_HANDLE_VALUE)
            return false;
        return true;
    }
    operator HANDLE() const
    {
        return hX;
    }


};
struct REBUFFERLOCK
{
    std::recursive_mutex* mm;
    REBUFFERLOCK(std::recursive_mutex& m)
    {
        mm = &m;
        m.lock();
    }
    ~REBUFFERLOCK()
    {
        mm->unlock();
    }
};
template <typename T = float>
class MIXBUFFER
{
    T* m = 0;
    unsigned int count = 0;
public:

    MIXBUFFER(T* f = 0)
    {
        Set(f);
    }
    void Set(T* f)
    {
        m = f;
    }
    void Reset(unsigned long long samples)
    {
        if (m)
            memset(m, 0, (size_t)(samples * sizeof(T)));
        count = 0;
    }
    void Put(T* data, unsigned long long samples, bool ForceIfEmpty = false, float VOut = 1.0f)
    {
        if (!m)
            return;
        if (!samples)
            return;

        // 空指针或空长度都直接忽略
        if (ForceIfEmpty == false && data)
        {
            bool Silence = true;
            for (unsigned long long ismp = 0; ismp < samples; ismp++)
            {
                if (data[ismp] > 0.001f || data[ismp] < -0.001f)
                {
                    Silence = false;
                    break;
                }
            }
            if (Silence)
                return;
        }
        if (ForceIfEmpty == false && !data)
            return;

        if (data)
        {
            if (count == 0 && VOut >= 0.99f && VOut <= 1.01f)
                memcpy(m, data, (size_t)(samples * sizeof(T)));
            else
            {
                for (unsigned long long s = 0; s < samples; s++)
                {
                    m[s] += data[s] * VOut;
                }
            }
        }
        else
        {
            if (count == 0)
                memset(m, 0, (size_t)(samples * sizeof(T)));
            else
            {
            }
        }
        count++;
    }
    T* Fin(unsigned long long samples)
    {
        if (count <= 1 || !m)
        {
            count = 0;
            return m;
        }

        for (unsigned long long s = 0; s < samples; s++)
            m[s] /= (float)count;
        count = 0;
        return m;
    }

};
struct REBUFFER
{
    std::recursive_mutex m;
    std::vector<char> d;
    AHANDLE Has = CreateEvent(0, TRUE, 0, 0);

    size_t PushX(const char* dd, size_t sz)
    {
        REBUFFERLOCK l(m);
        auto s = d.size();
        d.resize(s + sz);
        if (dd)
            memcpy(d.data() + s, dd, sz);
        else
            memset(d.data() + s, 0, sz);

        SetEvent(Has);
        return s + sz;
    }

    size_t Av()
    {
        REBUFFERLOCK l(m);
        return d.size();
    }

    size_t PopX(char* trg, size_t sz, DWORD wi = 0, bool NR = false)
    {
        if (wi)
            WaitForSingleObject(Has, wi);
        REBUFFERLOCK l(m);
        if (sz >= d.size())
            sz = d.size();
        if (sz == 0)
            return 0;
        if (trg)
            memcpy(trg, d.data(), sz);
        if (NR == false)
            d.erase(d.begin(), d.begin() + sz);
        if (d.size() == 0)
            ResetEvent(Has);
        return sz;
    }


    void Clear()
    {
        REBUFFERLOCK l(m);
        d.clear();
    }
};
class CAPTURE
{
public:
    // 没走桌面复制时给 GDI 用的那套 DC/位图。位图按 32bpp、正高度（自下而上）建，
    // 抓下来的行序与桌面复制一致，后面的缩放/编码链路不用区分来源。
    ~CAPTURE()
    {
        if (gdiMem && gdiOld) SelectObject(gdiMem, gdiOld);
        if (gdiBmp) DeleteObject(gdiBmp);
        if (gdiMem) DeleteDC(gdiMem);
        if (gdiScreen) ReleaseDC(nullptr, gdiScreen);
    }

    bool PrepareGdi(const RECT& deskRect)
    {
        gdiW = (int)(deskRect.right - deskRect.left);
        gdiH = (int)(deskRect.bottom - deskRect.top);
        if (gdiW <= 0 || gdiH <= 0) return false;
        gdiScreen = GetDC(nullptr);
        if (!gdiScreen) return false;
        gdiMem = CreateCompatibleDC(gdiScreen);
        if (!gdiMem) return false;
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = gdiW;
        // 正高度 = 自下而上的行序：GDI 位图与 MFVideoFormat_RGB32 的输入口径相同
        bmi.bmiHeader.biHeight = gdiH;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        bmi.bmiHeader.biSizeImage = (DWORD)gdiW * (DWORD)gdiH * 4u;
        gdiBmp = CreateDIBSection(gdiScreen, &bmi, DIB_RGB_COLORS, &gdiBits, nullptr, 0);
        if (!gdiBmp || !gdiBits) return false;
        gdiOld = SelectObject(gdiMem, gdiBmp);
        gdiRect = deskRect;
        gdiMode = true;
        InHDR = DXGI_FORMAT_UNKNOWN;   // GDI 只有 8 位 SDR
        return true;
    }

    // 抓一帧（deskRect 是桌面坐标）并合成光标。CAPTUREBLT 把分层窗口也带上 ——
    // 鼠标穿透时本程序的标注层就是分层窗口，不带它录出来会缺标注。
    bool GrabGdi(bool withCursor, const RECT& deskRect)
    {
        if (!gdiScreen || !gdiMem || !gdiBits) return false;
        if (!BitBlt(gdiMem, 0, 0, gdiW, gdiH, gdiScreen, deskRect.left, deskRect.top,
                SRCCOPY | CAPTUREBLT))
            return false;
        if (withCursor)
        {
            CURSORINFO ci{ sizeof(ci) };
            if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING))
            {
                ICONINFO info{};
                // GetIconInfo 失败时 hot spot 是栈上垃圾，下面不能拿它定位
                if (GetIconInfo(ci.hCursor, &info))
                {
                    const int cx = ci.ptScreenPos.x - deskRect.left - (int)info.xHotspot;
                    const int cy = ci.ptScreenPos.y - deskRect.top - (int)info.yHotspot;
                    if (cx > -64 && cy > -64 && cx < gdiW && cy < gdiH)
                        DrawIconEx(gdiMem, cx, cy, ci.hCursor, 0, 0, 0, nullptr,
                            DI_NORMAL | DI_DEFAULTSIZE);
                }
                if (info.hbmMask) DeleteObject(info.hbmMask);
                if (info.hbmColor) DeleteObject(info.hbmColor);
                // 光标句柄是系统共享的，不能 DestroyIcon
            }
        }
        const size_t need = (size_t)gdiW * (size_t)gdiH * 4;
        if (buf.size() != need) buf.resize(need);
        memcpy(buf.data(), gdiBits, need);
        return true;
    }

    DXGI_FORMAT InHDR = DXGI_FORMAT_UNKNOWN;
    // 是否退到了"只列 BGRA8、由系统转换"的桌面复制，见 Prepare
    bool SDRForced = false;
    // —— 统一 HDR→SDR：所有帧出口都是 BGRA8(sRGB) ——
    float sdrWhiteNits{ 80.f };
    std::array<unsigned char, 65536> srgbLut{};
    float srgbLutWhite{ 0.f };
    bool srgbLutValid{ false };
    std::vector<BYTE> sdrBuf;
    ULONGLONG lastWhiteRefresh{ 0 };

    void ensureSrgbLut()
    {
        const float white = (sdrWhiteNits > 1.f) ? sdrWhiteNits : 80.f;
        if (srgbLutValid && std::fabs(srgbLutWhite - white) < 0.5f) return;
        const float inv = 80.f / white;
        for (int bits = 0; bits < 65536; ++bits) {
            // scRGB 线性 → 以 SDR 白点为单位 → 高光滚降 → sRGB 编码。
            // 前两步固定，滚降只让超出 SDR 白点的部分让步，SDR 内容仍按原值出图。
            float v = halfToFloat((unsigned short)bits) * inv;
            srgbLut[(size_t)bits] = linearToSrgbByte(softHighlightKnee(v));
        }
        srgbLutWhite = white;
        srgbLutValid = true;
    }

    void refreshSdrWhite()
    {
        lastWhiteRefresh = GetTickCount64();
        const HMONITOR mon = MonitorFromRect(&monRect, MONITOR_DEFAULTTONEAREST);
        const float next = querySdrWhiteNits(mon);
        if (std::fabs(next - sdrWhiteNits) >= 0.5f) {
            sdrWhiteNits = next;
            srgbLutValid = false;
        }
    }

    // FP16(scRGB) → BGRA8(sRGB)。输入 buf 是自下而上的行序，输出保持一致，
    // 直接满足 MFVideoFormat_RGB32 的输入口径。只有 HDR 屏才会拿到 FP16 复制表面，
    // SDR 屏不会进这个函数。
    bool toneMapHdrToBgra(int w, int h)
    {
        if (w <= 0 || h <= 0 || buf.empty()) return false;
        const size_t need = (size_t)w * (size_t)h * 4;
        if (sdrBuf.size() != need) sdrBuf.resize(need);
        ensureSrgbLut();
        if (InHDR != DXGI_FORMAT_R16G16B16A16_FLOAT) return false;
        const unsigned short* src = reinterpret_cast<const unsigned short*>(buf.data());
        if ((size_t)w * h * 4 > buf.size() / 2) return false;
        for (size_t i = 0, n = (size_t)w * h; i < n; ++i) {
            const unsigned short* p = src + i * 4;
            BYTE* d = sdrBuf.data() + i * 4;
            d[0] = srgbLut[p[2]];   // B
            d[1] = srgbLut[p[1]];   // G
            d[2] = srgbLut[p[0]];   // R
            d[3] = 255;
        }
        return true;
    }

    // —— HDR 光标叠加 ——
    // FP16 复制表面没有 GDI 兼容位，光标不能像 SDR 那样用 DrawIconEx 直接画进 GPU 纹理。
    // 这里把光标图形渲染成一张 32 位位图（老式光标按掩码补 alpha），再按 alpha 混到
    // 色调映射后的 BGRA8 画面上；混合放在 CPU 侧，是因为这时的画面已经是普通内存里的
    // 紧凑 BGRA 缓冲，不需要再为光标建一张 GPU 纹理。
    struct CURSORSHOT
    {
        HCURSOR cur = nullptr;
        int w = 0, h = 0, hotX = 0, hotY = 0;
        std::vector<BYTE> pix;   // top-down BGRA
    };
    CURSORSHOT cursorShot;

    bool ensureCursorShot(HCURSOR cur)
    {
        if (cursorShot.cur == cur && !cursorShot.pix.empty()) return true;
        ICONINFO ii{};
        if (!cur || !GetIconInfo(cur, &ii)) return false;
        auto cleanup = [&] {
            if (ii.hbmColor) DeleteObject(ii.hbmColor);
            if (ii.hbmMask) DeleteObject(ii.hbmMask);
        };

        int width = 32, height = 32;
        if (ii.hbmColor) {
            BITMAP bm{};
            if (GetObject(ii.hbmColor, sizeof(bm), &bm)) { width = bm.bmWidth; height = bm.bmHeight; }
        }
        else if (ii.hbmMask) {
            BITMAP bm{};
            if (GetObject(ii.hbmMask, sizeof(bm), &bm)) { width = bm.bmWidth; height = bm.bmHeight / 2; }
        }
        width = width < 1 ? 1 : (width > 256 ? 256 : width);
        height = height < 1 ? 1 : (height > 256 ? 256 : height);

        HDC screen = GetDC(nullptr);
        HDC mem = screen ? CreateCompatibleDC(screen) : nullptr;
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -height;   // top-down：与热点坐标一致
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP dib = mem ? CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
        bool ok = false;
        if (dib && bits)
        {
            HGDIOBJ old = SelectObject(mem, dib);
            memset(bits, 0, (size_t)width * (size_t)height * 4);
            DrawIconEx(mem, 0, 0, cur, width, height, 0, nullptr, DI_NORMAL);

            cursorShot.pix.assign((size_t)width * (size_t)height * 4, 0);
            memcpy(cursorShot.pix.data(), bits, cursorShot.pix.size());

            // 老式光标只有 AND/XOR 掩码、没有 alpha：DrawIconEx 之后 alpha 仍全 0，按掩码补
            bool anyAlpha = false;
            for (size_t i = 3; i < cursorShot.pix.size(); i += 4) {
                if (cursorShot.pix[i]) { anyAlpha = true; break; }
            }
            BITMAP bmMask{};
            if (!anyAlpha && ii.hbmMask && GetObject(ii.hbmMask, sizeof(bmMask), &bmMask)
                && bmMask.bmWidth >= width && bmMask.bmHeight >= height)
            {
                BITMAPINFO mi{};
                mi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                mi.bmiHeader.biWidth = bmMask.bmWidth;
                mi.bmiHeader.biHeight = -bmMask.bmHeight;   // top-down
                mi.bmiHeader.biPlanes = 1;
                mi.bmiHeader.biBitCount = 1;
                mi.bmiHeader.biCompression = BI_RGB;
                const int stride = ((bmMask.bmWidth + 31) / 32) * 4;
                std::vector<BYTE> maskBits((size_t)stride * bmMask.bmHeight, 0);
                if (GetDIBits(mem, ii.hbmMask, 0, bmMask.bmHeight, maskBits.data(), &mi, DIB_RGB_COLORS) != 0)
                {
                    for (int y = 0; y < height; ++y) {
                        const BYTE* row = maskBits.data() + (size_t)y * stride;
                        BYTE* line = cursorShot.pix.data() + (size_t)y * width * 4;
                        for (int x = 0; x < width; ++x) {
                            const bool transparent = (row[x >> 3] >> (7 - (x & 7))) & 1;
                            line[x * 4 + 3] = transparent ? 0 : 255;
                        }
                    }
                }
            }
            SelectObject(mem, old);
            cursorShot.cur = cur;
            cursorShot.w = width;
            cursorShot.h = height;
            cursorShot.hotX = (int)ii.xHotspot;
            cursorShot.hotY = (int)ii.yHotspot;
            ok = true;
        }
        if (dib) DeleteObject(dib);
        if (mem) DeleteDC(mem);
        if (screen) ReleaseDC(nullptr, screen);
        cleanup();
        return ok;
    }

    // dst 是自下而上的 BGRA8；originX/originY 是裁剪区左上角在虚拟桌面中的坐标
    void blendCursor(std::vector<BYTE>& dst, int w, int h, int originX, int originY)
    {
        if (w <= 0 || h <= 0 || dst.empty()) return;
        CURSORINFO ci{ sizeof(ci) };
        if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) return;
        if (!ensureCursorShot(ci.hCursor)) return;
        const int cx = (int)ci.ptScreenPos.x - originX - cursorShot.hotX;
        const int cy = (int)ci.ptScreenPos.y - originY - cursorShot.hotY;
        const BYTE* src = cursorShot.pix.data();
        for (int y = 0; y < cursorShot.h; ++y) {
            const int dy = cy + y;
            if (dy < 0 || dy >= h) continue;
            BYTE* drow = dst.data() + (size_t)(h - 1 - dy) * (size_t)w * 4;
            for (int x = 0; x < cursorShot.w; ++x) {
                const int dx = cx + x;
                if (dx < 0 || dx >= w) continue;
                const BYTE* s = src + ((size_t)y * (size_t)cursorShot.w + (size_t)x) * 4;
                const unsigned a = s[3];
                if (!a) continue;
                BYTE* d = drow + (size_t)dx * 4;
                if (a == 255) {
                    d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
                }
                else {
                    d[0] = (BYTE)((s[0] * a + d[0] * (255 - a)) / 255);
                    d[1] = (BYTE)((s[1] * a + d[1] * (255 - a)) / 255);
                    d[2] = (BYTE)((s[2] * a + d[2] * (255 - a)) / 255);
                }
                d[3] = 255;
            }
        }
    }
    // —— GDI 兜底（见 DESKTOPCAPTUREPARAMS::GdiFallback）——
    bool gdiMode = false;
    HDC gdiScreen = nullptr;
    HDC gdiMem = nullptr;
    HBITMAP gdiBmp = nullptr;
    HGDIOBJ gdiOld = nullptr;
    void* gdiBits = nullptr;
    int gdiW = 0, gdiH = 0;
    RECT gdiRect{};
    HRESULT CreateDirect3DDevice(IDXGIAdapter1* g)
    {
        HRESULT hr = S_OK;

        // 没有指定适配器时，按「硬件 → WARP → 软件参考驱动」的顺序试
        D3D_DRIVER_TYPE DriverTypes[] =
        {
            D3D_DRIVER_TYPE_HARDWARE,
            D3D_DRIVER_TYPE_WARP,
            D3D_DRIVER_TYPE_REFERENCE,
        };
        UINT NumDriverTypes = ARRAYSIZE(DriverTypes);

        // 桌面复制只要求 D3D11，低版本作为老机器的退路
        D3D_FEATURE_LEVEL FeatureLevels[] =
        {
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
            D3D_FEATURE_LEVEL_9_3,
            D3D_FEATURE_LEVEL_9_2,
            D3D_FEATURE_LEVEL_9_1
        };
        UINT NumFeatureLevels = ARRAYSIZE(FeatureLevels);

        D3D_FEATURE_LEVEL FeatureLevel;

        // 指定了适配器时 DriverType 必须传 UNKNOWN，否则 D3D11CreateDevice 直接 E_INVALIDARG，
        // 下面那个循环三种 DriverType 会全军覆没
        if (g)
        {
            hr = D3D11CreateDevice(g, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, FeatureLevels, NumFeatureLevels,
                D3D11_SDK_VERSION, &device, &FeatureLevel, &context);
            if (SUCCEEDED(hr))
                return S_OK;
            g = nullptr; //指定的那块建不起来就退回默认适配器，别整个录制卡死在这
        }

        // 逐个驱动类型尝试，第一个成功的就留着
        for (UINT DriverTypeIndex = 0; DriverTypeIndex < NumDriverTypes; ++DriverTypeIndex)
        {
            hr = D3D11CreateDevice(g, DriverTypes[DriverTypeIndex], nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, FeatureLevels, NumFeatureLevels,
                D3D11_SDK_VERSION, &device, &FeatureLevel, &context);
            if (SUCCEEDED(hr))
                break;
        }
        if (FAILED(hr))
            return hr;

        return S_OK;
    }



    std::vector<BYTE> buf;
    CComPtr<ID3D11Device> device;
    CComPtr<ID3D11DeviceContext> context;
    CComPtr<IDXGIOutputDuplication> lDeskDupl;
    CComPtr<ID3D11Texture2D> lGDIImage;
    CComPtr<ID3D11Texture2D> lDestImage;
    DXGI_OUTDUPL_DESC lOutputDuplDesc = {};
    // 当前采集目标那块屏在虚拟桌面里的矩形（Prepare 时记下）：换屏跟随要拿它判断"是否换屏"
    RECT monRect{};

    // 换屏跟随：在本设备所属适配器下找目标显示器的输出索引。
    // 只有这个范围内可行 —— 挂在别的适配器上的屏，DuplicateOutput 用本设备的纹理接不住
    static bool FindOutputOnDeviceAdapter(ID3D11Device* dev, const RECT& wantMon, UINT& outIndex)
    {
        if (!dev) return false;
        CComPtr<IDXGIDevice> dxgiDev;
        if (FAILED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev)) || !dxgiDev) return false;
        CComPtr<IDXGIAdapter> ad;
        if (FAILED(dxgiDev->GetParent(__uuidof(IDXGIAdapter), (void**)&ad)) || !ad) return false;
        for (UINT i = 0;; ++i)
        {
            CComPtr<IDXGIOutput> out;
            if (FAILED(ad->EnumOutputs(i, &out)) || !out) break;
            DXGI_OUTPUT_DESC d{};
            if (FAILED(out->GetDesc(&d))) continue;
            if (d.DesktopCoordinates.left == wantMon.left && d.DesktopCoordinates.top == wantMon.top
                && d.DesktopCoordinates.right == wantMon.right && d.DesktopCoordinates.bottom == wantMon.bottom)
            {
                outIndex = i;
                return true;
            }
        }
        return false;
    }

    static void GetAdapters(std::vector<CComPtr<IDXGIAdapter1>>& a)
    {
        CComPtr<IDXGIFactory1> df;
        CreateDXGIFactory1(__uuidof(IDXGIFactory1),(void**)&df);
        a.clear();
        if (!df)
            return;
        int L = 0;
        for (;;)
        {
            CComPtr<IDXGIAdapter1> lDxgiAdapter;
            df->EnumAdapters1(L, &lDxgiAdapter);
            if (!lDxgiAdapter)
                break;
            L++;
            a.push_back(lDxgiAdapter);
        }
        return;
    }

    bool Get(IDXGIResource* lDesktopResource,bool Curs,RECT* rcx = 0)
    {
        // 取到的帧资源只有当它是 2D 纹理时才能拷进本设备的纹理
        CComPtr<ID3D11Texture2D> lAcquiredDesktopImage;
        if (!lDesktopResource)
            return 0;
        auto hr = lDesktopResource->QueryInterface(IID_PPV_ARGS(&lAcquiredDesktopImage));
        if (!lAcquiredDesktopImage)
            return 0;
        lDesktopResource = 0;


        if (InHDR != DXGI_FORMAT_UNKNOWN || Curs == 0)
        {
            // HDR 帧没有 GDI 兼容位，光标改在色调映射后由 blendCursor 叠加；
            // 不录光标时更省一步，直接拷进暂存纹理
            context->CopyResource(lDestImage, lAcquiredDesktopImage);
        }
        else
        {
            // SDR 且要录光标：先拷到 GDI 兼容纹理，GDI 才能把光标画上去
            context->CopyResource(lGDIImage, lAcquiredDesktopImage);

            // 把光标画进这张 GDI 兼容纹理
            CComPtr<IDXGISurface1> lIDXGISurface1;

            lIDXGISurface1 = lGDIImage;

            if (!lIDXGISurface1)
                return 0;

            CURSORINFO lCursorInfo = { 0 };
            lCursorInfo.cbSize = sizeof(lCursorInfo);
            auto lBoolres = GetCursorInfo(&lCursorInfo);
            if (lBoolres == TRUE)
            {
                if (lCursorInfo.flags == CURSOR_SHOWING && Curs)
                {
                    // GetIconInfo 和 GetDC 都可能失败（自绘光标、远程桌面、GDI 被挂钩），
                    // 失败时 iconInfo 和 lHDC 还是栈上的垃圾，照着往下用就是拿野句柄喂 GDI。
                    ICONINFO iconInfo{};
                    auto lGotIcon = GetIconInfo(lCursorInfo.hCursor, &iconInfo);
                    if (iconInfo.hbmMask) DeleteObject(iconInfo.hbmMask);
                    if (iconInfo.hbmColor) DeleteObject(iconInfo.hbmColor);
                    HDC  lHDC = nullptr;
                    if (lGotIcon && SUCCEEDED(lIDXGISurface1->GetDC(FALSE, &lHDC)) && lHDC)
                    {
                        // 纹理只覆盖单块显示器，光标坐标却是虚拟桌面坐标：
                        // 必须减去本屏原点，否则副屏（非 0,0）上光标会被画到纹理外
                        const int cx = (int)lCursorInfo.ptScreenPos.x - monRect.left - (int)iconInfo.xHotspot;
                        const int cy = (int)lCursorInfo.ptScreenPos.y - monRect.top - (int)iconInfo.yHotspot;
                        DrawIconEx(
                            lHDC,
                            cx,
                            cy,
                            lCursorInfo.hCursor,
                            0,
                            0,
                            0,
                            0,
                            DI_NORMAL | DI_DEFAULTSIZE);
                        // ReleaseDC 只在 GetDC 拿到了 DC 时调
                        lIDXGISurface1->ReleaseDC(nullptr);
                    }
                }
            }

            // 画好光标后再拷到 CPU 可读的暂存纹理
            context->CopyResource(lDestImage, lGDIImage);
        }


        // 从暂存纹理映射到内存，按裁剪区拷成紧凑位图
        D3D11_MAPPED_SUBRESOURCE resource;
        UINT subresource = D3D11CalcSubresource(0, 0, 0);
        hr = context->Map(lDestImage, subresource, D3D11_MAP_READ_WRITE, 0, &resource);
        if (FAILED(hr))
            return 0;

        int multi = 4;
        if (InHDR == DXGI_FORMAT_R16G16B16A16_FLOAT)
            multi = 8; // FP16 每像素 8 字节

        auto sz = lOutputDuplDesc.ModeDesc.Width
            * lOutputDuplDesc.ModeDesc.Height * multi;
        if (rcx)
            sz = (rcx->right - rcx->left) * (rcx->bottom - rcx->top) * multi;
        // 直接按目标尺寸 resize：先按整屏再缩到选区的话，capacity 会一直停在整屏大小，
        // 录一小块区域也要占一整屏内存。
        if (buf.size() != (size_t)sz) buf.resize(sz);

        UINT lBmpRowPitch = lOutputDuplDesc.ModeDesc.Width * multi;
        if (rcx)
            lBmpRowPitch = (rcx->right - rcx->left) * multi;
        UINT lRowPitch = std::min<UINT>(lBmpRowPitch, resource.RowPitch);

        BYTE* sptr = reinterpret_cast<BYTE*>(resource.pData);
        BYTE* dptr = buf.data() + sz - lBmpRowPitch;
        if (rcx)
            sptr += rcx->left * multi;
        for (size_t h = 0; h < lOutputDuplDesc.ModeDesc.Height; ++h)
        {
            if (rcx && h < (size_t)rcx->top)
            {
                sptr += resource.RowPitch;
                continue;
            }
            if (rcx && h >= (size_t)rcx->bottom)
                break;
            memcpy_s(dptr, lBmpRowPitch, sptr, lRowPitch);
            sptr += resource.RowPitch;
            dptr -= lBmpRowPitch;
        }
        context->Unmap(lDestImage, subresource);
        return 1;
    }

    bool Prepare(UINT Output = 0)
    {
        // 从已建好的 D3D 设备反查它所属的 DXGI 设备与适配器
        CComPtr<IDXGIDevice> lDxgiDevice;
        lDxgiDevice = device;
        if (!lDxgiDevice)
            return 0;

        HRESULT hr = 0;

        CComPtr<IDXGIAdapter> lDxgiAdapter;
        hr = lDxgiDevice->GetParent(
            __uuidof(IDXGIAdapter),
            reinterpret_cast<void**>(&lDxgiAdapter));

        if (FAILED(hr))
            return 0;

        lDxgiDevice = 0;

        // 按输出索引取要录的那块显示器
        CComPtr<IDXGIOutput> lDxgiOutput;
        hr = lDxgiAdapter->EnumOutputs(Output, &lDxgiOutput);
        if (FAILED(hr))
            return 0;

        lDxgiAdapter = 0;

        DXGI_OUTPUT_DESC lOutputDesc;
        hr = lDxgiOutput->GetDesc(&lOutputDesc);
        monRect = lOutputDesc.DesktopCoordinates;   // 换屏跟随的判据
        // HDR→SDR 的基准：当前这块屏的 Windows「SDR 内容亮度」
        sdrWhiteNits = querySdrWhiteNits(lOutputDesc.Monitor);
        srgbLutValid = false;
        lastWhiteRefresh = GetTickCount64();

        // 创建桌面复制至少需要 Output1
        CComPtr<IDXGIOutput1> lDxgiOutput1;
        lDxgiOutput1 = lDxgiOutput;
        if (!lDxgiOutput1)
            return 0;

        lDxgiOutput = 0;

        // 按桌面原生格式取帧：把 FP16(scRGB) 和 BGRA8 一起交给 DuplicateOutput1，HDR 屏拿到
        // FP16、SDR 屏拿到 BGRA8，两者都命中桌面当前格式，系统不做任何转换。
        // 只列 BGRA8 时系统会替我们把 FP16 转成 8 位，这个换算没有线性→sRGB 编码，
        // 中间调会被压暗，且高光截断位置不受本程序控制 —— 色调映射必须自己做，
        // 所以这里只在驱动不认双格式列表时才退回系统转换。
        // DuplicateOutput1 要求 Win10 1703 以上且进程 per-monitor DPI aware；拿不到就退回旧接口
        CComPtr<IDXGIOutput5> lDxgiOutput5;
        lDxgiOutput5 = lDxgiOutput1;
        SDRForced = false;
        bool nativeFormats = false;
        if (lDxgiOutput5)
        {
            const DXGI_FORMAT want[] = { DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_B8G8R8A8_UNORM };
            hr = lDxgiOutput5->DuplicateOutput1(device, 0, 2, want, &lDeskDupl);
            nativeFormats = SUCCEEDED(hr);
            if (!nativeFormats)
            {
                // 个别驱动不认双格式列表：只能退到只列 BGRA8，系统转换画质差但还能录
                const DXGI_FORMAT wantSdr[] = { DXGI_FORMAT_B8G8R8A8_UNORM };
                hr = lDxgiOutput5->DuplicateOutput1(device, 0, 1, wantSdr, &lDeskDupl);
                SDRForced = SUCCEEDED(hr);
            }
        }
        if (!nativeFormats && !SDRForced)
            hr = lDxgiOutput1->DuplicateOutput(
                device,
                &lDeskDupl);

        if (FAILED(hr))
            return 0;

        lDxgiOutput5 = 0;
        lDxgiOutput1 = 0;

        // 建 GPU 侧纹理：SDR 屏要靠它获得 GDI 兼容表面来画光标，HDR 屏只做拷贝中转
        lDeskDupl->GetDesc(&lOutputDuplDesc);
        // 只列了 BGRA8 一个，拿到的帧就只可能是它 —— 别听 GetDesc 报的显示模式格式，
        // 那个在 HDR 屏上仍是 FP16，照它建纹理会让下面的 CopyResource 拿不到画面
        if (SDRForced)
            lOutputDuplDesc.ModeDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = lOutputDuplDesc.ModeDesc.Width;
        desc.Height = lOutputDuplDesc.ModeDesc.Height;
        desc.Format = lOutputDuplDesc.ModeDesc.Format;
        desc.ArraySize = 1;
        desc.BindFlags = D3D11_BIND_FLAG::D3D11_BIND_RENDER_TARGET;
        InHDR = DXGI_FORMAT_UNKNOWN;
        if (desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT)
        {
            // FP16 表面没有 GDI 兼容位，光标只能另走 CPU 混合（见 blendCursor）
            InHDR = desc.Format;
        }
        else
            desc.MiscFlags = D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.MipLevels = 1;
        desc.CPUAccessFlags = 0;
        desc.Usage = D3D11_USAGE_DEFAULT;
        lGDIImage = 0;
        hr = device->CreateTexture2D(&desc, NULL, &lGDIImage);
        if (FAILED(hr))
            return 0;

        // 再建一张 CPU 可读的暂存纹理，映射后就是内存里的紧凑位图
        desc.Width = lOutputDuplDesc.ModeDesc.Width;
        desc.Height = lOutputDuplDesc.ModeDesc.Height;
        desc.Format = lOutputDuplDesc.ModeDesc.Format;
        desc.ArraySize = 1;
        desc.BindFlags = 0;
        desc.MiscFlags = 0;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.MipLevels = 1;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
        desc.Usage = D3D11_USAGE_STAGING;
        lDestImage = 0;
        hr = device->CreateTexture2D(&desc, NULL, &lDestImage);
        if (FAILED(hr))
            return 0;

        return 1;
    }

};
// 一个要录的音频端点：id 取自 IMMDevice::GetId，loopback=true 表示「系统声音」
// （播放端点 + AUDCLNT_STREAMFLAGS_LOOPBACK），false 表示麦克风。
struct AUDIOSOURCE
{
    std::wstring id;
    bool loopback = false;
};
struct DESKTOPCAPTUREPARAMS
{
    bool HasVideo = 1;
    bool HasAudio = 1;
    std::vector<AUDIOSOURCE> AudioFrom;
    GUID VIDEO_ENCODING_FORMAT = MFVideoFormat_H264;
    GUID AUDIO_ENCODING_FORMAT = MFAudioFormat_MP3;
    std::wstring f;
    void* cb = 0;
    std::function<HRESULT(const BYTE* d, size_t sz,void* cb)> Streamer;
    std::function<HRESULT(DXGI_FORMAT dxgi,int wi,int he,const BYTE* d, size_t sz,void* cb)> Framer;
    std::function<void(IMFAttributes* a)> PrepareAttributes;
    int fps = 25;
    int NumThreads = 0;
    int Qu = -1;
    int vbrm = 0;
    int vbrq = 0;
    int BR = 4000;
    // 输出分辨率上限（高度像素；0 = 按选区原生分辨率直出）。超过时按比例缩到该高度
    int MaxOutHeight = 0;
    int NCH = 2;
    int SR = 44100;
    int ABR = 192;
    bool Cursor = true;
    // 是否允许 SinkWriter 使用硬件 MFT（编码器）。关掉即只走软件编码
    bool HwTransforms = true;
    RECT rx = { 0,0,0,0 };
    // 录制中选区被挪动：UI 线程报告「期望的裁剪区左上角（桌面坐标）」，采集线程据此平移裁剪框；
    // 落到别的显示器上就换采集目标（同适配器内无缝切，换不过去保持原屏）。
    // 坐标打包进一个原子值（高 32 位 x / 低 32 位 y），免得读到半新半旧的一对坐标。
    std::atomic<bool> followOn{ false };
    std::atomic<long long> wantOrigin{ 0 };
    // 换屏失败记录：采集线程判定某块屏换不过去时写入它的左上角（桌面坐标）。
    // UI 侧据此把该屏从"可跨屏"中排除 —— 混合显卡下适配器报告了某块屏、实际复制不起来是常见情况，
    // 排除后选区不会再移过去又被回退。
    std::atomic<bool> hasBlocked{ false };
    std::atomic<long long> switchBlocked{ 0 };
    HWND hWnd = 0;
    IDXGIAdapter1* ad = 0;
    UINT nOutput = 0;
    // —— 桌面复制不可用时的 GDI 兜底 ——
    // 混合显卡（独显跑本程序、画面由核显输出）、虚拟显示器（远程桌面 / 云桌面 / 投屏）、
    // 部分老驱动上，IDXGIOutput1::DuplicateOutput 会直接失败（DXGI_ERROR_UNSUPPORTED）。
    // 那不是设置问题，改「省电」也未必有用 —— 这条路仍能录，只是走 BitBlt，没有 HDR。
    bool GdiFallback = false;
    // GDI 兜底用的裁剪区（**桌面坐标**，不是显示器局部坐标）
    RECT rxDesk = { 0,0,0,0 };
    // 上一次采集是不是 HDR（FP16）那条路：编码器失败时据此退回 GDI（8 位 SDR）再试一次
    bool LastCapturedHdr = false;

    unsigned long long StartMS = 0; // 0, none
    unsigned long long EndMS = 0; // 0, none
    bool MustEnd = false;
    // 用户主动取消（✕ / Esc / 退出）：这段录制本来就要丢弃，收尾阶段再失败也不必弹框
    bool UserAbort = false;
    bool Pause = false;

    // 真正 WriteSample 成功的视频帧数。一帧都没写就不是有效视频，函数末尾据此报错 ——
    // 循环里所有失败都是 break，跟"用户点停止"走同一个出口，光看返回值分不出这两种情况
    unsigned long long framesWritten = 0;
};
struct VectorStreamX2 : public IMFByteStream
{
    ULONG r = 1;
    std::vector<char> d;
    size_t p = 0;

    // IUnknown
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(
        /* [in] */ REFIID riid,
        /* [iid_is][out] */ _COM_Outptr_ void __RPC_FAR* __RPC_FAR* ppvObject)
    {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFByteStream))
        {
            *ppvObject = (IStream*)this;
            r++;
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    virtual ULONG STDMETHODCALLTYPE AddRef(void)
    {
        return ++r;
    }

    virtual ULONG STDMETHODCALLTYPE Release(void)
    {
        return --r;
    }

    HRESULT __stdcall  BeginRead(
        BYTE* pb,
        ULONG            cb,
        IMFAsyncCallback* pCallback,
        IUnknown* punkState
    )
    {
        ULONG pcb = 0;
        Read(pb, cb, &pcb);
        NextCB = pcb;
        CComPtr<IMFAsyncResult> ar;
        MFCreateAsyncResult(0, pCallback, punkState, &ar);
        pCallback->Invoke(ar);
        return S_OK;
    }

    ULONG NextCB = 0;
    HRESULT __stdcall   BeginWrite(
        const BYTE* pb,
        ULONG            cb,
        IMFAsyncCallback* pCallback,
        IUnknown* punkState
    )
    {
        ULONG pcb = 0;
        Write(pb, cb, &pcb);
        NextCB = pcb;
        CComPtr<IMFAsyncResult> ar;
        MFCreateAsyncResult(0, pCallback, punkState, &ar);
        pCallback->Invoke(ar);
        return S_OK;
    }

    HRESULT __stdcall   Close(

    )
    {
        return S_OK;
    }

    HRESULT __stdcall  EndRead(
        IMFAsyncResult*,
        ULONG* pcbRead
    )
    {
        if (!pcbRead)
            return E_POINTER;
        *pcbRead = NextCB;
        return S_OK;
    }

    HRESULT __stdcall EndWrite(
        IMFAsyncResult* ,
        ULONG* pcbWritten
    )
    {
        if (!pcbWritten)
            return E_POINTER;
        *pcbWritten = NextCB;
        return S_OK;
    }

    HRESULT __stdcall  Flush()
    {
        return S_OK;
    }

    HRESULT __stdcall   GetCapabilities(
        DWORD* pdwCapabilities
    )
    {
        if (!pdwCapabilities)
            return E_POINTER;
        *pdwCapabilities = MFBYTESTREAM_IS_READABLE | MFBYTESTREAM_IS_WRITABLE | MFBYTESTREAM_IS_SEEKABLE;
        return S_OK;
    }

    HRESULT __stdcall  GetCurrentPosition(
        QWORD* pqwPosition
    )
    {
        if (!pqwPosition)
            return E_POINTER;
        *pqwPosition = p;
        return S_OK;
    }

    HRESULT __stdcall  GetLength(
        QWORD* q
    )
    {
        if (!q)
            return E_POINTER;
        *q = d.size();
        return S_OK;
    }

    HRESULT __stdcall  IsEndOfStream(
        BOOL* q
    )
    {
        if (!q)
            return E_POINTER;
        *q = FALSE;
        if (p == d.size())
            *q = TRUE;
        return S_OK;
    }

    HRESULT __stdcall  Read(
        BYTE* pv,
        ULONG cb,
        ULONG* pcbRead
    )
    {
        auto av = d.size() - p;
        if (cb < av)
            av = cb;
        memcpy(pv, d.data() + p, av);
        p += av;
        if (pcbRead)
            *pcbRead = (ULONG)av;
        if (av < cb)
            return S_FALSE;
        return S_OK;
    }

    std::function<HRESULT(const BYTE*, size_t,void* cb)> func;
    void* cbx = 0;
    HRESULT __stdcall  Write(
        const BYTE* pv,
        ULONG      cb,
        ULONG* pcbWritten
    )
    {
        if (d.size() < (p + cb))
        {
            auto exc = (p + cb) - d.size();
            d.resize(d.size() + exc);
        }
        memcpy(d.data() + p, pv, cb);

        if (func)
        {
          auto hr = func(pv, cb,cbx);
          if (FAILED(hr))
              return hr;
        }


        p += cb;
        if (pcbWritten)
            *pcbWritten = cb;
        return S_OK;
    }

    HRESULT __stdcall  SetLength(
        QWORD qwLength
    )

    {
        d.resize(qwLength);
        if (p >= qwLength)
            p = qwLength;
        return S_OK;
    }
    
    HRESULT __stdcall  SetCurrentPosition(
        QWORD q
    )
    {
        if (q > d.size())
            return E_FAIL;
        p = q;
        return S_OK;

    }


    HRESULT __stdcall Seek(
        MFBYTESTREAM_SEEK_ORIGIN dwOrigin,
        LONGLONG                 llSeekOffset,
        DWORD                    ,
        QWORD* pqwCurrentPosition
    )
    {
        LARGE_INTEGER lo = { 0 };
        if (dwOrigin == msoBegin)
        {
            p = llSeekOffset;
        }
        if (dwOrigin == msoCurrent)
        {
            p += llSeekOffset;
        }
        if (p >= d.size())
            p = d.size();
        if (pqwCurrentPosition)
            *pqwCurrentPosition = p;

        return S_OK;
    }


};

// 录谁：默认播放端点走 loopback（系统声音），默认录音端点走麦克风。
// 只记端点 id 与用途，具体格式由采集端统一成 16bit/dp.SR，再由音频引擎转换。
inline void setAudio(DESKTOPCAPTUREPARAMS* dp, bool useSpeaker, bool useMic)
{
    if (!useMic && !useSpeaker) return;
    dp->AudioFrom.clear();
    ComPtr<IMMDeviceEnumerator> deviceEnumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_INPROC_SERVER, __uuidof(IMMDeviceEnumerator), (LPVOID*)&deviceEnumerator)) || !deviceEnumerator)
        return;
    const std::pair<EDataFlow, bool> want[] = { { eCapture, false }, { eRender, true } };
    for (auto& [flow, loopback] : want)
    {
        if (loopback ? !useSpeaker : !useMic) continue;
        ComPtr<IMMDevice> dev = nullptr;
        if (FAILED(deviceEnumerator->GetDefaultAudioEndpoint(flow, eConsole, &dev)) || !dev) continue;
        LPWSTR id = nullptr;
        if (FAILED(dev->GetId(&id)) || !id) continue;
        dp->AudioFrom.push_back({ id, loopback });
        CoTaskMemFree(id);
    }
}

// 桌面复制帧的 BGRA 面积平均缩放。源/目标都是紧凑排列（stride = w*4），行序一致，
// 所以这里只按"行索引比例"映射，不做翻转。仅在需要缩小时使用（画质档位）。
inline void scaleBGRABox(const BYTE* src, int sw, int sh, BYTE* dst, int dw, int dh)
{
    for (int j = 0; j < dh; ++j) {
        int sy0 = (int)((long long)j * sh / dh);
        int sy1 = (int)(((long long)(j + 1) * sh + dh - 1) / dh);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > sh) sy1 = sh;
        BYTE* drow = dst + (size_t)j * dw * 4;
        for (int i = 0; i < dw; ++i) {
            int sx0 = (int)((long long)i * sw / dw);
            int sx1 = (int)(((long long)(i + 1) * sw + dw - 1) / dw);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx1 > sw) sx1 = sw;
            unsigned int b = 0, g = 0, r = 0, a = 0, n = 0;
            for (int y = sy0; y < sy1; ++y) {
                const BYTE* srow = src + ((size_t)y * sw + sx0) * 4;
                for (int x = sx0; x < sx1; ++x) {
                    b += srow[0]; g += srow[1]; r += srow[2]; a += srow[3];
                    srow += 4; ++n;
                }
            }
            drow[i * 4 + 0] = (BYTE)(b / n);
            drow[i * 4 + 1] = (BYTE)(g / n);
            drow[i * 4 + 2] = (BYTE)(r / n);
            drow[i * 4 + 3] = (BYTE)(a / n);
        }
    }
}

inline int DesktopCapture(DESKTOPCAPTUREPARAMS& dp)
{
    HRESULT hr = S_OK;
    // 每次尝试都从零计帧：换适配器 / 换编码器 / 换 GDI 兜底时重来一遍，
    // 上一次尝试写进去的帧数不能带到这一次（否则"一帧没写成"的判断会失灵）
    dp.framesWritten = 0;
    struct AUDIOIN
    {
    private:
        BYTE* pData = 0;
        UINT32 framesAvailable = 0;
        DWORD  flags = 0;

    public:
        CComPtr<IAudioClient> ac;
        CComPtr<IAudioClient> ac2;
        CComPtr<IAudioCaptureClient> cap;
        CComPtr<IAudioRenderClient> ren;
        // 采集格式由初始化时统一指定（16bit / dp.NCH / dp.SR），各设备差异由音频引擎转换，
        // 所以这里只有一种样本格式，混音端不必再按位深/声道分叉。
        WAVEFORMATEX wfx = {};

        std::vector<float> tempf;
        std::vector<std::shared_ptr<REBUFFER>> buffer;
        HANDLE hEv = 0;



        bool Capturing = true;
        bool CapturingFin1 = false;
        bool CapturingFin2 = false;
        std::shared_ptr<REBUFFER> AudioDataX;
        std::vector<char> PopBuffer;

        size_t AvFrames()
        {
            auto avs = AudioDataX->Av();
            avs /= wfx.nChannels;
            avs /= (wfx.wBitsPerSample / 8);
            return avs;
        }

        void PlaySilence(REFERENCE_TIME rt)
        {
            // rt 是 100ns 单位，先折成毫秒再乘采样率，得到这一帧时长对应的采样数
            rt /= 10000;
            auto ns = (wfx.nSamplesPerSec * rt);
            ns /= 1000;
            while (Capturing)
            {
                if (!ren)
                    break;

                Sleep((DWORD)(rt / 2));

                if (!Capturing)
                    break;

                // 先问渲染缓冲还空着多少，只补空位，避免一次塞满导致累计延迟
                UINT32 numFramesPadding = 0;
                auto hr = ac2->GetCurrentPadding(&numFramesPadding);
                if (FAILED(hr))
                    break;


                auto numFramesAvailable = ns - numFramesPadding;
                if (!numFramesAvailable)
                    continue;

                BYTE* db = 0;
                hr = ren->GetBuffer((UINT32)numFramesAvailable, &db);
                if (FAILED(hr))
                    break;
                auto bs = numFramesAvailable * wfx.nChannels * wfx.wBitsPerSample / 8;
                memset(db, 0,(size_t) bs);
                ren->ReleaseBuffer((UINT32)numFramesAvailable, 0); //AUDCLNT_BUFFERFLAGS_SILENT
            }
            CapturingFin2 = true;
        }

        void ThreadLoopCapture()
        {
            UINT64 up, uq;
            while (Capturing)
            {
                if (hEv)
                    WaitForSingleObject(hEv, INFINITE);

                if (!Capturing)
                    break;
                auto hr = cap->GetBuffer(&pData, &framesAvailable, &flags, &up, &uq);
                if (FAILED(hr))
                    break;
                if (framesAvailable == 0)
                    continue;

                auto ThisAudioBytes = framesAvailable * wfx.nChannels * wfx.wBitsPerSample/8 ;

                AudioDataX->PushX((const char*)pData, ThisAudioBytes);
                cap->ReleaseBuffer(framesAvailable);
            }
            CapturingFin1 = true;
        }
    };
    // ---------------------- 
    std::vector<std::shared_ptr<AUDIOIN>> ains;

    class AINRELEASE
    {
        std::vector<std::shared_ptr<AUDIOIN>>* ains = 0;
    public:
        AINRELEASE(std::vector<std::shared_ptr<AUDIOIN>>& x)
        {
            ains = &x;
        }
        ~AINRELEASE()
        {
            if (!ains)
                return;
            for (auto& a : *ains)
            {
                a->Capturing = false;
                if (a->hEv)
                    SetEvent(a->hEv);
                for (int i = 0; i < 10; i++)
                {
                    if (a->CapturingFin1 && a->CapturingFin2)
                        break;
                    Sleep(150);
                }
                if (a->hEv)
                    CloseHandle(a->hEv);
                a->hEv = 0;
                    

            }
        }

    };
    AINRELEASE arx(ains);

    // 音频流缓冲长度 = 一帧视频的时长（100ns 单位）：SR 在原来的换算里会被约掉
    const REFERENCE_TIME rt = 10000000 / dp.fps;

    if (dp.hWnd)
    {
        GetWindowRect(dp.hWnd, &dp.rx);
        if (dp.rx.left < 0 || dp.rx.right < 0)
            dp.rx = {};
    }

    CComPtr<IMMDeviceEnumerator> deviceEnumerator = 0;
    if(dp.HasAudio)
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_INPROC_SERVER, __uuidof(IMMDeviceEnumerator), (LPVOID*)&deviceEnumerator);

    CAPTURE cap;
    int wi = 0, he = 0;
    if (dp.HasVideo)
    {
        dp.LastCapturedHdr = false;
        if (dp.GdiFallback)
        {
            // 桌面复制在所有候选适配器上都拿不到 → 换 GDI 抓屏，画质/帧率略降但能录
            if (!cap.PrepareGdi(dp.rxDesk))
                return -2;
            wi = cap.gdiW;
            he = cap.gdiH;
        }
        else
        {
            if (FAILED(cap.CreateDirect3DDevice(dp.ad)))
                return -1;
            if (!cap.Prepare(dp.nOutput))
                return -2;
            // HDR 帧在采集出口统一色调映射成 BGRA8，编码器不再强制 HEVC
            dp.LastCapturedHdr = cap.InHDR != DXGI_FORMAT_UNKNOWN;
            wi = cap.lOutputDuplDesc.ModeDesc.Width;
            he = cap.lOutputDuplDesc.ModeDesc.Height;
            if (dp.rx.right && dp.rx.bottom)
            {
                wi = dp.rx.right - dp.rx.left;
                he = dp.rx.bottom - dp.rx.top;
            }
        }
    }

    // —— 录制中「选区跨屏」跟随 ——
    // UI 报的是期望的裁剪区左上角（桌面坐标）；这里记下它与本帧实际裁剪原点的差，之后每次跟随
    // 都按同一口径平移（避免跟随开始那一下画面整体跳几像素）。尺寸一律沿用开录时定下的裁剪
    // 尺寸 —— 编码器的帧尺寸不可变，所以跟随只平移、不缩放。
    long followBiasX = 0, followBiasY = 0;
    long followCropW = 0, followCropH = 0;
    long lastWantX = LONG_MIN, lastWantY = 0;
    if (dp.HasVideo)
    {
        if (cap.gdiMode)
        {
            // GDI 直接抓桌面坐标的裁剪区，没有「显示器局部坐标」这一层
            followCropW = cap.gdiW;
            followCropH = cap.gdiH;
            cap.gdiRect = dp.rxDesk;
        }
        else
        {
            followCropW = dp.rx.right - dp.rx.left;
            followCropH = dp.rx.bottom - dp.rx.top;
            if (dp.followOn.load(std::memory_order_relaxed))
            {
                const long long packed = dp.wantOrigin.load(std::memory_order_relaxed);
                lastWantX = (long)(int)(packed >> 32);
                lastWantY = (long)(int)(packed & 0xFFFFFFFFll);
                followBiasX = (dp.rx.left + cap.monRect.left) - lastWantX;
                followBiasY = (dp.rx.top + cap.monRect.top) - lastWantY;
            }
        }
    }

    // 「视频质量」输出分辨率上限：只缩不放，宽对齐 4、高对齐 2（与选区对齐要求一致）。
    // 出口已经是 BGRA8，HDR/SDR 都能走同一套缩放。
    int encW = wi, encH = he;
    if (dp.HasVideo && dp.MaxOutHeight > 0 && he > dp.MaxOutHeight)
    {
        const double f = (double)dp.MaxOutHeight / (double)he;
        encW = std::max(4, (int)(wi * f + 0.5) & ~3);
        encH = std::max(2, (int)(he * f + 0.5) & ~1);
        if (encW > wi) encW = wi;
        if (encH > he) encH = he;
    }

    CComPtr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, 0);
    attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, dp.HwTransforms ? 1u : 0u);

    VectorStreamX2 bs;
    bs.func = dp.Streamer;
    CComPtr<IMFSinkWriter> pSinkWriter;
    if (dp.f.empty())
    {
        if (dp.Framer)
        {
            hr = S_OK;
        }
        else
        {
            if (dp.HasVideo)
                attrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
            else
                attrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MP3);
            if (dp.PrepareAttributes)
                dp.PrepareAttributes(attrs);
            hr = MFCreateSinkWriterFromURL(NULL, &bs, attrs, &pSinkWriter);
        }
    }
    else
        hr = MFCreateSinkWriterFromURL(dp.f.c_str(), NULL, attrs, &pSinkWriter);
    if (FAILED(hr)) return -3;


    CComPtr<IMFMediaType> pMediaTypeOutVideo;
    DWORD OutVideoStreamIndex = 0;
    CComPtr<IMFMediaType> pMediaTypeVideoIn;
    if (dp.HasVideo && !dp.Framer)
    {
        hr = MFCreateMediaType(&pMediaTypeOutVideo);
        if (FAILED(hr)) return -4;

        pMediaTypeOutVideo->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);

        pMediaTypeOutVideo->SetGUID(MF_MT_SUBTYPE, dp.VIDEO_ENCODING_FORMAT);

        pMediaTypeOutVideo->SetUINT32(MF_MT_AVG_BITRATE, dp.BR * 1000);
        MFSetAttributeSize(pMediaTypeOutVideo, MF_MT_FRAME_SIZE, encW, encH);
        MFSetAttributeRatio(pMediaTypeOutVideo, MF_MT_FRAME_RATE, dp.fps, 1);
        MFSetAttributeRatio(pMediaTypeOutVideo, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        pMediaTypeOutVideo->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

        // 色彩口径显式声明。输入是 RGB32（全范围），RGB→YUV 由 SinkWriter 的 Color Converter 做；
        // 不声明 MF_MT_YUV_MATRIX 时它默认按 BT.601 转，而高清编码器写 VUI 通常按 BT.709 ——
        // 两边对不上，颜色会偏（饱和度、肤色最明显）。这里按分辨率选，与播放器的通行约定一致。
        const UINT32 yuvMatrix = (encH >= 720) ? MFVideoTransferMatrix_BT709
                                              : MFVideoTransferMatrix_BT601;
        pMediaTypeOutVideo->SetUINT32(MF_MT_YUV_MATRIX, yuvMatrix);
        if (encH >= 720) {
            pMediaTypeOutVideo->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
            pMediaTypeOutVideo->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
        }


        if (dp.VIDEO_ENCODING_FORMAT == MFVideoFormat_H265 || dp.VIDEO_ENCODING_FORMAT == MFVideoFormat_HEVC || dp.VIDEO_ENCODING_FORMAT == MFVideoFormat_HEVC_ES)
            pMediaTypeOutVideo->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_Normal);

        if (dp.VIDEO_ENCODING_FORMAT == MFVideoFormat_VP80 || dp.VIDEO_ENCODING_FORMAT == MFVideoFormat_VP90)
            pMediaTypeOutVideo->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_Normal);

        hr = pSinkWriter->AddStream(pMediaTypeOutVideo, &OutVideoStreamIndex);
        if (FAILED(hr)) return -5;
    }

    // Audio：采集格式统一成 16bit / dp.NCH / dp.SR，设备本来的采样率、位深、声道数
    // 全交给 AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 在音频引擎里转换 —— 96k、24bit、
    // 单声道设备都不用再各自适配，混音端也就只剩一种样本格式。
    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = (WORD)dp.NCH;
    wfx.nSamplesPerSec = dp.SR;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = wfx.nChannels * wfx.wBitsPerSample / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    std::vector<HANDLE> AudioEvents;
    if (deviceEnumerator && dp.HasAudio)
    {
        for (auto& src : dp.AudioFrom)
        {
            std::shared_ptr<AUDIOIN> ain = std::make_shared<AUDIOIN>();
            ain->AudioDataX = std::make_shared<REBUFFER>();

            CComPtr<IMMDevice> d;
            deviceEnumerator->GetDevice(src.id.c_str(), &d);
            if (!d)
                continue;

            // 麦克风走事件回调，系统声音走 loopback
            DWORD flg = AUDCLNT_STREAMFLAGS_NOPERSIST | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM;
            flg |= src.loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : AUDCLNT_STREAMFLAGS_EVENTCALLBACK;

            hr = d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (LPVOID*)&ain->ac);
            if (!ain->ac)
                continue;

            hr = ain->ac->Initialize(AUDCLNT_SHAREMODE_SHARED, flg, rt, 0, &wfx, 0);
            if (FAILED(hr))
                continue;

            hr = ain->ac->GetService(__uuidof(IAudioCaptureClient), (void**)&ain->cap);
            if (!ain->cap)
                continue;

            AudioEvents.push_back(ain->AudioDataX->Has);
            if (!src.loopback)
            {
                ain->hEv = CreateEvent(0, 0, 0, 0);
                ain->ac->SetEventHandle(ain->hEv);
            }
            else
            {
                // 播放端点：另开一路持续写静音，系统没在放声音时 loopback 也照样有数据可抓
                hr = d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (LPVOID*)&ain->ac2);
                if (!ain->ac2)
                    continue;
                hr = ain->ac2->Initialize(AUDCLNT_SHAREMODE_SHARED,
                    AUDCLNT_STREAMFLAGS_NOPERSIST | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM, rt, 0, &wfx, 0);
                if (FAILED(hr))
                    continue;
                hr = ain->ac2->GetService(__uuidof(IAudioRenderClient), (void**)&ain->ren);
                if (!ain->ren)
                    continue;
            }

            hr = ain->ac->Start();
            if (FAILED(hr))
                continue;

            ain->wfx = wfx;
            if (ain->ren)
            {
                hr = ain->ac2->Start();
                if (FAILED(hr))
                    continue;
                std::thread(&AUDIOIN::PlaySilence, ain.get(), rt).detach();
            }
            else
                ain->CapturingFin2 = true;
            std::thread(&AUDIOIN::ThreadLoopCapture, ain.get()).detach();
            ains.push_back(ain);
        }
    }

    if (ains.empty())
        dp.HasAudio = 0;

    CComPtr<IMFMediaType> pMediaTypeOutAudio;
    DWORD OutAudioStreamIndex = 0;



    if (dp.HasAudio)
    {
        hr = MFCreateMediaType(&pMediaTypeOutAudio);
        if (FAILED(hr)) return -6;

        hr = pMediaTypeOutAudio->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        if (FAILED(hr)) return -7;


        // mp3（不指定编码器时的默认输出）
        if (dp.AUDIO_ENCODING_FORMAT == MFAudioFormat_MP3)
        {
            pMediaTypeOutAudio->SetGUID(MF_MT_SUBTYPE, dp.AUDIO_ENCODING_FORMAT);
            pMediaTypeOutAudio->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, dp.NCH);
            pMediaTypeOutAudio->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, dp.SR);
            pMediaTypeOutAudio->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, dp.ABR*1000 / 8);
        }

        // aac
        if (dp.AUDIO_ENCODING_FORMAT == MFAudioFormat_AAC || dp.AUDIO_ENCODING_FORMAT == MFAudioFormat_FLAC)
        {
            pMediaTypeOutAudio->SetGUID(MF_MT_SUBTYPE, dp.AUDIO_ENCODING_FORMAT);
            pMediaTypeOutAudio->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, dp.NCH);
            pMediaTypeOutAudio->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            pMediaTypeOutAudio->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, dp.SR);
            pMediaTypeOutAudio->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, dp.ABR*1000 / 8);
        }

        hr = pSinkWriter->AddStream(pMediaTypeOutAudio, &OutAudioStreamIndex);
        if (FAILED(hr)) return -8;
    }



    hr = MFCreateMediaType(&pMediaTypeVideoIn);
    if (FAILED(hr)) 
        return -9;
    pMediaTypeVideoIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);

    MFSetAttributeSize(pMediaTypeVideoIn, MF_MT_FRAME_SIZE, encW, encH);
    MFSetAttributeRatio(pMediaTypeVideoIn, MF_MT_FRAME_RATE, dp.fps, 1);
    MFSetAttributeRatio(pMediaTypeVideoIn, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    // 部分硬件编码器的 SetInputMediaType 会因为输入类型缺这个属性而拒绝（MF_E_INVALIDMEDIATYPE），
    // 之后悄悄退回软件编码。补上，避免莫名其妙地掉到软编。
    pMediaTypeVideoIn->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);




    // 输入固定为 RGB32（BGRA8 全范围）：色调映射已在采集出口完成，
    // RGB→YUV 与范围收缩交给 SinkWriter 自带的颜色转换。
    pMediaTypeVideoIn->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (dp.HasVideo && !dp.Framer)
    {
        hr = pSinkWriter->SetInputMediaType(OutVideoStreamIndex, pMediaTypeVideoIn, NULL);
        if (FAILED(hr)) return -10;

        CComPtr<ICodecAPI> ca;
        hr = pSinkWriter->GetServiceForStream(OutVideoStreamIndex, GUID_NULL, __uuidof(ICodecAPI), (void**)&ca);
        if (ca)
        {
            int thr2 = dp.NumThreads;
            if (thr2)
            {
                VARIANT v = {};
                v.vt = VT_UI4;
                v.ulVal = thr2;
                ca->SetValue(&CODECAPI_AVEncNumWorkerThreads, &v);
            }
            int qu = dp.Qu;
            if (qu >= 0 && qu <= 100)
            {
                VARIANT v = {};
                v.vt = VT_UI4;
                v.ulVal = qu;
                ca->SetValue(&CODECAPI_AVEncCommonQualityVsSpeed, &v);
            }

            if (dp.vbrm > 0)
            {
                VARIANT v = {};
                v.vt = VT_UI4;
                if (dp.vbrm == 1)
                    v.ulVal = eAVEncCommonRateControlMode_UnconstrainedVBR;
                if (dp.vbrm == 2)
                    v.ulVal = eAVEncCommonRateControlMode_Quality;
                if (dp.vbrm == 3)
                    v.ulVal = eAVEncCommonRateControlMode_CBR;
                ca->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v);
                if (dp.vbrm == 2)
                {
                    VARIANT v7 = {};
                    v7.vt = VT_UI4;
                    v7.ulVal = dp.vbrq;
                    ca->SetValue(&CODECAPI_AVEncCommonQuality, &v7);

                    // 实测（同素材重编对比）：Quality 模式不看码率参数，只花掉预算的 25%。
                    // 上限与平均目标一起压进去，让编码器把这段预算用掉；
                    // 注意只有在 VBR/CBR 下它们才生效（CapVideo 里已把 vbrm 设为 UnconstrainedVBR）。
                    VARIANT vmax = {};
                    vmax.vt = VT_UI4;
                    vmax.ulVal = (ULONG)dp.BR * 1000u;   // bps
                    ca->SetValue(&CODECAPI_AVEncCommonMaxBitRate, &vmax);
                    VARIANT vmean = {};
                    vmean.vt = VT_UI4;
                    vmean.ulVal = (ULONG)dp.BR * 1000u;
                    ca->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &vmean);
                }
            }
        }
    }

    CComPtr<IMFMediaType> pMediaTypeAudioIn;
    if (dp.HasAudio)
    {
        hr = MFCreateMediaType(&pMediaTypeAudioIn);
        if (FAILED(hr)) return -11;

        pMediaTypeAudioIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        pMediaTypeAudioIn->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        pMediaTypeAudioIn->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, dp.NCH);
        pMediaTypeAudioIn->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, dp.SR);
        pMediaTypeAudioIn->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        int BA = (int)((16 / 8) * dp.NCH);
        pMediaTypeAudioIn->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, BA);
        pMediaTypeAudioIn->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32)(dp.SR * BA));

        hr = pSinkWriter->SetInputMediaType(OutAudioStreamIndex, pMediaTypeAudioIn, NULL);
        if (FAILED(hr)) return -12;
    }


    // 出口统一 BGRA8：无论 HDR 还是 SDR，编码器输入都是 4 字节/像素
    const int multi = 4;

    if (!dp.Framer)
    {
        if (pSinkWriter)
            hr = pSinkWriter->BeginWriting();
        if (FAILED(hr)) return -13;
    }

    const LONG cbWidth = multi * encW;
    DWORD VideoBufferSize = cbWidth * encH;
    CComPtr<IMFMediaBuffer> pVideoBuffer = NULL;
    CComPtr<IMFSample> pVideoSampleS = NULL;

    unsigned long long rtA = 0;
    unsigned long long rtV = 0;
    std::chrono::system_clock::time_point last_frame_time_point;
    bool isFirstFrame = true;

    [[maybe_unused]] unsigned long long msloop = 1000 / dp.fps;

    std::vector<char> AudioData;
    std::vector<std::vector<float>> v(dp.NCH);
    std::vector<MIXBUFFER<float>> mixv(dp.NCH);
    for (;;)
    {
        if (!dp.HasAudio && !dp.Framer)
            Sleep((DWORD)msloop);
        if (dp.MustEnd)
            break;

        // Windows 的「SDR 内容亮度」可能在录制中被调整；定期刷新，避免整段录屏沿用旧白点。
        if (cap.InHDR != DXGI_FORMAT_UNKNOWN
            && GetTickCount64() - cap.lastWhiteRefresh > 2000)
            cap.refreshSdrWhite();

        unsigned long long ThisDurA = 0;
        unsigned long long ThisDurV = 0;
        size_t FinalAudioBytes = 0;

        if (dp.HasAudio)
        {
            // 等所有音频端点都至少有数据可抓
            if (!AudioEvents.empty())
                WaitForMultipleObjects((DWORD)AudioEvents.size(), AudioEvents.data(), TRUE, INFINITE);

            size_t AvailableSamples = 0;
            for (auto& ain : ains)
            {
                auto frm = ain->AvFrames();
                if (frm < AvailableSamples || AvailableSamples == 0)
                    AvailableSamples = frm;
            }

            // Pop 出来按声道拆成 float；样本统一是 16bit，混音端不再分位深
            int TotalBuffers = 0;
            for (auto& ain : ains)
            {
                if (AvailableSamples == 0)
                    break;
                const size_t ThisAudioBytes = AvailableSamples * dp.NCH * sizeof(short);

                ain->PopBuffer.resize(ThisAudioBytes);
                ain->AudioDataX->PopX(ain->PopBuffer.data(), ThisAudioBytes);

                const short* s = (const short*)ain->PopBuffer.data();
                ain->buffer.resize(dp.NCH);
                for (int ch = 0; ch < dp.NCH; ch++)
                {
                    auto& aa = ain->buffer[ch];
                    if (!aa)
                        aa = std::make_shared<REBUFFER>();
                    ain->tempf.resize(AvailableSamples);
                    for (size_t j = 0; j < AvailableSamples; j++)
                        ain->tempf[j] = s[j * dp.NCH + ch] / 32768.0f;

                    aa->PushX((const char*)ain->tempf.data(), AvailableSamples * sizeof(float));
                    TotalBuffers++;
                }
            }

            for (int i = 0; i < dp.NCH; i++)
                v[i].resize(AvailableSamples);

            if (TotalBuffers == dp.NCH)
            {
                // 只有一路音源（最常见：系统声音），按声道直接拷贝
                int ib = 0;
                for (auto& ain : ains)
                {
                    for (auto& b : ain->buffer)
                    {
                        if (!AvailableSamples)
                            continue;
                        b->PopX((char*)v[ib].data(), AvailableSamples * sizeof(float));
                        ib++;
                    }
                }
            }
            else
            {
                // 麦克风 + 系统声音：各路都摊到每个声道上再合回去
                for (int ich = 0; ich < dp.NCH; ich++)
                {
                    mixv[ich].Set(v[ich].data());
                    mixv[ich].Reset(AvailableSamples);
                }

                for (auto& ain : ains)
                {
                    if (AvailableSamples == 0)
                        continue;
                    for (auto& b : ain->buffer)
                    {
                        if (!AvailableSamples)
                            continue;
                        b->PopX((char*)ain->tempf.data(), AvailableSamples * sizeof(float));
                        for (int ich = 0; ich < dp.NCH; ich++)
                            mixv[ich].Put(ain->tempf.data(), AvailableSamples);
                    }
                }
                for (int ich = 0; ich < dp.NCH; ich++)
                    mixv[ich].Fin(AvailableSamples);
            }

            // 混音结果是 ±1 的 float，这里转回 16bit 样本
            FinalAudioBytes = AvailableSamples * dp.NCH * 2;
            AudioData.resize(FinalAudioBytes);
            short* ad = (short*)AudioData.data();
            for (size_t i = 0; i < AvailableSamples; i++)
            {
                for (int nch = 0; nch < dp.NCH; nch++)
                {
                    float V = v[nch][i];
                    short S = (short)(V * 32768.0f);
                    ad[i * dp.NCH + nch] = S;
                }
            }

            // 样本数 → 100ns：SR 个样本算 1 秒
            unsigned long long  ms = 1000 * AvailableSamples;
            ms *= 10000;
            ms /= dp.SR;
            ThisDurA = ms;
        }

        ThisDurV = 10 * 1000 * 1000 / dp.fps;
        if (ThisDurA)
            ThisDurV = ThisDurA;

        if (dp.HasAudio && ThisDurA == 0)
            continue;

        if (dp.Pause)
            continue;

        CComPtr<IDXGIResource> lDesktopResource;
        DXGI_OUTDUPL_FRAME_INFO lFrameInfo;

        // 取下一帧；超时不算失败，没有新画面就继续循环
        BYTE* pData = NULL;
        HRESULT hrf = S_FALSE;
        if (dp.HasVideo)
        {
            if (cap.gdiMode)
            {
                // GDI 兜底：跟随只平移抓取原点（帧尺寸不可变），抓帧本身没有任何 COM 调用
                if (dp.followOn.load(std::memory_order_relaxed))
                {
                    const long long packed = dp.wantOrigin.load(std::memory_order_relaxed);
                    const long wx = (long)(int)(packed >> 32);
                    const long wy = (long)(int)(packed & 0xFFFFFFFFll);
                    if (wx != lastWantX || wy != lastWantY)
                    {
                        lastWantX = wx;
                        lastWantY = wy;
                        const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
                        const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
                        const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
                        const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
                        long l = wx, t = wy;
                        if (l < vx) l = vx;
                        if (t < vy) t = vy;
                        if (vw >= followCropW && l > vx + vw - followCropW) l = vx + vw - followCropW;
                        if (vh >= followCropH && t > vy + vh - followCropH) t = vy + vh - followCropH;
                        cap.gdiRect = { l, t, l + followCropW, t + followCropH };
                    }
                }
                if (!cap.GrabGdi(dp.Cursor, cap.gdiRect))
                    break;
            }
            else
            {
                // 换屏跟随：选区移到别的显示器时切换采集目标继续录制（只平移，帧尺寸不变）。
                // 位置必须在 AcquireNextFrame **之前** —— 换屏会释放并重建复制对象，此刻不能持有上一帧，
                // 否则随后的 ReleaseFrame 会作用到新对象上，帧数据也可能取自另一块屏。
                // 目标屏挂在别的适配器上时无法切换 → 保持当前屏，不做部分跟随。
                if (dp.followOn.load(std::memory_order_relaxed))
                {
                    const long long packed = dp.wantOrigin.load(std::memory_order_relaxed);
                    const long wx = (long)(int)(packed >> 32);
                    const long wy = (long)(int)(packed & 0xFFFFFFFFll);
                    if (wx != lastWantX || wy != lastWantY)
                    {
                        lastWantX = wx;
                        lastWantY = wy;
                        const long dx = wx + followBiasX;   // 期望的裁剪原点（虚拟桌面坐标）
                        const long dy = wy + followBiasY;
                        POINT mid{ (LONG)(dx + followCropW / 2), (LONG)(dy + followCropH / 2) };
                        HMONITOR hm = MonitorFromPoint(mid, MONITOR_DEFAULTTONEAREST);
                        MONITORINFO mi{ sizeof(MONITORINFO) };
                        if (hm && GetMonitorInfoW(hm, &mi))
                        {
                            // 采集帧尺寸不可变：目标屏装不下裁剪框就**不换**（换过去必然裁到屏外）。
                            // 选区那侧同一条取舍（CutMask::clampToSingleMonitor）—— 两边行为一致。
                            const bool fits = followCropW <= (mi.rcMonitor.right - mi.rcMonitor.left)
                                && followCropH <= (mi.rcMonitor.bottom - mi.rcMonitor.top);
                            if (fits && !EqualRect(&mi.rcMonitor, &cap.monRect))
                            {
                                UINT idx = 0;
                                if (CAPTURE::FindOutputOnDeviceAdapter(cap.device, mi.rcMonitor, idx)
                                    && cap.Prepare(idx))
                                {
                                    cap.monRect = mi.rcMonitor;
                                    dp.nOutput = idx;   // ACCESS_LOST 后的重建沿用该索引
                                }
                                else
                                {
                                    // 切换失败：记录该显示器，UI 侧随后会禁止选区再移向这里
                                    dp.switchBlocked.store(
                                        ((long long)(int)mi.rcMonitor.left << 32)
                                        | (long long)(unsigned int)(int)mi.rcMonitor.top,
                                        std::memory_order_relaxed);
                                    dp.hasBlocked.store(true, std::memory_order_relaxed);
                                }
                            }
                            if (EqualRect(&mi.rcMonitor, &cap.monRect))
                            {
                                // 换算成当前屏的局部裁剪框并夹进复制纹理（尺寸仍是开录时定下的）
                                long l = dx - cap.monRect.left;
                                long t = dy - cap.monRect.top;
                                const long maxL = (long)cap.lOutputDuplDesc.ModeDesc.Width - followCropW;
                                const long maxT = (long)cap.lOutputDuplDesc.ModeDesc.Height - followCropH;
                                if (l < 0) l = 0; else if (l > maxL) l = maxL > 0 ? maxL : 0;
                                if (t < 0) t = 0; else if (t > maxT) t = maxT > 0 ? maxT : 0;
                                dp.rx = { l, t, l + followCropW, t + followCropH };
                            }
                        }
                    }
                }

                hr = cap.lDeskDupl->AcquireNextFrame(
                    0,
                    &lFrameInfo,
                    &lDesktopResource);
                if (hr == DXGI_ERROR_WAIT_TIMEOUT)
                    hr = S_OK;
                if (hr == DXGI_ERROR_ACCESS_LOST)
                {
                    cap.lDeskDupl = 0;
                    bool C = false;
                    for (int i = 0; i < 10; i++)
                    {
                        if (cap.Prepare(dp.nOutput))
                        {
                            C = true;
                            break;
                        }
                        Sleep(250);
                    }
                    if (!C)
                        break;
                    hr = S_OK;
                }
                if (FAILED(hr))
                    break;



                // 抓帧并拷进内存；有裁剪区时只拷选区那一块
                if (lDesktopResource && !cap.Get(lDesktopResource, dp.Cursor, dp.rx.right && dp.rx.bottom ? &dp.rx : 0))
                    break;
            }

            if (isFirstFrame)
            {
                isFirstFrame = false;
                rtV = 0;
                last_frame_time_point = std::chrono::system_clock::now();
            }
            else
            {
                // 用与上一帧的实际间隔当帧时长：掉帧时累计时长也不会漂移
                auto current_frameTimePoint = std::chrono::system_clock::now();
                auto frameDuration = current_frameTimePoint - last_frame_time_point;
                last_frame_time_point = current_frameTimePoint;
                ThisDurV = frameDuration.count();
                rtV += ThisDurV;
            }


            if (!pVideoBuffer)
            {
                hr = MFCreateMemoryBuffer(VideoBufferSize, &pVideoBuffer);
                if (FAILED(hr))
                    break;
            }

            hr = pVideoBuffer->Lock(&pData, NULL, NULL);
            if (FAILED(hr))
                break;

            // 采集出口已经统一成 BGRA8：HDR 帧先按当前 SDR 白点做色调映射，
            // 再和普通帧一样缩放/拷贝——编码器只认这一种格式。
            if (cap.InHDR != DXGI_FORMAT_UNKNOWN)
            {
                if (!cap.toneMapHdrToBgra(wi, he))
                    break;
                // HDR 帧没有 GDI 兼容表面，光标改叠在色调映射后的画面上
                if (dp.Cursor)
                {
                    const int ox = cap.monRect.left + ((dp.rx.right && dp.rx.bottom) ? dp.rx.left : 0);
                    const int oy = cap.monRect.top + ((dp.rx.right && dp.rx.bottom) ? dp.rx.top : 0);
                    cap.blendCursor(cap.sdrBuf, wi, he, ox, oy);
                }
                if (encW != wi || encH != he)
                    scaleBGRABox(cap.sdrBuf.data(), wi, he, (BYTE*)pData, encW, encH);
                else
                    memcpy(pData, cap.sdrBuf.data(), (size_t)VideoBufferSize);
            }
            else if (encW != wi || encH != he)
            {
                // 「视频质量」缩到目标分辨率后再交给编码器
                scaleBGRABox(cap.buf.data(), wi, he, (BYTE*)pData, encW, encH);
            }
            else
            {
                memcpy(pData, cap.buf.data(), (size_t)VideoBufferSize);
            }
            if (dp.Framer)
                hrf = dp.Framer(DXGI_FORMAT_B8G8R8A8_UNORM, wi, he, pData, (size_t)VideoBufferSize, dp.cb);
            hr = pVideoBuffer->Unlock();
            if (FAILED(hr)) break;

            hr = pVideoBuffer->SetCurrentLength(VideoBufferSize);
            if (FAILED(hr)) break;

            CComPtr<IMFSample> pVideoSample = NULL;

            if (!pVideoSampleS)
            {
                hr = MFCreateSample(&pVideoSampleS);
                if (FAILED(hr)) break;
            }
            pVideoSample = pVideoSampleS;


            pVideoSample->RemoveAllBuffers();
            hr = pVideoSample->AddBuffer(pVideoBuffer);
            if (FAILED(hr)) break;

            if (cap.lDeskDupl)
                cap.lDeskDupl->ReleaseFrame();




            if (pVideoSample && dp.HasVideo && !dp.Framer)
            {
                hr = pVideoSample->SetSampleTime(rtV);
                if (FAILED(hr)) break;

                hr = pVideoSample->SetSampleDuration(ThisDurV);
                if (FAILED(hr)) break;
                hr = pSinkWriter->WriteSample(OutVideoStreamIndex, pVideoSample);
                if (FAILED(hr))
                    break;
                dp.framesWritten++;
            }
        }

        // 音频：本帧有可写样本时才建缓冲
        CComPtr<IMFMediaBuffer> pAudioBuffer = NULL;
        if (dp.HasAudio && FinalAudioBytes)
        {
            hr = MFCreateMemoryBuffer((DWORD)FinalAudioBytes, &pAudioBuffer);
            if (FAILED(hr))
                break;

            hr = pAudioBuffer->Lock(&pData, NULL, NULL);
            if (FAILED(hr))
                break;

            memcpy(pData, AudioData.data(), FinalAudioBytes);

            hr = pAudioBuffer->Unlock();
            if (FAILED(hr))
                break;

            hr = pAudioBuffer->SetCurrentLength((DWORD)FinalAudioBytes);
            if (FAILED(hr))
                break;

            CComPtr<IMFSample> pAudioSample = NULL;
            hr = MFCreateSample(&pAudioSample);
            if (FAILED(hr))
                break;

            hr = pAudioSample->AddBuffer(pAudioBuffer);
            if (FAILED(hr))
                break;

            hr = pAudioSample->SetSampleTime(rtA);
            if (FAILED(hr))
                break;

            hr = pAudioSample->SetSampleDuration(ThisDurA);
            if (FAILED(hr))
                break;

            hr = pSinkWriter->WriteSample(OutAudioStreamIndex, pAudioSample);
            if (FAILED(hr))
                break;

            rtA += ThisDurA;
        }

        // EndMS 是毫秒，rtV 是 100ns，换算后再比
        auto rvx = rtV / 10000;
        if (dp.EndMS > 0 && rvx >= dp.EndMS)
            break;

        if (dp.Framer && hrf == S_OK)
            break;
    }

    // 收工：停掉所有音频客户端，线程由 AINRELEASE 负责回收
    for (auto& a6 : ains)
    {
        if (a6->ac)
            a6->ac->Stop();
    }

    if (pSinkWriter)
        hr = pSinkWriter->Finalize();
    if (FAILED(hr))
        return -14;
    // 循环里每一处失败都是 break，跟"用户点了停止"走同一个出口，之后照样 Finalize ——
    // 而 Finalize 一个没写过样本的 MP4 是会成功的，于是调用方拿到 0 却得到个 0 字节文件，
    // 一句报错都没有。既然一帧都没写就不可能是有效视频，这里明确报错。
    // 但"刚开录就点了停止"同样是一帧都没有，那是用户自己清楚的操作，不该拿报错去打扰他 ——
    // 那种情况 MustEnd 已经立起来了，交给调用方照 framesWritten 静静收场
    if (dp.HasVideo && !dp.Framer && dp.framesWritten == 0 && !dp.MustEnd)
        return -15;
	return 0;
}

}