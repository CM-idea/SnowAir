#include "pch.h"
#include "ImageExport.h"
#include <wincodec.h>
#include <wrl.h>
#include <algorithm>

using Microsoft::WRL::ComPtr;

namespace {
    ComPtr<IWICImagingFactory> wicFactory()
    {
        static ComPtr<IWICImagingFactory> factory;
        if (!factory)
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        return factory;
    }
}

bool ImageExport::encodeJpeg(const std::vector<BYTE>& bgra, int w, int h, int quality, std::vector<BYTE>& out)
{
    out.clear();
    if (w <= 0 || h <= 0 || bgra.size() < (size_t)w * h * 4) return false;
    auto wic = wicFactory();
    if (!wic) return false;

    // 用 HGLOBAL 流而不是 InitializeFromMemory：后者的 Stat 返回缓冲区容量，
    // 会把尾部未写入的零字节一并当成 JPEG 数据。
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, stream.GetAddressOf()))) return false;

    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(wic->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder))) return false;
    if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(encoder->CreateNewFrame(&frame, &props))) return false;
    if (props) {
        PROPBAG2 option{};
        option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT value{};
        value.vt = VT_R4;
        value.fltVal = (float)std::max(1, std::min(100, quality)) / 100.f;
        props->Write(1, &option, &value);
    }
    if (FAILED(frame->Initialize(props.Get()))) return false;
    frame->SetSize(w, h);

    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    frame->SetPixelFormat(&fmt);

    ComPtr<IWICBitmap> bmp;
    if (FAILED(wic->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppBGRA,
        (UINT)((size_t)w * 4), (UINT)((size_t)w * 4 * h), const_cast<BYTE*>(bgra.data()), &bmp))) return false;

    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(&conv))) return false;
    if (FAILED(conv->Initialize(bmp.Get(), GUID_WICPixelFormat24bppBGR,
        WICBitmapDitherTypeNone, nullptr, 0.f, WICBitmapPaletteTypeCustom))) return false;

    if (FAILED(frame->WriteSource(conv.Get(), nullptr))) return false;
    if (FAILED(frame->Commit())) return false;
    if (FAILED(encoder->Commit())) return false;

    STATSTG st{};
    if (FAILED(stream->Stat(&st, STATFLAG_NONAME))) return false;
    const ULONGLONG size = st.cbSize.QuadPart;
    if (size == 0) return false;
    out.resize((size_t)size);
    LARGE_INTEGER zero{};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    ULONG read = 0;
    if (FAILED(stream->Read(out.data(), (ULONG)out.size(), &read)) || read != out.size()) {
        out.clear();
        return false;
    }
    return true;
}
