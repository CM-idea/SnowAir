#pragma once

#include <cstdio>
#include <cstring>
#include <atomic>
#include "pch.h"
#include "Util.h"
#include "Image/WebpCodec.h"
#include <wincodec.h>
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
        // 录制中选区被整体拖动：相对开录时的平移量，抓帧时叠到 x/y 上（见 grabFrame / drawCursor）
        std::atomic<int> followX{ 0 };
        std::atomic<int> followY{ 0 };
        // 输出尺寸（「动图分辨率」上限）：0 = 按选区原生尺寸直出
        int outW{ 0 };
        int outH{ 0 };
        UINT fps{16};
        // 「动图格式」：0 = GIF（cgif），1 = WebP（动画封装的静态帧），2 = APNG
        int format{ 0 };
    };

    inline void drawCursor(HDC hMemDC, GifParam* param) {
        CURSORINFO cursorInfo = { sizeof(CURSORINFO) };
        GetCursorInfo(&cursorInfo);
        if (cursorInfo.flags == CURSOR_SHOWING) {
            ICONINFO iconInfo;
            GetIconInfo(cursorInfo.hCursor, &iconInfo);
            int localX = cursorInfo.ptScreenPos.x - (param->x + param->followX.load()) - iconInfo.xHotspot;
            int localY = cursorInfo.ptScreenPos.y - (param->y + param->followY.load()) - iconInfo.yHotspot;
            if (iconInfo.hbmMask) DeleteObject(iconInfo.hbmMask);
            if (iconInfo.hbmColor) DeleteObject(iconInfo.hbmColor);
            if (localX >= 0 && localX < param->w  && localY >= 0 && localY < param->h) {
                DrawIconEx(hMemDC, localX, localY, cursorInfo.hCursor, 0, 0, 0, nullptr, DI_NORMAL | DI_DEFAULTSIZE);
            }
            DestroyIcon(cursorInfo.hCursor);
        }
    }

    // BGRA(sw×sh) → RGB(dw×dh)。尺寸相同则逐像素转换，否则面积平均缩小。
    inline void bgraToRgbScaled(const unsigned char* bgra, int sw, int sh,
        unsigned char* rgb, int dw, int dh)
    {
        if (dw == sw && dh == sh) {
            for (int row = 0; row < dh; ++row) {
                const unsigned char* s = bgra + (size_t)row * sw * 4;
                unsigned char* d = rgb + (size_t)row * dw * 3;
                for (int col = 0; col < dw; ++col) {
                    d[col * 3 + 0] = s[col * 4 + 2];
                    d[col * 3 + 1] = s[col * 4 + 1];
                    d[col * 3 + 2] = s[col * 4 + 0];
                }
            }
            return;
        }
        for (int j = 0; j < dh; ++j) {
            int sy0 = (int)((long long)j * sh / dh);
            int sy1 = (int)(((long long)(j + 1) * sh + dh - 1) / dh);
            if (sy1 <= sy0) sy1 = sy0 + 1;
            if (sy1 > sh) sy1 = sh;
            unsigned char* d = rgb + (size_t)j * dw * 3;
            for (int i = 0; i < dw; ++i) {
                int sx0 = (int)((long long)i * sw / dw);
                int sx1 = (int)(((long long)(i + 1) * sw + dw - 1) / dw);
                if (sx1 <= sx0) sx1 = sx0 + 1;
                if (sx1 > sw) sx1 = sw;
                unsigned int b = 0, g = 0, r = 0, n = 0;
                for (int y = sy0; y < sy1; ++y) {
                    const unsigned char* s = bgra + ((size_t)y * sw + sx0) * 4;
                    for (int x = sx0; x < sx1; ++x) {
                        b += s[0]; g += s[1]; r += s[2];
                        s += 4; ++n;
                    }
                }
                d[i * 3 + 0] = (unsigned char)(r / n);
                d[i * 3 + 1] = (unsigned char)(g / n);
                d[i * 3 + 2] = (unsigned char)(b / n);
            }
        }
    }

    // 抓一帧（区域 param->x/y/w/h）并缩放到 rgb_buffer（ow×oh）
    inline void grabFrame(HDC hScreenDC, HDC hMemDC, HBITMAP hBitmap, BITMAPINFO& bmi, GifParam* param,
        std::vector<unsigned char>& bgra_buffer, std::vector<unsigned char>& rgb_buffer, int ow, int oh)
    {
        // 录制中拖动选区：抓取区域整体平移（同上，尺寸不变）
        BitBlt(hMemDC, 0, 0, param->w, param->h, hScreenDC,
            param->x + param->followX.load(), param->y + param->followY.load(), SRCCOPY);
        drawCursor(hMemDC, param);
        GetDIBits(hMemDC, hBitmap, 0, param->h, (void*)bgra_buffer.data(), &bmi, DIB_RGB_COLORS);
        bgraToRgbScaled(bgra_buffer.data(), param->w, param->h, rgb_buffer.data(), ow, oh);
    }

    // ============================ APNG ============================
    // APNG 是 PNG 的超集：签名 + IHDR + acTL + （fcTL + IDAT）首帧 + 后续（fcTL + fdAT）+ IEND。
    // 这里每帧都是全尺寸、不透明，dispose=OP_NONE、blend=OP_SOURCE，不涉及帧间叠加/处置。
    inline unsigned int apngCrc32(const unsigned char* buf, size_t len)
    {
        static unsigned int table[256];
        static bool inited = false;
        if (!inited) {
            for (unsigned int i = 0; i < 256; ++i) {
                unsigned int c = i;
                for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                table[i] = c;
            }
            inited = true;
        }
        unsigned int c = 0xFFFFFFFFu;
        for (size_t i = 0; i < len; ++i) c = table[(c ^ buf[i]) & 0xFF] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    }

    inline void putBe32(unsigned char* p, unsigned int v)
    {
        p[0] = (unsigned char)((v >> 24) & 0xFF);
        p[1] = (unsigned char)((v >> 16) & 0xFF);
        p[2] = (unsigned char)((v >> 8) & 0xFF);
        p[3] = (unsigned char)(v & 0xFF);
    }

    inline unsigned int readBe32(const unsigned char* p)
    {
        return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) | ((unsigned int)p[2] << 8) | (unsigned int)p[3];
    }

    inline void writePngChunk(FILE* f, const char type[4], const unsigned char* data, unsigned int len)
    {
        unsigned char lb[4];
        putBe32(lb, len);
        fwrite(lb, 1, 4, f);
        fwrite(type, 1, 4, f);
        if (len && data) fwrite(data, 1, len, f);
        std::vector<unsigned char> tb;
        tb.reserve(4 + len);
        tb.insert(tb.end(), type, type + 4);
        if (len && data) tb.insert(tb.end(), data, data + len);
        unsigned char cb[4];
        putBe32(cb, apngCrc32(tb.data(), tb.size()));
        fwrite(cb, 1, 4, f);
    }

    // 用 WIC 把一帧 RGB(24bppBGR, top-down) 编成 PNG，取出 IHDR（仅首帧需要）与拼接后的 IDAT 负载
    inline bool encodePngFrame(const unsigned char* rgb, int w, int h,
        std::vector<unsigned char>& ihdrOut, std::vector<unsigned char>& idatOut, IWICImagingFactory* factory)
    {
        if (!factory) return false;
        IStream* stream = nullptr;
        if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) || !stream) return false;
        bool ok = false;
        IWICBitmapEncoder* enc = nullptr;
        IWICBitmapFrameEncode* frame = nullptr;
        do {
            if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) || !enc) break;
            if (FAILED(enc->Initialize(stream, WICBitmapEncoderNoCache))) break;
            if (FAILED(enc->CreateNewFrame(&frame, nullptr)) || !frame) break;
            if (FAILED(frame->Initialize(nullptr))) break;
            if (FAILED(frame->SetSize((UINT)w, (UINT)h))) break;
            WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
            if (FAILED(frame->SetPixelFormat(&fmt)) || !IsEqualGUID(fmt, GUID_WICPixelFormat24bppBGR)) break;
            const UINT stride = (UINT)w * 3;
            if (FAILED(frame->WritePixels((UINT)h, stride, stride * (UINT)h, (BYTE*)rgb))) break;
            if (FAILED(frame->Commit())) break;
            if (FAILED(enc->Commit())) break;
            ok = true;
        } while (false);

        if (ok) {
            STATSTG stat{};
            HGLOBAL hg = nullptr;
            if (SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME))
                && SUCCEEDED(GetHGlobalFromStream(stream, &hg)) && hg) {
                auto p = (const unsigned char*)GlobalLock(hg);
                const size_t n = (size_t)stat.cbSize.QuadPart;
                if (p && n > 8) {
                    size_t pos = 8; // 跳过 PNG 签名
                    while (pos + 8 <= n) {
                        const unsigned int len = readBe32(p + pos);
                        const char* type = (const char*)(p + pos + 4);
                        const size_t dataPos = pos + 8;
                        if (dataPos + len + 4 > n) break;
                        if (memcmp(type, "IHDR", 4) == 0 && ihdrOut.empty())
                            ihdrOut.assign(p + dataPos, p + dataPos + len);
                        else if (memcmp(type, "IDAT", 4) == 0)
                            idatOut.insert(idatOut.end(), p + dataPos, p + dataPos + len);
                        else if (memcmp(type, "IEND", 4) == 0)
                            break;
                        pos = dataPos + len + 4;
                    }
                }
                if (p) GlobalUnlock(hg);
            }
        }
        if (frame) frame->Release();
        if (enc) enc->Release();
        stream->Release();
        return ok && !idatOut.empty() && !ihdrOut.empty();
    }

    inline bool assembleApng(const std::wstring& path, int w, int h, int fps,
        const std::vector<unsigned char>& ihdr, const std::vector<std::vector<unsigned char>>& frames)
    {
        if (ihdr.empty() || frames.empty()) return false;
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.data(), L"wb") != 0 || !f) return false;
        const unsigned char sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
        fwrite(sig, 1, 8, f);
        writePngChunk(f, "IHDR", ihdr.data(), (unsigned int)ihdr.size());
        unsigned char actl[8];
        putBe32(actl, (unsigned int)frames.size());
        putBe32(actl + 4, 0); // num_plays = 0：无限循环
        writePngChunk(f, "acTL", actl, 8);

        const unsigned int den = (fps > 0 ? (unsigned int)fps : 10u);
        unsigned int seq = 0;
        for (size_t i = 0; i < frames.size(); ++i) {
            unsigned char fctl[26];
            putBe32(fctl, seq++);
            putBe32(fctl + 4, (unsigned int)w);
            putBe32(fctl + 8, (unsigned int)h);
            putBe32(fctl + 12, 0);
            putBe32(fctl + 16, 0);
            fctl[20] = 0; fctl[21] = 1;                                              // delay_num = 1
            fctl[22] = (unsigned char)((den >> 8) & 0xFF); fctl[23] = (unsigned char)(den & 0xFF); // delay_den = fps
            fctl[24] = 0;                                                            // dispose_op = NONE
            fctl[25] = 0;                                                            // blend_op = SOURCE
            writePngChunk(f, "fcTL", fctl, 26);
            if (i == 0) {
                writePngChunk(f, "IDAT", frames[0].data(), (unsigned int)frames[0].size());
            }
            else {
                std::vector<unsigned char> fdat(4 + frames[i].size());
                putBe32(fdat.data(), seq++);
                memcpy(fdat.data() + 4, frames[i].data(), frames[i].size());
                writePngChunk(f, "fdAT", fdat.data(), (unsigned int)fdat.size());
            }
        }
        writePngChunk(f, "IEND", nullptr, 0);
        fclose(f);
        return true;
    }

    inline void createApng(GifParam* param)
    {
        const int ow = param->outW > 0 ? param->outW : param->w;
        const int oh = param->outH > 0 ? param->outH : param->h;
        // WIC / 流对象要求本线程已初始化 COM
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        IWICImagingFactory* factory = nullptr;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            __uuidof(IWICImagingFactory), (void**)&factory);
        if (!factory) { CoUninitialize(); return; }

        std::vector<unsigned char> bgra_buffer((size_t)param->w * 4 * param->h);
        std::vector<unsigned char> rgb_buffer((size_t)ow * oh * 3);
        std::vector<unsigned char> ihdr;
        std::vector<std::vector<unsigned char>> frames;

        HDC hScreenDC = GetDC(nullptr);
        HDC hMemDC = CreateCompatibleDC(hScreenDC);
        HBITMAP hBitmap = CreateCompatibleBitmap(hScreenDC, param->w, param->h);
        HGDIOBJ hOldBitmap = SelectObject(hMemDC, hBitmap);
        BITMAPINFO bmi = { sizeof(BITMAPINFOHEADER), param->w, 0 - param->h, 1, 32, BI_RGB, 0, 0, 0, 0, 0 };
        const int delayMs = std::max(2, (int)(1000.0 / param->fps + 0.5));
        while (!param->isFinish) {
            if (param->isPaused) { Sleep(50); continue; }
            auto tickStart = GetTickCount64();
            grabFrame(hScreenDC, hMemDC, hBitmap, bmi, param, bgra_buffer, rgb_buffer, ow, oh);
            std::vector<unsigned char> idat;
            if (!encodePngFrame(rgb_buffer.data(), ow, oh, ihdr, idat, factory)) break;
            frames.push_back(std::move(idat));
            int sleepTime = delayMs - (int)(GetTickCount64() - tickStart);
            if (sleepTime > 0) Sleep(sleepTime);
        }
        // 抓完再统一封装：只有全部帧都拿到才写文件
        if (!frames.empty())
            assembleApng(param->path, ow, oh, (int)param->fps, ihdr, frames);

        SelectObject(hMemDC, hOldBitmap);
        DeleteObject(hBitmap);
        DeleteDC(hMemDC);
        ReleaseDC(nullptr, hScreenDC);
        factory->Release();
        CoUninitialize();
    }
    // =============================================================

    // ============================ WebP（动画） ============================
    // 内置 libwebp 逐帧编静态 WebP，抠出其中的图像块（VP8 / VP8L，含可选的 ALPH），
    // 再按动画 WebP 容器规范拼装：
    // RIFF/WEBP + VP8X(动画标志) + ANIM + 每帧 ANMF(帧头 + 图像块) + pad。
    inline void putLe32(unsigned char* p, unsigned int v)
    {
        p[0] = (unsigned char)(v & 0xFF);
        p[1] = (unsigned char)((v >> 8) & 0xFF);
        p[2] = (unsigned char)((v >> 16) & 0xFF);
        p[3] = (unsigned char)((v >> 24) & 0xFF);
    }

    inline unsigned int readLe32(const unsigned char* p)
    {
        return (unsigned int)p[0] | ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
    }

    inline void putLe24(unsigned char* p, unsigned int v)
    {
        p[0] = (unsigned char)(v & 0xFF);
        p[1] = (unsigned char)((v >> 8) & 0xFF);
        p[2] = (unsigned char)((v >> 16) & 0xFF);
    }

    // 追加一个 RIFF 块：FourCC + 长度(LE) + 负载 + 偶数字节对齐
    inline void appendRiffChunk(std::vector<unsigned char>& v, const char* fourcc,
        const unsigned char* data, unsigned int len)
    {
        v.insert(v.end(), fourcc, fourcc + 4);
        unsigned char sz[4];
        putLe32(sz, len);
        v.insert(v.end(), sz, sz + 4);
        if (len && data) v.insert(v.end(), data, data + len);
        if (len & 1) v.push_back(0);
    }

    // 用内置 libwebp 把一帧 RGB 编成静态 WebP，抠出 ALPH(可选) + VP8/VP8L 图像块（按原样，含各自的头与对齐）
    inline bool encodeWebpFrame(const unsigned char* rgb, int w, int h,
        std::vector<unsigned char>& imgChunksOut)
    {
        std::vector<uint8_t> file;
        // method 取 3：动图逐帧编码，优先保证录制帧率
        if (!WebpCodec::EncodeRGB(rgb, w, h, w * 3, 90.f, 3, file)) return false;
        const unsigned char* p = file.data();
        const size_t n = file.size();
        if (!p || n <= 12 || memcmp(p, "RIFF", 4) != 0 || memcmp(p + 8, "WEBP", 4) != 0) return false;
        size_t pos = 12;
        while (pos + 8 <= n) {
            const unsigned int len = readLe32(p + pos + 4);
            const size_t dataPos = pos + 8;
            const size_t chunkLen = 8 + len + (len & 1);
            if (dataPos + len > n) break;
            const char* fourcc = (const char*)(p + pos);
            if (memcmp(fourcc, "ALPH", 4) == 0 || memcmp(fourcc, "VP8 ", 4) == 0
                || memcmp(fourcc, "VP8L", 4) == 0) {
                const size_t copy = (std::min)(chunkLen, n - pos);
                imgChunksOut.insert(imgChunksOut.end(), p + pos, p + pos + copy);
                if (memcmp(fourcc, "VP8L", 4) == 0 || memcmp(fourcc, "VP8 ", 4) == 0) break; // 图像块之后不用再看
            }
            pos += chunkLen;
        }
        return !imgChunksOut.empty();
    }

    inline bool assembleWebpAnim(const std::wstring& path, int w, int h, int fps,
        const std::vector<std::vector<unsigned char>>& frames)
    {
        if (frames.empty()) return false;
        std::vector<unsigned char> body;
        {
            unsigned char d[10]{};
            d[0] = 0x02;                 // 特性位：动画
            putLe24(d + 4, (unsigned int)(w - 1));
            putLe24(d + 7, (unsigned int)(h - 1));
            appendRiffChunk(body, "VP8X", d, 10);
        }
        {
            unsigned char d[6]{};
            putLe32(d, 0);               // 背景色 BGRA
            d[4] = 0; d[5] = 0;          // 循环次数 0 = 无限
            appendRiffChunk(body, "ANIM", d, 6);
        }
        const unsigned int durMs = (fps > 0) ? (unsigned int)(1000.0 / fps + 0.5) : 100u;
        for (size_t i = 0; i < frames.size(); ++i) {
            std::vector<unsigned char> anmf;
            unsigned char hdr[16]{};
            putLe24(hdr + 0, 0);                     // 帧左上角 x / 2
            putLe24(hdr + 3, 0);                     // 帧左上角 y / 2
            putLe24(hdr + 6, (unsigned int)(w - 1)); // 帧宽 - 1
            putLe24(hdr + 9, (unsigned int)(h - 1)); // 帧高 - 1
            putLe24(hdr + 12, durMs);                // 时长（毫秒）
            hdr[15] = 0x01;                          // 位0=不混合（整帧覆盖），位1=dispose none
            anmf.insert(anmf.end(), hdr, hdr + 16);
            anmf.insert(anmf.end(), frames[i].begin(), frames[i].end());
            appendRiffChunk(body, "ANMF", anmf.data(), (unsigned int)anmf.size());
        }

        FILE* f = nullptr;
        if (_wfopen_s(&f, path.data(), L"wb") != 0 || !f) return false;
        fwrite("RIFF", 1, 4, f);
        unsigned char sz[4];
        putLe32(sz, (unsigned int)(4 + body.size()));
        fwrite(sz, 1, 4, f);
        fwrite("WEBP", 1, 4, f);
        if (!body.empty()) fwrite(body.data(), 1, body.size(), f);
        fclose(f);
        return true;
    }

    inline void createWebp(GifParam* param)
    {
        const int ow = param->outW > 0 ? param->outW : param->w;
        const int oh = param->outH > 0 ? param->outH : param->h;

        std::vector<unsigned char> bgra_buffer((size_t)param->w * 4 * param->h);
        std::vector<unsigned char> rgb_buffer((size_t)ow * oh * 3);
        std::vector<std::vector<unsigned char>> frames;
        HDC hScreenDC = GetDC(nullptr);
        HDC hMemDC = CreateCompatibleDC(hScreenDC);
        HBITMAP hBitmap = CreateCompatibleBitmap(hScreenDC, param->w, param->h);
        HGDIOBJ hOldBitmap = SelectObject(hMemDC, hBitmap);
        BITMAPINFO bmi = { sizeof(BITMAPINFOHEADER), param->w, 0 - param->h, 1, 32, BI_RGB, 0, 0, 0, 0, 0 };
        const int delayMs = std::max(2, (int)(1000.0 / param->fps + 0.5));
        while (!param->isFinish) {
            if (param->isPaused) { Sleep(50); continue; }
            auto tickStart = GetTickCount64();
            grabFrame(hScreenDC, hMemDC, hBitmap, bmi, param, bgra_buffer, rgb_buffer, ow, oh);
            std::vector<unsigned char> chunks;
            if (!encodeWebpFrame(rgb_buffer.data(), ow, oh, chunks)) break;
            frames.push_back(std::move(chunks));
            int sleepTime = delayMs - (int)(GetTickCount64() - tickStart);
            if (sleepTime > 0) Sleep(sleepTime);
        }
        if (!frames.empty()) assembleWebpAnim(param->path, ow, oh, (int)param->fps, frames);

        SelectObject(hMemDC, hOldBitmap);
        DeleteObject(hBitmap);
        DeleteDC(hMemDC);
        ReleaseDC(nullptr, hScreenDC);
    }
    // =====================================================================

    inline void createGifInternal(GifParam* param)
    {
        const int ow = param->outW > 0 ? param->outW : param->w;
        const int oh = param->outH > 0 ? param->outH : param->h;
        FILE* file{ nullptr };
        if (_wfopen_s(&file, param->path.data(), L"wb") != 0 || !file) return;
        CGIFrgb_Config config = { 0 };
        config.pWriteFn = [](void* ctx, const uint8_t* data, const size_t size) -> int {
            return fwrite(data, 1, size, static_cast<FILE*>(ctx)) == size ? 0 : -1;
        };
        config.pContext = file;
        config.width = ow;
        config.height = oh;
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
        std::vector<unsigned char> bgra_buffer((size_t)param->w * 4 * param->h);
        std::vector<unsigned char> rgb_buffer((size_t)ow * oh * 3);
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
            grabFrame(hScreenDC, hMemDC, hBitmap, bmi, param, bgra_buffer, rgb_buffer, ow, oh);
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

    // 入口：按「动图格式」分派
    inline void createGif(GifParam* param) {
        if (param->format == 1) createWebp(param);
        else if (param->format == 2) createApng(param);
        else createGifInternal(param);
    }
}
