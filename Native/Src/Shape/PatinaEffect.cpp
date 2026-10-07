#include "pch.h"
#include <wincodec.h>
#include <cmath>
#include <random>
#include "PatinaEffect.h"

using Microsoft::WRL::ComPtr;

namespace PatinaEffect {
namespace {

int clampByte(int x) { return x < 0 ? 0 : (x > 255 ? 255 : x); }
int clampUv(int x) { return x < -128 ? -128 : (x > 127 ? 127 : x); }

const wchar_t* kNames[] = {
	L"卜卜口", L"拆家大主教", L"能不能好好说话", L"神奇海螺_0000", L"电脑玩家海螺",
	L"电子包浆", L"阿卡梦", L"极限天空", L"夹去阳间", L"大吉山放送部",
	L"绫波", L"樱岛麻衣俺老婆0000", L"fps爱好者", L"蒙古上单", L"黄前久美子",
	L"凉宫春日", L"折木奉太郎", L"小鸟游六花", L"北白川玉子", L"薇尔莉特",
};
const wchar_t kPunct[] = L"-_+~!^&、.。”“\"'|";

struct SeedRand {
	uint32_t h{ 2166136261u };
	explicit SeedRand(const std::wstring& seed)
	{
		for (wchar_t c : seed) {
			h ^= (uint32_t)c;
			h *= 16777619u;
		}
	}
	double next()
	{
		h ^= h << 13;
		h ^= h >> 17;
		h ^= h << 5;
		return double(h % 100000u) / 100000.0;
	}
	int range(int a, int b)
	{
		return (int)std::round(next() * (b - a) + a);
	}
};

void greenify(BYTE* bits, UINT32 pitch, UINT32 w, UINT32 h, float amount = 0.58f)
{
	amount = std::clamp(amount, 0.f, 1.f);
	for (UINT32 y = 0; y < h; ++y) {
		auto row = bits + y * pitch;
		for (UINT32 x = 0; x < w; ++x) {
			auto p = row + x * 4;
			const int b = p[0], g = p[1], r = p[2], a = p[3];
			const int yy = clampByte((77 * r + 150 * g + 29 * b) >> 8);
			const int u = clampUv(((-43 * r - 85 * g + 128 * b) >> 8) - 1);
			const int v = clampUv(((128 * r - 107 * g - 21 * b) >> 8) - 1);
			const int nr = clampByte((65536 * yy + 91881 * v) >> 16);
			const int ng = clampByte((65536 * yy - 22553 * u - 46802 * v) >> 16);
			const int nb = clampByte((65536 * yy + 116130 * u) >> 16);
			p[0] = (BYTE)clampByte(b + (int)((nb - b) * amount));
			p[1] = (BYTE)clampByte(g + (int)((ng - g) * amount));
			p[2] = (BYTE)clampByte(r + (int)((nr - r) * amount));
			p[3] = (BYTE)a;
		}
	}
}

bool jpegRoundtrip(std::vector<BYTE>& bgra, UINT32 w, UINT32 h, UINT32 pitch, int quality)
{
	ComPtr<IWICImagingFactory> factory;
	if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(factory.GetAddressOf()))))
		return false;

	// 编码用 24bpp BGR（无 alpha），对齐 QT RGB32→JPEG
	std::vector<BYTE> bgr((size_t)w * h * 3);
	for (UINT32 y = 0; y < h; ++y) {
		auto src = bgra.data() + y * pitch;
		auto dst = bgr.data() + (size_t)y * w * 3;
		for (UINT32 x = 0; x < w; ++x) {
			dst[x * 3 + 0] = src[x * 4 + 0];
			dst[x * 3 + 1] = src[x * 4 + 1];
			dst[x * 3 + 2] = src[x * 4 + 2];
		}
	}

	ComPtr<IStream> stream;
	if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, stream.GetAddressOf()))) return false;
	ComPtr<IWICBitmapEncoder> encoder;
	if (FAILED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, encoder.GetAddressOf())))
		return false;
	if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;
	ComPtr<IWICBitmapFrameEncode> frame;
	ComPtr<IPropertyBag2> props;
	if (FAILED(encoder->CreateNewFrame(frame.GetAddressOf(), props.GetAddressOf()))) return false;
	if (props) {
		PROPBAG2 opt{};
		opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
		VARIANT v; VariantInit(&v);
		v.vt = VT_R4;
		v.fltVal = std::clamp(quality, 5, 100) / 100.f;
		props->Write(1, &opt, &v);
		VariantClear(&v);
	}
	if (FAILED(frame->Initialize(props.Get()))) return false;
	if (FAILED(frame->SetSize(w, h))) return false;
	WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
	if (FAILED(frame->SetPixelFormat(&fmt))) return false;
	if (FAILED(frame->WritePixels(h, w * 3, (UINT)bgr.size(), bgr.data()))) return false;
	if (FAILED(frame->Commit())) return false;
	if (FAILED(encoder->Commit())) return false;

	STATSTG st{};
	if (FAILED(stream->Stat(&st, STATFLAG_NONAME))) return false;
	const ULONG sz = (ULONG)st.cbSize.QuadPart;
	std::vector<BYTE> jpeg(sz);
	LARGE_INTEGER zero{};
	stream->Seek(zero, STREAM_SEEK_SET, nullptr);
	ULONG read = 0;
	if (FAILED(stream->Read(jpeg.data(), sz, &read)) || read != sz) return false;

	ComPtr<IWICStream> inStream;
	factory->CreateStream(inStream.GetAddressOf());
	if (FAILED(inStream->InitializeFromMemory(jpeg.data(), sz))) return false;
	ComPtr<IWICBitmapDecoder> decoder;
	if (FAILED(factory->CreateDecoderFromStream(inStream.Get(), nullptr, WICDecodeMetadataCacheOnLoad,
		decoder.GetAddressOf())))
		return false;
	ComPtr<IWICBitmapFrameDecode> decoded;
	if (FAILED(decoder->GetFrame(0, decoded.GetAddressOf()))) return false;
	ComPtr<IWICFormatConverter> conv;
	if (FAILED(factory->CreateFormatConverter(conv.GetAddressOf()))) return false;
	if (FAILED(conv->Initialize(decoded.Get(), GUID_WICPixelFormat32bppBGRA,
		WICBitmapDitherTypeNone, nullptr, 0.f, WICBitmapPaletteTypeCustom)))
		return false;
	UINT dw = 0, dh = 0;
	conv->GetSize(&dw, &dh);
	if (dw != w || dh != h) {
		// 尺寸偶有偏差时仍读回再双线性由调用方缩放；此处要求一致
		return false;
	}
	if ((UINT)bgra.size() < pitch * h) bgra.resize((size_t)pitch * h);
	if (FAILED(conv->CopyPixels(nullptr, pitch, pitch * h, bgra.data()))) return false;
	return true;
}

