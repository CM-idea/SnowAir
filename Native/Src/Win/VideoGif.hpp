#pragma once

#include <cstdio>
#include <atomic>
#include "pch.h"
#include "Util.h"
#include "cgif/cgif.h"

namespace VideoGif {

    struct GifParam
    {
        std::atomic<bool> isFinish;
        std::atomic<bool> isPaused{ false };
        std::wstring path;
        int w;
        int h;
        int x;
        int y;
        UINT fps{16};
    };

    inline void drawCursor(HDC hMemDC, GifParam* param) {
        CURSORINFO cursorInfo = { sizeof(CURSORINFO) };
        GetCursorInfo(&cursorInfo);
        if (cursorInfo.flags == CURSOR_SHOWING) {
            ICONINFO iconInfo;
            GetIconInfo(cursorInfo.hCursor, &iconInfo);
            int localX = cursorInfo.ptScreenPos.x - param->x - iconInfo.xHotspot;
            int localY = cursorInfo.ptScreenPos.y - param->y - iconInfo.yHotspot;
            if (iconInfo.hbmMask) DeleteObject(iconInfo.hbmMask);
            if (iconInfo.hbmColor) DeleteObject(iconInfo.hbmColor);
            if (localX >= 0 && localX < param->w  && localY >= 0 && localY < param->h) {
                DrawIconEx(hMemDC, localX, localY, cursorInfo.hCursor, 0, 0, 0, nullptr, DI_NORMAL | DI_DEFAULTSIZE);
            }
            DestroyIcon(cursorInfo.hCursor);
        }
    }

    inline void createGif(GifParam* param) {
        FILE* file{ nullptr };
        if (_wfopen_s(&file, param->path.data(), L"wb") != 0 || !file) return;
        CGIFrgb_Config config = { 0 };
        config.pWriteFn = [](void* ctx, const uint8_t* data, const size_t size) -> int {
            return fwrite(data, 1, size, static_cast<FILE*>(ctx)) == size ? 0 : -1;
        };
        config.pContext = file;
        config.width = param->w;
        config.height = param->h;
        config.attrFlags = 0;
        config.genFlags = CGIF_FRAME_GEN_USE_DIFF_WINDOW | CGIF_FRAME_GEN_USE_TRANSPARENCY;
        CGIFrgb* pGIF = cgif_rgb_newgif(&config);
        if (!pGIF) {
            fclose(file);
            return;
        }
        CGIFrgb_FrameConfig fconfig = { 0 };
        fconfig.fmtChan = CGIF_CHAN_FMT_RGB;
        fconfig.delay = static_cast<uint16_t>(std::max(2, static_cast<int>(100.0 / param->fps + 0.5)));
        fconfig.attrFlags = CGIF_RGB_FRAME_ATTR_NO_DITHERING;
        const int actualDelayMs = fconfig.delay * 10;
        uint32_t srcRowBytes = param->w * 4;
        std::vector<unsigned char> bgra_buffer(srcRowBytes * param->h);
        uint32_t dstRowBytes = param->w * 3;
        std::vector<unsigned char> rgb_buffer(dstRowBytes * param->h);
        HDC hScreenDC = GetDC(nullptr);
        HDC hMemDC = CreateCompatibleDC(hScreenDC);
        HBITMAP hBitmap = CreateCompatibleBitmap(hScreenDC, param->w, param->h);
        HGDIOBJ hOldBitmap = SelectObject(hMemDC, hBitmap);
        BITMAPINFO bmi = { sizeof(BITMAPINFOHEADER), param->w, 0 - param->h, 1, 32, BI_RGB, 0, 0, 0, 0, 0 };
        while (!param->isFinish) {
            if (param->isPaused) {
                Sleep(50);
                continue;
            }
            auto tickStart = GetTickCount64();
            BitBlt(hMemDC, 0, 0, param->w, param->h, hScreenDC, param->x, param->y, SRCCOPY);
            drawCursor(hMemDC, param);
            GetDIBits(hMemDC, hBitmap, 0, param->h, (void*)bgra_buffer.data(), &bmi, DIB_RGB_COLORS);
            for (int row = 0; row < param->h; row++) {
                const unsigned char* srcRow = bgra_buffer.data() + row * srcRowBytes;
                unsigned char* dstRow = rgb_buffer.data() + row * dstRowBytes;
                for (int col = 0; col < param->w; col++) {
                    dstRow[col * 3 + 0] = srcRow[col * 4 + 2];
                    dstRow[col * 3 + 1] = srcRow[col * 4 + 1];
                    dstRow[col * 3 + 2] = srcRow[col * 4 + 0];
                }
            }
            fconfig.pImageData = rgb_buffer.data();
            cgif_rgb_addframe(pGIF, &fconfig);
            auto elapsed = GetTickCount64() - tickStart;
            int sleepTime = actualDelayMs - static_cast<int>(elapsed);
            if (sleepTime > 0) {
                Sleep(sleepTime);
            }
        }
        cgif_rgb_close(pGIF);
        fclose(file);
        SelectObject(hMemDC, hOldBitmap);
        DeleteObject(hBitmap);
        DeleteDC(hMemDC);
        ReleaseDC(nullptr, hScreenDC);
    }
}
