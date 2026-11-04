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


namespace VideoMp4{
// {1884E6DC-2AE3-4499-A3D7-1A27E3B2AC39}
inline static const GUID MyFakeFmt = { 0x1884e6dc, 0x2ae3, 0x4499, { 0xa3, 0xd7, 0x1a, 0x27, 0xe3, 0xb2, 0xac, 0x39 } };

// {7F478D2D-9EBF-4B9A-B2F3-83800263BB86}
inline static const GUID MFVideoFormat_RGB10 = { 0x7f478d2d, 0x9ebf, 0x4b9a, { 0xb2, 0xf3, 0x83, 0x80, 0x2, 0x63, 0xbb, 0x86 } };


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

        // If null, do nothing
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
template <typename T = float>
class READYBUFF
{
public:

    std::vector<T> g;

    void Add(T* d, size_t sz)
    {
        auto t = g.size();
        g.resize(t + sz);
        memcpy(g.data() + t, d, sz * sizeof(T));
    }

    bool Next(T* d, size_t sz)
    {
        if (g.empty())
            return false;
        if (!sz)
            return false;
        memcpy(d, g.data(), sz * sizeof(T));

        if (g.size() < sz)
            g.clear();
        else
            g.erase(g.begin(), g.begin() + sz);
        return true;
    }

};
inline HRESULT MFTrs(DWORD, DWORD iid, DWORD ood, CComPtr<IMFTransform> trs, CComPtr<IMFSample> s, IMFSample** sx)
{
    if (!trs)
        return E_FAIL;
    auto hr = s ? trs->ProcessInput(iid, s, 0) : S_OK;
    if (SUCCEEDED(hr) || hr == 0xC00D36B5)
    {
        bool FX = false;
        if (hr == 0xC00D36B5)
            FX = true;

        MFT_INPUT_STREAM_INFO six = {};
        trs->GetInputStreamInfo(iid, &six);

        MFT_OUTPUT_STREAM_INFO si = {};
        CComPtr<IMFMediaBuffer> bb;
        if (si.cbSize == 0)
            trs->GetOutputStreamInfo(ood, &si);
        CComPtr<IMFSample> pSample2;
        if ((si.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0)
        {
            if (si.cbSize == 0)
            {
                return E_FAIL;
            }
            MFCreateSample(&pSample2);
            MFCreateMemoryBuffer(si.cbSize, &bb);
            pSample2->AddBuffer(bb);
        }

        MFT_OUTPUT_DATA_BUFFER db = { 0 };
        db.dwStreamID = ood;
        db.pSample = pSample2;
        if (si.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)
            db.pSample = 0;

        DWORD st = 0;
        hr = trs->ProcessOutput(0, 1, &db, &st);
        if (hr == 0xC00D6D72 && FX)
        {
            hr = trs->ProcessInput(iid, s, 0);
            hr = trs->ProcessOutput(0, 1, &db, &st);
        }
        if (FAILED(hr))
            return hr;
        if (db.pEvents)
            db.pEvents->Release();
        if (!db.pSample)
            return E_FAIL;
        if (sx)
        {
            *sx = db.pSample;
            if ((si.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0)
                (*sx)->AddRef();

        }
        return S_OK;
    }
    return hr;
}
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
    int ConvertHDR = 2; // 0 no convert, 1 convert, 1 convert and go updown
    // 是否成功要到了"系统帮我转成 8 位"的桌面复制，见 Prepare
    bool SDRForced = false;
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

        // Driver types supported
        D3D_DRIVER_TYPE DriverTypes[] =
        {
            D3D_DRIVER_TYPE_HARDWARE,
            D3D_DRIVER_TYPE_WARP,
            D3D_DRIVER_TYPE_REFERENCE,
        };
        UINT NumDriverTypes = ARRAYSIZE(DriverTypes);

        // Feature levels supported
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

        // Create device
        for (UINT DriverTypeIndex = 0; DriverTypeIndex < NumDriverTypes; ++DriverTypeIndex)
        {
            hr = D3D11CreateDevice(g, DriverTypes[DriverTypeIndex], nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, FeatureLevels, NumFeatureLevels,
                D3D11_SDK_VERSION, &device, &FeatureLevel, &context);
            if (SUCCEEDED(hr))
            {
                // Device creation success, no need to loop anymore
                break;
            }
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
        // QI for ID3D11Texture2D
        CComPtr<ID3D11Texture2D> lAcquiredDesktopImage;
        if (!lDesktopResource)
            return 0;
        auto hr = lDesktopResource->QueryInterface(IID_PPV_ARGS(&lAcquiredDesktopImage));
        if (!lAcquiredDesktopImage)
            return 0;
        lDesktopResource = 0;


        if (InHDR != DXGI_FORMAT_UNKNOWN || Curs == 0)
        {
            // No Cursor support
            context->CopyResource(lDestImage, lAcquiredDesktopImage);
        }
        else
        {
            // Copy image into GDI drawing texture
            context->CopyResource(lGDIImage, lAcquiredDesktopImage);

            // Draw cursor image into GDI drawing texture
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
                        auto lCursorPosition = lCursorInfo.ptScreenPos;
                        DrawIconEx(
                            lHDC,
                            lCursorPosition.x- iconInfo.xHotspot,
                            lCursorPosition.y- iconInfo.yHotspot,
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

            // Copy image into CPU access texture
            context->CopyResource(lDestImage, lGDIImage);
        }


        // Copy from CPU access texture to bitmap buffer
        D3D11_MAPPED_SUBRESOURCE resource;
        UINT subresource = D3D11CalcSubresource(0, 0, 0);
        hr = context->Map(lDestImage, subresource, D3D11_MAP_READ_WRITE, 0, &resource);
        if (FAILED(hr))
            return 0;

        int multi = 4;
        if (InHDR == DXGI_FORMAT_R16G16B16A16_FLOAT)
            multi = 8; // 64-bit

        auto sz = lOutputDuplDesc.ModeDesc.Width
            * lOutputDuplDesc.ModeDesc.Height * multi;
        auto sz2 = sz;
        if (rcx)
        {
            sz2 = (rcx->right - rcx->left) * (rcx->bottom - rcx->top) * multi;
            sz = sz2;
        }
        // ★ 先算好目标尺寸再 resize 一次：以前先按整屏 resize、再缩到选区尺寸，
        //   vector 的 capacity 会一直停在整屏大小 —— 录一小块区域也要占一整屏内存。
        if (buf.size() != (size_t)sz2) buf.resize(sz2);

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
    // Get DXGI device
        CComPtr<IDXGIDevice> lDxgiDevice;
        lDxgiDevice = device;
        if (!lDxgiDevice)
            return 0;

        // Get DXGI adapter
        HRESULT hr = 0;

        CComPtr<IDXGIAdapter> lDxgiAdapter;
        hr = lDxgiDevice->GetParent(
            __uuidof(IDXGIAdapter),
            reinterpret_cast<void**>(&lDxgiAdapter));

        if (FAILED(hr))
            return 0;

        lDxgiDevice = 0;

        // Get output
        CComPtr<IDXGIOutput> lDxgiOutput;
        hr = lDxgiAdapter->EnumOutputs(Output, &lDxgiOutput);
        if (FAILED(hr))
            return 0;

        lDxgiAdapter = 0;

        DXGI_OUTPUT_DESC lOutputDesc;
        hr = lDxgiOutput->GetDesc(&lOutputDesc);
        monRect = lOutputDesc.DesktopCoordinates;   // 换屏跟随的判据

        // QI for Output 1
        CComPtr<IDXGIOutput1> lDxgiOutput1;
        lDxgiOutput1 = lDxgiOutput;
        if (!lDxgiOutput1)
            return 0;

        lDxgiOutput = 0;

        // Create desktop duplication
        // 开着 HDR 的屏，桌面复制默认给的是 FP16(scRGB)，编码器一个都吃不下，得自己转：
        // 转 10 位只有 HEVC 能收（编码器不是每台机器都有），自己降到 8 位则一片黑（FP16 表面
        // 的 alpha 是未定义的，WIC 从 float 转 8 位时那一遍预乘 alpha 会把画面乘成 0）。
        // DuplicateOutput1 可以直接指定"我只要 BGRA8"，转换和色调映射都交给系统做，于是
        // HDR 屏走的就是和普通屏一样那条验证过的路，连光标叠加都能用上。
        // Win10 1703 才有，且要求进程 per-monitor DPI aware；要不到就退回老接口
        CComPtr<IDXGIOutput5> lDxgiOutput5;
        lDxgiOutput5 = lDxgiOutput1;
        SDRForced = false;
        if (lDxgiOutput5)
        {
            const DXGI_FORMAT want[] = { DXGI_FORMAT_B8G8R8A8_UNORM };
            hr = lDxgiOutput5->DuplicateOutput1(device, 0, 1, want, &lDeskDupl);
            SDRForced = SUCCEEDED(hr);
        }
        if (!SDRForced)
            hr = lDxgiOutput1->DuplicateOutput(
                device,
                &lDeskDupl);

        if (FAILED(hr))
            return 0;

        lDxgiOutput5 = 0;
        lDxgiOutput1 = 0;

        // Create GUI drawing texture
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
        if (desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) // HDR, no GDI compatible
        {
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
        {
//            desc.MiscFlags = 0;
  //          hr = device->CreateTexture2D(&desc, NULL, &lGDIImage);
        }
        if (FAILED(hr))
            return 0;

        if (lGDIImage == nullptr)
            return 0;

        // Create CPU access texture
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

        if (lDestImage == nullptr)
            return 0;

        return 1;
    }


    CComPtr<IWICImagingFactory2> wbfact;
    HRESULT Convert(void* f, int wi, int he,std::vector<DWORD>& out,GUID from = GUID_WICPixelFormat128bppPRGBAFloat)
    {
        if (!wbfact)
            CoCreateInstance(CLSID_WICImagingFactory2, 0, CLSCTX_INPROC_SERVER,
                __uuidof(IWICImagingFactory2), (void**)&wbfact);
        // 工厂建不起来就得就此收手，下面一路都是拿它当非空用的
        if (!wbfact)
            return E_FAIL;

        int multi = 4;
        if (from == GUID_WICPixelFormat128bppPRGBAFloat)
            multi = 16;
        if (from == GUID_WICPixelFormat64bppRGBAHalf)
            multi = 8;
        CComPtr<IWICBitmap> b;
        wbfact->CreateBitmapFromMemory(wi, he, from, wi * multi, wi * he * multi, (BYTE*)f, &b);
        if (!b)
            return E_FAIL;


        CComPtr<IWICFormatConverter> wf;
        wbfact->CreateFormatConverter(&wf);

        if (!wf)
            return E_FAIL;
        auto hr = wf->Initialize(b, GUID_WICPixelFormat32bppRGBA1010102, WICBitmapDitherTypeNone, 0, 0, WICBitmapPaletteTypeCustom);
        if (FAILED(hr))
            return E_FAIL;

        CComPtr<IWICBitmap> wic;
        wbfact->CreateBitmapFromSource(wf, WICBitmapCacheOnLoad, &wic);
        if (!wic)
            return E_FAIL;

        WICRect wr = { 0,0,(INT)wi,(INT)he };
        out.resize(wi * he);
        hr = wic->CopyPixels(&wr, wi * 4, (UINT)out.size() * 4, (BYTE*)out.data());

        return hr;
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
            // ns
            rt /= 10000;
            // in SR , 1000 ms
            //  ?    , rt ms
            auto ns = (wfx.nSamplesPerSec * rt);
            ns /= 1000;
            while (Capturing)
            {
                if (!ren)
                    break;

                Sleep((DWORD)(rt / 2));

                if (!Capturing)
                    break;

                // See how much buffer space is available.
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
            // 桌面复制退回到 FP16 那条路时（Prepare 里要不到 BGRA8），只有 HEVC 收 10 位输入
            dp.LastCapturedHdr = cap.InHDR != DXGI_FORMAT_UNKNOWN;
            if (cap.InHDR)
                dp.VIDEO_ENCODING_FORMAT = MFVideoFormat_HEVC;
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
    // HDR(FP16) 那条路像素格式特殊且极少用，不做缩放，保持原生。
    int encW = wi, encH = he;
    if (dp.HasVideo && !cap.InHDR && dp.MaxOutHeight > 0 && he > dp.MaxOutHeight)
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

/*
    MF_MT_FRAME_SIZE	1920 x 1080
    MF_MT_AVG_BITRATE	185984000
    MF_MT_MAJOR_TYPE	MFMediaType_Video
    MF_MT_VIDEO_NOMINAL_RANGE	1
    MF_MT_FRAME_RATE	30 x 1
    MF_MT_PIXEL_ASPECT_RATIO	1 x 1
    MF_MT_INTERLACE_MODE	2
    MF_MT_SUBTYPE	MFVideoFormat_HEVC


*/
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


        // pcm
/*        if (AUDIO_ENCODING_FORMAT == MFAudioFormat_PCM)
        {
            pMediaTypeOutAudio->SetGUID(MF_MT_SUBTYPE, vp.AUDIO_TYPE);
            pMediaTypeOutAudio->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, vp.AUDIO_CHANNELS);
            pMediaTypeOutAudio->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, vp.AUDIO_SAMPLE_RATE);
            pMediaTypeOutAudio->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, vp.AUDIO_BIT_RATE / 8);
            int BA = (int)((vp.AUDIO_INPUT_BITS / 8) * vp.AUDIO_INPUT_CHANNELS);
            pMediaTypeOutAudio->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, BA);
            pMediaTypeOutAudio->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, vp.AUDIO_INPUT_BITS);
        }
*/
        // mp3
        if (dp.AUDIO_ENCODING_FORMAT == MFAudioFormat_MP3)
        {
            pMediaTypeOutAudio->SetGUID(MF_MT_SUBTYPE, dp.AUDIO_ENCODING_FORMAT);
            pMediaTypeOutAudio->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, dp.NCH);
            pMediaTypeOutAudio->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, dp.SR);
            pMediaTypeOutAudio->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, dp.ABR*1000 / 8);
        }

/*        // wma
        if (vp.AUDIO_TYPE == MFAudioFormat_WMAudioV9)
        {
            pMediaTypeOutAudio->SetGUID(MF_MT_SUBTYPE, vp.AUDIO_TYPE);
            pMediaTypeOutAudio->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, vp.AUDIO_CHANNELS);
            pMediaTypeOutAudio->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, vp.AUDIO_SAMPLE_RATE);
        }
*/
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




    // NV12 converter
    CComPtr<IMFTransform> VideoTransformMFT;
    bool NV12 = 0;
    if (false)
    {
        CComPtr<IMFTransform> trs;
        trs.CoCreateInstance(CLSID_VideoProcessorMFT);
        std::vector<DWORD> iids;
        std::vector<DWORD> oods;
        DWORD is = 0, os = 0;
        hr = trs->GetStreamCount(&is, &os);
        iids.resize(is);
        oods.resize(os);
        hr = trs->GetStreamIDs(is, iids.data(), os, oods.data());
        CComPtr<IMFMediaType> ptype;
        CComPtr<IMFMediaType> ptype2;
        MFCreateMediaType(&ptype);
        MFCreateMediaType(&ptype2);
        pMediaTypeVideoIn->CopyAllItems(ptype);
        pMediaTypeVideoIn->CopyAllItems(ptype2);

        PROPVARIANT pv;
        InitPropVariantFromCLSID(MFVideoFormat_RGB32, &pv);
        ptype->SetItem(MF_MT_SUBTYPE, pv);

        PROPVARIANT pv2;
        InitPropVariantFromCLSID(MFVideoFormat_NV12, &pv2);
        ptype2->SetItem(MF_MT_SUBTYPE, pv2);

        hr = trs->SetInputType(iids[0], ptype, 0);
        auto hr2 = trs->SetOutputType(oods[0], ptype2, 0);
        if (SUCCEEDED(hr) && SUCCEEDED(hr2))
        {
            VideoTransformMFT = trs;
            pMediaTypeVideoIn->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
            NV12 = true;
        }
    }

    if (!NV12)
        pMediaTypeVideoIn->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);

    // FP16 那条路才要给编码器喂 10 位输入，下面这套门道也只在那时候走
    const bool hdr10 = cap.InHDR != DXGI_FORMAT_UNKNOWN;

    if (hdr10)
    {
        // Force our Nvidia MFT encoder to load by calling first our fake guid
        pMediaTypeVideoIn->SetGUID(MF_MT_SUBTYPE, MyFakeFmt);
    }

    if (dp.HasVideo && !dp.Framer)
    {
        hr = pSinkWriter->SetInputMediaType(OutVideoStreamIndex, pMediaTypeVideoIn, NULL);
        if (FAILED(hr)) return -10;


        if (hdr10)
        {
            // Now put the real value
            pMediaTypeVideoIn->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB10);
            hr = pSinkWriter->SetInputMediaType(OutVideoStreamIndex, pMediaTypeVideoIn, NULL);
            if (FAILED(hr)) return -10;
        }

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


    int multi = 4;
    if (cap.InHDR != DXGI_FORMAT_UNKNOWN)
        multi = 16;

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
   
#ifdef _DEBUG
    wchar_t ydebug[300] = { 0};
#endif
    for (;;)
    {
        if (!dp.HasAudio && !dp.Framer)
            Sleep((DWORD)msloop);
        if (dp.MustEnd)
            break;

        unsigned long long ThisDurA = 0;
        unsigned long long ThisDurV = 0;
        size_t FinalAudioBytes = 0;

        if (dp.HasAudio)
        {
            // Wait for all the events
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

            // v to shorts
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

            // In SR , 1000 ms
            // In ThisAudioSamples, ?
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

        // Get new frame
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



                // take a time stamp here
                // Get the current time point
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
                // Time stamp of current frame.
                // Calculate the dt with last frame. and set to the frame property.
                auto current_frameTimePoint = std::chrono::system_clock::now();
                auto frameDuration = current_frameTimePoint - last_frame_time_point;
                last_frame_time_point = current_frameTimePoint;
                ThisDurV = frameDuration.count();
                rtV += ThisDurV;
            }


            std::vector<DWORD> hdrout;


            if (cap.InHDR == DXGI_FORMAT_R32G32B32A32_FLOAT)
            {
                // This is a 16-bit 4xfloat sample
                void* fld = cap.buf.data();
                auto hr2 = cap.Convert(fld, wi, he, hdrout, GUID_WICPixelFormat128bppPRGBAFloat);
                if (FAILED(hr2))
                    break;
                VideoBufferSize = (DWORD)(hdrout.size() * 4);
            }
            if (cap.InHDR == DXGI_FORMAT_R16G16B16A16_FLOAT && cap.ConvertHDR)
            {
                // 64-bit float 16 bit each float, convert
                void* fld = cap.buf.data();
                auto hr2 = cap.Convert(fld, wi, he, hdrout, GUID_WICPixelFormat64bppRGBAHalf);
                if (FAILED(hr2))
                    break;
                VideoBufferSize = (DWORD)(hdrout.size() * 4);
                // Up down

                if (cap.ConvertHDR == 2)
                {
                    std::vector<DWORD> raw3;
                    raw3.resize(wi);
                    for (int y = 0; y < he / 2; y++)
                    {
                        DWORD* d = hdrout.data() + y * wi;
                        DWORD* d2 = hdrout.data() + (he - y - 1) * wi;
                        memcpy(raw3.data(), d, wi * sizeof(DWORD));
                        memcpy(d, d2, wi * sizeof(DWORD));
                        memcpy(d2, raw3.data(), wi * sizeof(DWORD));
                    }
                }
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

            if (cap.InHDR != DXGI_FORMAT_UNKNOWN)
            {
                memcpy(pData, hdrout.data(), VideoBufferSize);
            }
            else if (encW != wi || encH != he)
            {
                // 「视频质量」缩到目标分辨率后再交给编码器
                scaleBGRABox(cap.buf.data(), wi, he, (BYTE*)pData, encW, encH);
            }
            else
            {
                memcpy(pData, cap.buf.data(), std::min(cap.buf.size(), (size_t)VideoBufferSize));
            }
            if (dp.Framer)
                hrf = dp.Framer(cap.InHDR,wi,he,pData, std::min(cap.buf.size(), (size_t)VideoBufferSize), dp.cb);
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




            hr = pVideoSample->SetSampleTime(rtV);
            if (FAILED(hr)) break;

            hr = pVideoSample->SetSampleDuration(ThisDurV);
            if (FAILED(hr)) break;


            // Video Transform MFT
            if (VideoTransformMFT)
            {
                CComPtr<IMFSample> s2;
                MFTrs(0, 0, 0, VideoTransformMFT, pVideoSample, &s2);
                if (s2)
                    pVideoSample = s2;
            }

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

        // Audio 
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

#ifdef _DEBUG
            swprintf_s(ydebug, 300, L"Writing audio at %llu - %llu...", rtA, ThisDurA);
            OutputDebugString(ydebug);
#endif
            hr = pSinkWriter->WriteSample(OutAudioStreamIndex, pAudioSample);
#ifdef _DEBUG
            OutputDebugString(L"OK\r\n");
#endif
            if (FAILED(hr))
                break;

            rtA += ThisDurA;
        }

        // rtv to ms
        auto rvx = rtV / 10000;
        if (dp.EndMS > 0 && rvx >= dp.EndMS)
            break;

        if (dp.Framer && hrf == S_OK)
            break;
    }

    // Audio off
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