std::wstring pickName(SeedRand& rand)
{
	const int ni = (int)(rand.next() * (double)(sizeof(kNames) / sizeof(kNames[0])));
	std::wstring name = kNames[std::clamp(ni, 0, (int)(sizeof(kNames) / sizeof(kNames[0])) - 1)];
	const int pi = std::clamp((int)(rand.next() * 15), 0, (int)wcslen(kPunct) - 1);
	const wchar_t k = kPunct[pi];
	wchar_t digits[8];
	swprintf_s(digits, L"%04d", (int)(rand.next() * 9999));
	for (size_t i = 0; i + 3 < name.size(); ++i) {
		if (iswdigit(name[i]) && iswdigit(name[i + 1]) && iswdigit(name[i + 2]) && iswdigit(name[i + 3])) {
			name.replace(i, 4, digits);
			break;
		}
	}
	for (auto& c : name) {
		if (c == L'_') c = k;
	}
	return L"@" + name;
}

void drawStamp(BYTE* bits, UINT32 pitch, UINT32 w, UINT32 h, int basePlan, int wmSize, SeedRand& rand,
	IDWriteFactory* dw, ID2D1Factory* /*d2d*/)
{
	// CPU 侧用 GDI 画戳，避免再开 D2D target；字号对齐 QT
	int fontSize = 22 + rand.range(0, 7);
	if (fontSize < 1) fontSize = 1;
	float px = (float)w / (float)fontSize;
	px *= std::max(0.4f, wmSize / 24.f);
	const float shift = px / 2.f;
	const int plan = std::clamp(rand.range(0, basePlan), 0, 2);
	float left = w / 2.f, top = h / 2.f;
	UINT align = TA_CENTER;
	if (plan == 0) {
		align = TA_RIGHT;
		left = w - shift * 1.2f + rand.range(-5, 5);
		top = h - shift + rand.range(-5, 5);
	}
	else if (plan == 1) {
		left = w / 2.f + rand.range(-10, 10);
		top = h - shift * 1.2f + rand.range(-5, 5);
	}
	else {
		left = w / 2.f + rand.range(-10, 10);
		top = h / 2.f + shift + rand.range(-10, 10);
	}
	const auto text = pickName(rand);
	const int fpx = std::max(8, (int)std::round(px));

	BITMAPINFO bi{};
	bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bi.bmiHeader.biWidth = (LONG)w;
	bi.bmiHeader.biHeight = -(LONG)h;
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	void* bitsDib = nullptr;
	HDC screen = GetDC(nullptr);
	HDC mem = CreateCompatibleDC(screen);
	HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bitsDib, nullptr, 0);
	ReleaseDC(nullptr, screen);
	if (!dib || !bitsDib) {
		if (mem) DeleteDC(mem);
		return;
	}
	HGDIOBJ oldBmp = SelectObject(mem, dib);
	// 拷贝当前像素
	for (UINT32 y = 0; y < h; ++y)
		memcpy((BYTE*)bitsDib + y * w * 4, bits + y * pitch, w * 4);

	HFONT font = CreateFontW(-fpx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
		DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
	HGDIOBJ oldFont = SelectObject(mem, font);
	SetBkMode(mem, TRANSPARENT);
	SetTextAlign(mem, align | TA_BOTTOM);
	SetTextColor(mem, RGB(0, 0, 0));
	TextOutW(mem, (int)left, (int)top + 1, text.c_str(), (int)text.size());
	SetTextColor(mem, RGB(255, 255, 255));
	TextOutW(mem, (int)left, (int)top, text.c_str(), (int)text.size());
	SelectObject(mem, oldFont);
	DeleteObject(font);
	SelectObject(mem, oldBmp);
	DeleteDC(mem);

	for (UINT32 y = 0; y < h; ++y)
		memcpy(bits + y * pitch, (BYTE*)bitsDib + y * w * 4, w * 4);
	DeleteObject(dib);
	(void)dw;
}

void scaleBilinear(const BYTE* src, UINT32 sw, UINT32 sh, UINT32 sp,
	BYTE* dst, UINT32 dw, UINT32 dh, UINT32 dp)
{
	for (UINT32 y = 0; y < dh; ++y) {
		float sy = (y + 0.5f) * sh / dh - 0.5f;
		int y0 = (int)std::floor(sy);
		float fy = sy - y0;
		int y1 = y0 + 1;
		if (y0 < 0) y0 = 0;
		if (y1 >= (int)sh) y1 = (int)sh - 1;
		for (UINT32 x = 0; x < dw; ++x) {
			float sx = (x + 0.5f) * sw / dw - 0.5f;
			int x0 = (int)std::floor(sx);
			float fx = sx - x0;
			int x1 = x0 + 1;
			if (x0 < 0) x0 = 0;
			if (x1 >= (int)sw) x1 = (int)sw - 1;
			auto sample = [&](int xx, int yy) {
				return src + yy * sp + xx * 4;
			};
			auto a = sample(x0, y0), b = sample(x1, y0), c = sample(x0, y1), d = sample(x1, y1);
			auto o = dst + y * dp + x * 4;
			for (int k = 0; k < 4; ++k) {
				float v = a[k] * (1 - fx) * (1 - fy) + b[k] * fx * (1 - fy)
					+ c[k] * (1 - fx) * fy + d[k] * fx * fy;
				o[k] = (BYTE)std::clamp((int)std::round(v), 0, 255);
			}
		}
	}
}

} // namespace

bool apply(std::vector<BYTE>& bgra, UINT32 width, UINT32 height, UINT32 pitch, const Style& s)
{
	if (width < 2 || height < 2 || bgra.empty()) return false;
	const int strength = std::clamp(s.strength, 0, 100);
	const int rounds = std::clamp((int)std::round(strength * 0.45), 1, 40);
	constexpr int kMaxDim = 480;
	const float down = std::min(1.f, kMaxDim / (float)std::max(width, height));
	const UINT32 tw = std::max(1u, (UINT32)std::round(width * down));
	const UINT32 th = std::max(1u, (UINT32)std::round(height * down));
	const UINT32 tp = tw * 4;

	std::vector<BYTE> canvas((size_t)tp * th);
	scaleBilinear(bgra.data(), width, height, pitch, canvas.data(), tw, th, tp);

	SeedRand rand(s.seed.empty() ? L"SnowAir" : s.seed);
	auto stampAndGreen = [&](std::vector<BYTE>& img) {
		if (s.watermark) {
			drawStamp(img.data(), tp, tw, th, s.plan, s.wmSize > 0 ? s.wmSize : 20, rand,
				Ling::D2D::get()->dwriteFactory.Get(), Ling::D2D::get()->d2dFactory.Get());
		}
		if (s.green)
			greenify(img.data(), tp, tw, th);
	};

	stampAndGreen(canvas);
	for (int i = 0; i < rounds; ++i) {
		stampAndGreen(canvas);
		const int q = std::clamp((int)std::round(68 + rand.next() * 8), 5, 100);
		std::vector<BYTE> jpeg = canvas;
		if (!jpegRoundtrip(jpeg, tw, th, tp, q)) break;
		const int dx = rand.range(-2, 2);
		const int dy = rand.range(-2, 2);
		std::vector<BYTE> next((size_t)tp * th, 255);
		// 填白底
		for (UINT32 y = 0; y < th; ++y) {
			auto row = next.data() + y * tp;
			for (UINT32 x = 0; x < tw; ++x) {
				row[x * 4 + 0] = 255;
				row[x * 4 + 1] = 255;
				row[x * 4 + 2] = 255;
				row[x * 4 + 3] = 255;
			}
		}
		// 略微错位贴回（对齐 QT drawImage 拉伸）
		const float dw = (float)tw + dx;
		const float dh = (float)th + dy;
		const float ox = -dx / 2.f;
		const float oy = -dy / 2.f;
		for (UINT32 y = 0; y < th; ++y) {
			for (UINT32 x = 0; x < tw; ++x) {
				float sx = (x - ox) * tw / std::max(1.f, dw);
				float sy = (y - oy) * th / std::max(1.f, dh);
				int ix = std::clamp((int)std::round(sx), 0, (int)tw - 1);
				int iy = std::clamp((int)std::round(sy), 0, (int)th - 1);
				auto src = jpeg.data() + iy * tp + ix * 4;
				auto dst = next.data() + y * tp + x * 4;
				dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 255;
			}
		}
		canvas = std::move(next);
	}

	scaleBilinear(canvas.data(), tw, th, tp, bgra.data(), width, height, pitch);
	return true;
}

} // namespace PatinaEffect
