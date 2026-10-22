#include "pch.h"
#include <wincodec.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <winver.h>
#include <format>
#include <fstream>
#include "Util.h"
#include "Image/WebpCodec.h"
#include "Lang.h"
#include "Setting.h"
#include "Win/WinPin.h"
#include "quirc/quirc.h"
#include "ReadBarcode.h"

using Microsoft::WRL::ComPtr;

std::array<int, 3> Util::getAppVersion()
{
	std::array<int, 3> result{ 0, 0, 0 };
	// 从当前进程已加载的主模块里读 RT_VERSION 资源（在内存里），不按路径读磁盘上的 exe。
	HMODULE mod = GetModuleHandleW(nullptr);
	if (!mod) return result;
	// VS_VERSION_INFO 的资源 id 就是 1（winres.h 里 VS_VERSION_INFO = 1）
	HRSRC res = FindResourceW(mod, MAKEINTRESOURCEW(1), RT_VERSION);
	if (!res) return result;
	HGLOBAL glob = LoadResource(mod, res);
	if (!glob) return result;
	const void* data = LockResource(glob);
	if (!data) return result;
	VS_FIXEDFILEINFO* ffi = nullptr;
	UINT size = 0;
	if (!VerQueryValueW(data, L"\\", reinterpret_cast<void**>(&ffi), &size) || !ffi) return result;
	result[0] = static_cast<int>((ffi->dwFileVersionMS >> 16) & 0xFFFF);
	result[1] = static_cast<int>(ffi->dwFileVersionMS & 0xFFFF);
	result[2] = static_cast<int>((ffi->dwFileVersionLS >> 16) & 0xFFFF);
	return result;
}

bool Util::isElevated()
{
	HANDLE token = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
	TOKEN_ELEVATION elevation{};
	DWORD size = 0;
	BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size)
		&& elevation.TokenIsElevated != 0;
	CloseHandle(token);
	return ok != FALSE;
}

bool Util::isBuiltinAdminAccount()
{
	HANDLE token = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
	DWORD size = 0;
	GetTokenInformation(token, TokenUser, nullptr, 0, &size);
	std::vector<BYTE> buf(size);
	bool isAdmin = false;
	if (GetTokenInformation(token, TokenUser, buf.data(), size, &size)) {
		PSID sid = reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid;
		const BYTE subCount = *GetSidSubAuthorityCount(sid);
		// 内置 Administrator 的 RID 固定为 500（本地账户 SID 形如 S-1-5-21-...-500）；
		// 子权限个数 <4 的是 S-1-5-... 这类服务/组 SID，没有 500 这个用户，直接排除
		if (subCount >= 4) {
			isAdmin = (*GetSidSubAuthority(sid, subCount - 1) == 500);
		}
	}
	CloseHandle(token);
	return isAdmin;
}

bool Util::spawnElevated(const std::wstring& params)
{
	wchar_t exePath[MAX_PATH];
	GetModuleFileNameW(nullptr, exePath, MAX_PATH);
	SHELLEXECUTEINFOW sei{ sizeof(sei) };
	sei.lpVerb = L"runas";
	sei.lpFile = exePath;
	sei.lpParameters = params.c_str();
	sei.nShow = SW_SHOWNORMAL;
	return ShellExecuteExW(&sei) != FALSE;
}

namespace {
	// 把 BGRA top-down 像素按指定容器编码写进 stream。
	// PNG/BMP 原生支持 32bppBGRA；JPEG 等只接受 24bppBGR，交给 WIC 格式转换器转一道。
	// quality < 0 表示该编码器不设质量（无损 / 用默认）。
	bool encodeImage(IStream* stream, const int w, const int h, BYTE* data,
		REFGUID container, const float quality)
	{
		ComPtr<IWICImagingFactory> factory;
		auto hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf()));
		if (FAILED(hr)) return false;
		ComPtr<IWICBitmapEncoder> encoder;
		hr = factory->CreateEncoder(container, nullptr, encoder.GetAddressOf());
		if (FAILED(hr)) return false;
		hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
		if (FAILED(hr)) return false;
		ComPtr<IWICBitmapFrameEncode> frame;
		ComPtr<IPropertyBag2> props;
		hr = encoder->CreateNewFrame(frame.GetAddressOf(), props.GetAddressOf());
		if (FAILED(hr)) return false;
		if (props && quality >= 0.f) {
			// 有损编码器认 ImageQuality；不认的（PNG/BMP）会返回错误，忽略即可
			PROPBAG2 opt{};
			opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
			VARIANT val; VariantInit(&val);
			val.vt = VT_R4; val.fltVal = quality;
			props->Write(1, &opt, &val);
		}
		hr = frame->Initialize(props.Get());
		if (FAILED(hr)) return false;
		hr = frame->SetSize((UINT)w, (UINT)h);
		if (FAILED(hr)) return false;
		WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
		hr = frame->SetPixelFormat(&fmt);
		if (FAILED(hr)) return false;

		ComPtr<IWICBitmap> src;
		hr = factory->CreateBitmapFromMemory((UINT)w, (UINT)h, GUID_WICPixelFormat32bppBGRA,
			(UINT)w * 4, (UINT)w * 4 * (UINT)h, data, src.GetAddressOf());
		if (FAILED(hr)) return false;

		if (IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA)) {
			hr = frame->WriteSource(src.Get(), nullptr);
		}
		else {
			// 编码器把像素格式改成了它支持的那种（JPEG→24bppBGR 等），先转再写
			ComPtr<IWICFormatConverter> conv;
			hr = factory->CreateFormatConverter(conv.GetAddressOf());
			if (SUCCEEDED(hr))
				hr = conv->Initialize(src.Get(), fmt, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
			if (SUCCEEDED(hr)) hr = frame->WriteSource(conv.Get(), nullptr);
		}
		if (FAILED(hr)) return false;
		hr = frame->Commit();
		if (FAILED(hr)) return false;
		return SUCCEEDED(encoder->Commit());
	}

	// 保存路径的扩展名 → WIC 容器 + 有损质量（认不出来按 PNG 无损）
	void containerForExt(const std::wstring& ext, GUID& container, float& quality)
	{
		std::wstring e;
		e.reserve(ext.size());
		for (auto c : ext) e.push_back((wchar_t)towlower(c));
		// 调用方传的是 path::extension()，带前导点（".jpg" / ".bmp"），先去掉再比较，
		// 否则 jpg/bmp 永远匹配不上，会静默退化成 PNG（扩展名却是 .jpg / .bmp）。
		if (!e.empty() && e.front() == L'.') e.erase(e.begin());
		if (e == L"jpg" || e == L"jpeg") { container = GUID_ContainerFormatJpeg; quality = 0.92f; }
		else if (e == L"bmp") { container = GUID_ContainerFormatBmp; quality = -1.f; }
		else { container = GUID_ContainerFormatPng; quality = -1.f; }
	}

	// 把 BGRA top-down 像素编码成 PNG 写进 stream。剪切板固定用 PNG（浏览器/Electron 认）。
	bool encodePng(IStream* stream, const int w, const int h, BYTE* data)
	{
		return encodeImage(stream, w, h, data, GUID_ContainerFormatPng, -1.f);
	}

	// 取进程可执行文件名（不含扩展名），用于文件名模板里的 {{FOCUS_WINDOW_APP_NAME}}
	std::wstring processBaseName(DWORD pid)
	{
		if (!pid) return L"";
		HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		if (!h) return L"";
		wchar_t buf[MAX_PATH]{};
		DWORD n = MAX_PATH;
		std::wstring result;
		if (QueryFullProcessImageNameW(h, 0, buf, &n))
			result = std::filesystem::path(buf).stem().wstring();
		CloseHandle(h);
		return result;
	}

	// quirc 交出来的是裸字节流：BYTE 类型的二维码现实中基本都是 UTF-8（微信、支付宝
	// 生成的都是），Kanji 类型按 ISO 18004 规定是 Shift-JIS。所以先按 UTF-8 严格解，
	// 解不通再退回对应的本地代码页，避免把中文变成一堆问号
	std::wstring qrPayloadToWStr(const uint8_t* payload, const int len, const int dataType)
	{
		if (len <= 0) return L"";
		auto convert = [payload, len](UINT codePage, DWORD flags) {
			auto str = (const char*)payload;
			auto count = MultiByteToWideChar(codePage, flags, str, len, nullptr, 0);
			if (count <= 0) return std::wstring();
			std::wstring result(count, 0);
			MultiByteToWideChar(codePage, flags, str, len, result.data(), count);
			return result;
		};
		auto result = convert(CP_UTF8, MB_ERR_INVALID_CHARS);
		if (!result.empty()) return result;
		return convert(dataType == QUIRC_DATA_TYPE_KANJI ? 932 : CP_ACP, 0);
	}

	// 插件的查找顺序：先本 exe 同目录（绿色包一起解压的情况），
	// 再 %appdata%\SnowAir\plugin（后来单独下载的情况）
	std::filesystem::path findImageReader()
	{
		wchar_t buffer[MAX_PATH]{};
		GetModuleFileName(nullptr, buffer, MAX_PATH);
		auto path = std::filesystem::path{ buffer }.parent_path().append(L"ImageReader.exe");
		if (std::filesystem::exists(path)) return path;
		path = Setting::get()->getDataPath().append(L"plugin").append(L"ImageReader.exe");
		if (std::filesystem::exists(path)) return path;
		return {};
	}
}

void Util::saveToClipboard(const int w, const int h, BYTE* data)
{
	if (w <= 0 || h <= 0 || !data) return;
	DWORD rowBytes = (DWORD)w * 4;
	DWORD imgBytes = rowBytes * (DWORD)h;

	// ---------- 1) PNG 编码到内存流 ----------
	ComPtr<IStream> pngStream;
	if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, pngStream.GetAddressOf()))) return;
	if (!encodePng(pngStream.Get(), w, h, data)) return;
	// 流内部的 HGLOBAL 尺寸可能大于实际字节数，拷一份精确大小的出来给剪切板
	STATSTG stat{};
	if (FAILED(pngStream->Stat(&stat, STATFLAG_NONAME))) return;
	SIZE_T pngSize = (SIZE_T)stat.cbSize.QuadPart;
	if (pngSize == 0) return;
	HGLOBAL hPngSrc{ nullptr };
	if (FAILED(GetHGlobalFromStream(pngStream.Get(), &hPngSrc)) || !hPngSrc) return;
	auto srcPtr = GlobalLock(hPngSrc);
	if (!srcPtr) return;
	HGLOBAL hPng = GlobalAlloc(GMEM_MOVEABLE, pngSize);
	if (!hPng) { GlobalUnlock(hPngSrc); return; }
	auto dstPtr = GlobalLock(hPng);
	if (!dstPtr) { GlobalUnlock(hPngSrc); GlobalFree(hPng); return; }
	CopyMemory(dstPtr, srcPtr, pngSize);
	GlobalUnlock(hPng);
	GlobalUnlock(hPngSrc);

	// ---------- 2) 构造 CF_DIBV5（带 alpha） ----------
	HGLOBAL hDibV5 = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPV5HEADER) + imgBytes);
	if (!hDibV5) { GlobalFree(hPng); return; }
	auto pv5 = static_cast<BYTE*>(GlobalLock(hDibV5));
	if (!pv5) { GlobalFree(hDibV5); GlobalFree(hPng); return; }
	auto bv5 = reinterpret_cast<BITMAPV5HEADER*>(pv5);
	*bv5 = {};
	bv5->bV5Size = sizeof(BITMAPV5HEADER);
	bv5->bV5Width = w;
	bv5->bV5Height = -h;                  // 负 = top-down
	bv5->bV5Planes = 1;
	bv5->bV5BitCount = 32;
	bv5->bV5Compression = BI_BITFIELDS;   // 让接收端识别 alpha
	bv5->bV5SizeImage = imgBytes;
	bv5->bV5RedMask = 0x00FF0000;
	bv5->bV5GreenMask = 0x0000FF00;
	bv5->bV5BlueMask = 0x000000FF;
	bv5->bV5AlphaMask = 0xFF000000;
	bv5->bV5CSType = LCS_sRGB;
	bv5->bV5Intent = LCS_GM_GRAPHICS;
	CopyMemory(pv5 + sizeof(BITMAPV5HEADER), data, imgBytes);
	GlobalUnlock(hDibV5);

	// ---------- 3) 构造 CF_DIB（24bpp、BI_RGB、自下而上） ----------
	// 老软件（比如 Illustrator 2020）只认最传统的这一种 DIB：注册格式 PNG 它不查，
	// CF_DIBV5 它不认，32bpp + BI_BITFIELDS 和 top-down 也读不了。系统虽然能从 CF_DIBV5
	// 合成出 CF_DIB，合成出来的仍是那份带 alpha 的 32 位数据，一样不合它的口味。
	// 所以显式再放一份最保守的：丢掉 alpha 写成 24 位，行按 4 字节对齐，自下而上排列
	DWORD dibRowBytes = ((DWORD)w * 3 + 3) & ~3u;
	DWORD dibImgBytes = dibRowBytes * (DWORD)h;
	HGLOBAL hDib = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + dibImgBytes);
	if (!hDib) { GlobalFree(hDibV5); GlobalFree(hPng); return; }
	auto pDib = static_cast<BYTE*>(GlobalLock(hDib));
	if (!pDib) { GlobalFree(hDib); GlobalFree(hDibV5); GlobalFree(hPng); return; }
	auto bi = reinterpret_cast<BITMAPINFOHEADER*>(pDib);
	*bi = {};
	bi->biSize = sizeof(BITMAPINFOHEADER);
	bi->biWidth = w;
	bi->biHeight = h;                     // 正 = 自下而上
	bi->biPlanes = 1;
	bi->biBitCount = 24;
	bi->biCompression = BI_RGB;
	bi->biSizeImage = dibImgBytes;
	auto dibPixels = pDib + sizeof(BITMAPINFOHEADER);
	for (int row = 0; row < h; row++) {
		auto src = data + (size_t)row * rowBytes;                 //入参是 top-down
		auto dst = dibPixels + (size_t)(h - 1 - row) * dibRowBytes;
		for (int col = 0; col < w; col++) {
			dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];     //BGRA -> BGR
			src += 4;
			dst += 3;
		}
	}
	GlobalUnlock(hDib);

	// ---------- 4) 写入剪切板 ----------
	// OpenClipboard 可能被别的进程短暂占用：单次失败就会"复制不上"，这里有限次重试（最多约 100ms）
	bool opened = false;
	for (int attempt = 0; attempt < 10; ++attempt) {
		if (OpenClipboard(nullptr)) { opened = true; break; }
		Sleep(10);
	}
	if (!opened) {
		GlobalFree(hDib);
		GlobalFree(hDibV5);
		GlobalFree(hPng);
		return;
	}
	EmptyClipboard();
	// SetClipboardData 成功后 HGLOBAL 归剪切板所有，不能再 GlobalFree；失败了才要自己释放
	if (!SetClipboardData(CF_DIBV5, hDibV5)) {
		GlobalFree(hDibV5);
	}
	if (!SetClipboardData(CF_DIB, hDib)) {
		GlobalFree(hDib);
	}
	UINT cfPng = RegisterClipboardFormatW(L"PNG");
	if (cfPng == 0 || !SetClipboardData(cfPng, hPng)) {
		GlobalFree(hPng);
	}
	CloseClipboard();
}

bool Util::saveToFile(const std::wstring& path, const int w, const int h, BYTE* data)
{
	if (path.empty() || w <= 0 || h <= 0 || !data) return false;
	std::wstring ext = std::filesystem::path(path).extension().wstring();
	for (auto& c : ext) c = (wchar_t)towlower(c);
	// WebP 走内置编码器（libwebp）：系统 WIC 不一定带 WebP 编码器，不能再依赖它。
	// data 是 top-down BGRA，stride = w * 4。
	if (ext == L".webp") {
		std::vector<uint8_t> encoded;
		if (!WebpCodec::EncodeBGRA(data, w, h, w * 4, 90.f, false, encoded))
			return false;
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		if (!file) return false;
		file.write(reinterpret_cast<const char*>(encoded.data()), (std::streamsize)encoded.size());
		return file.good();
	}
	// 按扩展名选编码容器（PNG/JPEG/BMP）。临时文件与历史缩略图固定走 .png，
	// 所以这里的格式切换只影响用户可见的落盘文件。
	GUID container{}; float quality = -1.f;
	containerForExt(ext, container, quality);
	ComPtr<IWICImagingFactory> factory;
	auto hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf()));
	if (FAILED(hr)) return false;
	ComPtr<IWICStream> stream;
	hr = factory->CreateStream(stream.GetAddressOf());
	if (FAILED(hr)) return false;
	hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
	if (FAILED(hr)) return false;
	return encodeImage(stream.Get(), w, h, data, container, quality);
}

std::wstring Util::getSaveFilePath(HWND hwnd, const std::wstring& ext, const std::wstring& defaultName,
	const std::wstring& defaultDir)
{
	std::wstring result;
	ComPtr<IFileSaveDialog> saveDialog;
	auto hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(saveDialog.GetAddressOf()));
	if (FAILED(hr)) return result;
	DWORD dwFlags{ 0 };
	saveDialog->GetOptions(&dwFlags);
	saveDialog->SetOptions(dwFlags | FOS_OVERWRITEPROMPT | FOS_STRICTFILETYPES);
	auto pattern = L"*." + ext;
	auto typeName = Lang::get(L"util.file");
	COMDLG_FILTERSPEC filterSpec[]{ { typeName.c_str(), pattern.c_str() } };
	saveDialog->SetFileTypes(_countof(filterSpec), filterSpec);
	saveDialog->SetFileTypeIndex(1);
	saveDialog->SetDefaultExtension(ext.c_str());
	auto fileName = defaultName.empty() ? createFileName(ext) : defaultName;
	saveDialog->SetFileName(fileName.c_str());
	if (!defaultDir.empty()) {
		ComPtr<IShellItem> folder;
		if (SUCCEEDED(SHCreateItemFromParsingName(defaultDir.c_str(), nullptr, IID_PPV_ARGS(folder.GetAddressOf()))))
			// 必须用 SetFolder 而不是 SetDefaultFolder：后者只是"没有历史记录时才用"的兜底，
			// 用户上次在别处保存过之后，对话框会重开在上次那个目录，而不是这里指定的目录。
			// 手动保存要求"一打开就在桌面"，所以要每次强制初始目录。
			saveDialog->SetFolder(folder.Get());
	}
	// 用户取消时 Show 返回 HRESULT_FROM_WIN32(ERROR_CANCELLED)，一样走 FAILED 分支
	hr = saveDialog->Show(hwnd);
	if (FAILED(hr)) return result;
	ComPtr<IShellItem> item;
	hr = saveDialog->GetResult(item.GetAddressOf());
	if (FAILED(hr)) return result;
	PWSTR filePath{ nullptr };
	hr = item->GetDisplayName(SIGDN_FILESYSPATH, &filePath);
	if (FAILED(hr)) return result;
	result = filePath;
	CoTaskMemFree(filePath);
	return result;
}

std::wstring Util::createFileName(const std::wstring& ext)
{
	SYSTEMTIME st;
	GetLocalTime(&st);
	return std::format(L"{:04d}{:02d}{:02d}{:02d}{:02d}{:02d}{:03d}.{}",
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, ext);
}

std::vector<BYTE> Util::captureScreen(const int x, const int y, const int w, const int h)
{
	std::vector<BYTE> data;
	if (w <= 0 || h <= 0) return data;
	HDC hScreen = GetDC(nullptr);
	HDC hDC = CreateCompatibleDC(hScreen);
	HBITMAP hBitmap = CreateCompatibleBitmap(hScreen, w, h);
	auto oldObj = SelectObject(hDC, hBitmap);
	BitBlt(hDC, 0, 0, w, h, hScreen, x, y, SRCCOPY);
	ReleaseDC(nullptr, hScreen);
	data.resize((size_t)w * 4 * h);
	BITMAPINFO bmi{};
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = w;
	// 负高度 = top-down，第一行就是屏幕最上面那行，省掉后续所有翻转
	bmi.bmiHeader.biHeight = -h;
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;
	GetDIBits(hDC, hBitmap, 0, h, data.data(), &bmi, DIB_RGB_COLORS);
	SelectObject(hDC, oldObj);
	DeleteDC(hDC);
	DeleteObject(hBitmap);
	return data;
}

std::vector<BYTE> Util::scaleBGRA(const BYTE* src, int sw, int sh, int dw, int dh)
{
	std::vector<BYTE> out;
	if (!src || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return out;
	out.resize((size_t)dw * dh * 4);
	// Catmull-Rom（a = -0.5）双三次插值：比双线性锐利。小图/小字放大后笔画更清楚 ——
	// 双线性会把笔画糊开，Windows OCR 这类对小字敏感的场景常因此把字形认成别的字（甚至乱码）。
	auto kernel = [](float d) -> float {
		d = std::fabs(d);
		constexpr float a = -0.5f;
		if (d <= 1.f) return (a + 2.f) * d * d * d - (a + 3.f) * d * d + 1.f;
		if (d < 2.f) return a * d * d * d - 5.f * a * d * d + 8.f * a * d - 4.f * a;
		return 0.f;
	};
	for (int y = 0; y < dh; ++y) {
		const float fy = (sh == 1) ? 0.f : (y + 0.5f) * sh / dh - 0.5f;
		const int iy = (int)std::floor(fy);
		const float ty = fy - iy;
		float wy[4]; float wsumY = 0.f;
		for (int j = 0; j < 4; ++j) { wy[j] = kernel((float)(j - 1) - ty); wsumY += wy[j]; }
		if (wsumY == 0.f) wsumY = 1.f;
		for (int x = 0; x < dw; ++x) {
			const float fx = (sw == 1) ? 0.f : (x + 0.5f) * sw / dw - 0.5f;
			const int ix = (int)std::floor(fx);
			const float tx = fx - ix;
			float wx[4]; float wsumX = 0.f;
			for (int i = 0; i < 4; ++i) { wx[i] = kernel((float)(i - 1) - tx); wsumX += wx[i]; }
			if (wsumX == 0.f) wsumX = 1.f;
			const float inv = 1.f / (wsumX * wsumY);
			for (int c = 0; c < 4; ++c) {
				float acc = 0.f;
				for (int j = 0; j < 4; ++j) {
					const int yy = std::clamp(iy - 1 + j, 0, sh - 1);
					for (int i = 0; i < 4; ++i) {
						const int xx = std::clamp(ix - 1 + i, 0, sw - 1);
						acc += src[((size_t)yy * sw + xx) * 4 + c] * wx[i] * wy[j];
					}
				}
				out[((size_t)y * dw + x) * 4 + c] = (BYTE)std::clamp(acc * inv + 0.5f, 0.f, 255.f);
			}
		}
	}
	return out;
}

void Util::addFileToClipboard(const std::wstring& filePath)
{
	// 同「复制图片」：OpenClipboard 可能被别的进程短暂占用，有限次重试
	bool opened = false;
	for (int attempt = 0; attempt < 10; ++attempt) {
		if (OpenClipboard(nullptr)) { opened = true; break; }
		Sleep(10);
	}
	if (!opened) return;
	EmptyClipboard();
	// DROPFILES 之后紧跟双 \0 结尾的路径列表，这里只放一条
	auto totalSize = sizeof(DROPFILES) + (filePath.length() + 2) * sizeof(wchar_t);
	auto hGlobal = GlobalAlloc(GMEM_MOVEABLE, totalSize);
	if (!hGlobal) {
		CloseClipboard();
		return;
	}
	auto pDropFiles = static_cast<DROPFILES*>(GlobalLock(hGlobal));
	if (!pDropFiles) {
		GlobalFree(hGlobal);
		CloseClipboard();
		return;
	}
	pDropFiles->pFiles = sizeof(DROPFILES);
	pDropFiles->fWide = TRUE;
	auto dest = reinterpret_cast<wchar_t*>(pDropFiles + 1);
	wcscpy_s(dest, filePath.length() + 1, filePath.c_str());
	dest[filePath.length() + 1] = L'\0';
	GlobalUnlock(hGlobal);
	// 成功后 HGLOBAL 归剪切板所有，只在失败时自己释放
	if (!SetClipboardData(CF_HDROP, hGlobal)) {
		GlobalFree(hGlobal);
	}
	CloseClipboard();
}

bool Util::openWithImageReader(const int w, const int h, BYTE* data)
{
	// Legacy optional path: OCR/translate now use in-process OcrService + ToolOcr.
	// Kept for manual/plugin fallback only; WinCap no longer calls this.
	auto exePath = findImageReader();
	if (exePath.empty()) {
		// 插件没装，直接把用户带到下载页，不再多弹一层提示
		ShellExecute(nullptr, L"open", L"https://github.com/xland/ImageReader/releases", nullptr, nullptr, SW_SHOWNORMAL);
		return false;
	}
	auto imgPath = Setting::get()->getDataPath().append(L"ocr_" + createFileName(L"png")).wstring();
	if (!saveToFile(imgPath, w, h, data)) return false;
	// --del-image=true：插件读完自己把缓存图删掉，免得在数据目录里越攒越多
	auto cmd = std::format(L"\"{}\" --image-path=\"{}\" --del-image=true", exePath.wstring(), imgPath);
	// 工作目录设成插件所在目录，它才找得到自己身边的依赖
	auto workDir = exePath.parent_path().wstring();
	STARTUPINFO si{ .cb = sizeof(STARTUPINFO) };
	PROCESS_INFORMATION pi{};
	if (!CreateProcess(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, workDir.data(), &si, &pi)) {
		std::error_code ec;
		std::filesystem::remove(imgPath, ec); //插件没起来，别留下垃圾文件
		return false;
	}
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return true;
}

std::wstring Util::decodeQrCode(const int w, const int h, BYTE* data)
{
	std::wstring result;
	if (w <= 0 || h <= 0 || !data) return result;

	auto utf8ToW = [](const std::string& s) {
		if (s.empty()) return std::wstring();
		int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
		if (n <= 0) return std::wstring();
		std::wstring out(n, 0);
		MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
		return out;
	};

	// ZXing：二维码 + 常见一维条码
	try {
		ZXing::ImageView image(data, w, h, ZXing::ImageFormat::BGRA);
		ZXing::ReaderOptions opts;
		opts.setTryHarder(true);
		opts.setTryRotate(true);
		auto barcodes = ZXing::ReadBarcodes(image, opts);
		for (const auto& code : barcodes) {
			if (!code.isValid()) continue;
			auto text = utf8ToW(code.text());
			if (text.empty()) continue;
			if (!result.empty()) result += L'\n';
			result += text;
		}
		if (!result.empty()) return result;
	}
	catch (...) {
	}

	// quirc 兜底（仅 QR）
	auto qr = quirc_new();
	if (!qr) return result;
	if (quirc_resize(qr, w, h) < 0) {
		quirc_destroy(qr);
		return result;
	}
	int bufW{ 0 }, bufH{ 0 };
	auto buffer = quirc_begin(qr, &bufW, &bufH);
	const size_t count = (size_t)w * h;
	for (size_t i = 0; i < count; i++) {
		auto px = data + i * 4;
		buffer[i] = (uint8_t)((px[2] * 77 + px[1] * 150 + px[0] * 29) >> 8);
	}
	quirc_end(qr);
	auto codeCount = quirc_count(qr);
	for (int i = 0; i < codeCount; i++) {
		quirc_code code{};
		quirc_data qrData{};
		quirc_extract(qr, i, &code);
		auto err = quirc_decode(&code, &qrData);
		if (err == QUIRC_ERROR_DATA_ECC) {
			quirc_flip(&code);
			err = quirc_decode(&code, &qrData);
		}
		if (err != QUIRC_SUCCESS) continue;
		auto text = qrPayloadToWStr(qrData.payload, qrData.payload_len, qrData.data_type);
		if (text.empty()) continue;
		if (!result.empty()) result += L"\n";
		result += text;
	}
	quirc_destroy(qr);
	return result;
}

void Util::cursorBodyOffset(float& dx, float& dy)
{
	dx = 0.f;
	dy = 0.f;

	// 形状不变时复用上次结果：本函数由录制期的光标轮询驱动，逐次重画位图不划算
	static HCURSOR s_cur = nullptr;
	static bool s_valid = false;
	static float s_dx = 0.f;
	static float s_dy = 0.f;

	CURSORINFO ci{};
	ci.cbSize = sizeof(ci);
	if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) return;
	if (ci.hCursor == s_cur) {
		if (s_valid) { dx = s_dx; dy = s_dy; }
		return;
	}
	s_cur = ci.hCursor;
	s_valid = false;

	ICONINFO ii{};
	if (!GetIconInfo(ci.hCursor, &ii)) return;
	// 单色光标只有 AND/XOR 掩码（总高度为图形高度的两倍），没有颜色位图
	const bool mono = (ii.hbmColor == nullptr);
	BITMAP bm{};
	if (GetObjectW(mono ? ii.hbmMask : ii.hbmColor, sizeof(bm), &bm)
		&& bm.bmWidth > 0 && bm.bmHeight > 0) {
		const int w = bm.bmWidth;
		const int h = mono ? bm.bmHeight / 2 : bm.bmHeight;
		HDC screen = (h > 0) ? GetDC(nullptr) : nullptr;
		HDC mem = screen ? CreateCompatibleDC(screen) : nullptr;
		void* bits = nullptr;
		HBITMAP dib = nullptr;
		if (mem) {
			// 32bpp、top-down：DrawIconEx 会把带 alpha 的光标原样画进来；屏幕坐标系 y 向下，与热点一致
			BITMAPINFO bi{};
			bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
			bi.bmiHeader.biWidth = w;
			bi.bmiHeader.biHeight = -h;
			bi.bmiHeader.biPlanes = 1;
			bi.bmiHeader.biBitCount = 32;
			bi.bmiHeader.biCompression = BI_RGB;
			dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
		}

		int minX = w, minY = h, maxX = -1, maxY = -1;
		if (dib && bits) {
			auto* px = static_cast<UINT32*>(bits);
			// 单色光标没有 alpha 通道：先铺一层哨兵色，未被图形覆盖的像素保持哨兵值
			constexpr UINT32 kSentinel = 0xFFFF00FFu;
			if (mono) {
				for (size_t i = 0, n = (size_t)w * h; i < n; i++) px[i] = kSentinel;
			}
			HGDIOBJ old = SelectObject(mem, dib);
			DrawIconEx(mem, 0, 0, ci.hCursor, w, h, 0, nullptr, DI_NORMAL);
			SelectObject(mem, old);
			for (int y = 0; y < h; y++) {
				for (int x = 0; x < w; x++) {
					const UINT32 v = px[(size_t)y * w + x];
					const bool opaque = mono ? (v != kSentinel) : ((v >> 24) != 0u);
					if (!opaque) continue;
					if (x < minX) minX = x;
					if (x > maxX) maxX = x;
					if (y < minY) minY = y;
					if (y > maxY) maxY = y;
				}
			}
		}

		if (dib) DeleteObject(dib);
		if (mem) DeleteDC(mem);
		if (screen) ReleaseDC(nullptr, screen);

		// 位图坐标与热点同以左上为原点、y 向下，包围盒中心减热点即得偏置
		if (maxX >= minX && maxY >= minY) {
			s_dx = (minX + maxX) * 0.5f - (float)ii.xHotspot;
			s_dy = (minY + maxY) * 0.5f - (float)ii.yHotspot;
			s_valid = true;
			dx = s_dx;
			dy = s_dy;
		}
	}
	if (ii.hbmColor) DeleteObject(ii.hbmColor);
	if (ii.hbmMask) DeleteObject(ii.hbmMask);
}

// ================= History 辅助：id → 条目 filePath 查找 =================
static std::wstring findHistoryFilePath(const std::wstring& id)
{
	auto list = Setting::get()->getHistoryItems();
	for (auto& it : list) if (it.id == id) return it.filePath;
	// 目录扫描出来的条目把路径直接当 id
	std::error_code ec;
	if (std::filesystem::exists(id, ec)) return id;
	return L"";
}

// WIC 从文件解码成 BGRA、top-down、行紧凑 (步长 = w*4) 向量；失败返回 {0,0,空}
static std::tuple<int, int, std::vector<BYTE>> loadImagePixels(const std::wstring& path)
{
	std::tuple<int, int, std::vector<BYTE>> empty{ 0, 0, {} };
	if (path.empty() || !std::filesystem::exists(path)) return empty;
	ComPtr<IWICImagingFactory> factory;
	if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf()))))
		return empty;
	ComPtr<IWICBitmapDecoder> decoder;
	if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf())))
		return empty;
	ComPtr<IWICBitmapFrameDecode> frame;
	if (FAILED(decoder->GetFrame(0, frame.GetAddressOf()))) return empty;
	UINT w = 0, h = 0;
	if (FAILED(frame->GetSize(&w, &h))) return empty;
	if (w == 0 || h == 0) return empty;
	// 转 32bpp BGRA
	ComPtr<IWICFormatConverter> conv;
	if (FAILED(factory->CreateFormatConverter(conv.GetAddressOf()))) return empty;
	if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
		nullptr, 0.0, WICBitmapPaletteTypeCustom))) return empty;
	std::vector<BYTE> data(size_t(w) * 4u * h);
	UINT stride = (UINT)w * 4;
	if (FAILED(conv->CopyPixels(nullptr, stride, (UINT)data.size(), data.data()))) return empty;
	return { (int)w, (int)h, std::move(data) };
}

void Util::copyHistoryFileToClipboard(const std::wstring& historyId)
{
	auto path = findHistoryFilePath(historyId);
	if (path.empty()) return;
	addFileToClipboard(path);
}

void Util::pinHistoryFile(const std::wstring& historyId)
{
	auto path = findHistoryFilePath(historyId);
	if (path.empty()) return;
	auto [w, h, data] = loadImagePixels(path);
	if (w <= 0 || h <= 0 || data.empty()) return;
	// 位置：屏幕工作区中央
	RECT rc{};
	SystemParametersInfo(SPI_GETWORKAREA, 0, &rc, 0);
	int areaW = rc.right - rc.left;
	int areaH = rc.bottom - rc.top;
	int x = rc.left + (std::max)(0, (areaW - w) / 2);
	int y = rc.top + (std::max)(0, (areaH - h) / 2);
	WinPin::initFromData(x, y, w, h, data);
}

// ============ 截图输出路径 ============

std::filesystem::path Util::defaultOutputDir()
{
	PWSTR pictures{ nullptr };
	if (FAILED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pictures)) || !pictures) {
		if (pictures) CoTaskMemFree(pictures);
		return {};
	}
	std::filesystem::path dir{ pictures };
	CoTaskMemFree(pictures);
	dir.append(L"SnowAir");
	return dir;
}

// 手动保存（工具栏「保存」）对话框的起始目录：桌面。用户仍可在对话框里改到别处
std::filesystem::path Util::desktopDir()
{
	PWSTR desktop{ nullptr };
	if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop)) || !desktop) {
		if (desktop) CoTaskMemFree(desktop);
		return {};
	}
	std::filesystem::path dir{ desktop };
	CoTaskMemFree(desktop);
	return dir;
}

std::filesystem::path Util::outputDir()
{
	// 「文件输出」里自定义了截图目录就用它；否则用系统「图片」下的 SnowAir
	auto* setting = Setting::get();
	auto custom = setting ? setting->getScreenshotDir() : std::wstring{};
	auto dir = custom.empty() ? defaultOutputDir() : std::filesystem::path{ custom };
	if (dir.empty()) return {};
	std::error_code ec;
	if (!std::filesystem::exists(dir, ec)) std::filesystem::create_directories(dir, ec);
	return dir;
}

namespace {
	// 路径分段里不能出现的字符，统一换成 '_'；末尾的点和空格也去掉（Windows 会自己吃掉，
	// 留着会让「文件名」和实际落盘名不一致）
	std::wstring sanitizeSegment(std::wstring s)
	{
		for (auto& c : s) {
			if (c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' ||
				c == L'"' || c == L'<' || c == L'>' || c == L'|') {
				c = L'_';
			}
		}
		while (!s.empty() && (s.back() == L'.' || s.back() == L' ')) s.pop_back();
		return s.empty() ? std::wstring{ L"Unknown" } : s;
	}

	// {{...}} 里的时间标记展开；认不出（含非法字符）就原样保留 {{...}}
	std::wstring expandFileNameTemplate(const std::wstring& format, const std::wstring& focusAppName)
	{
		if (format.empty()) return {};
		SYSTEMTIME st{};
		GetLocalTime(&st);
		const std::wstring app = focusAppName.empty() ? L"Unknown" : focusAppName;
		auto num = [](int v) { return std::format(L"{:02d}", v); };
		std::wstring out;
		out.reserve(format.size());
		size_t i = 0;
		while (i < format.size()) {
			if (format[i] == L'{' && i + 1 < format.size() && format[i + 1] == L'{') {
				auto end = format.find(L"}}", i + 2);
				if (end == std::wstring::npos) {
					out += format.substr(i);
					break;
				}
				const std::wstring token = format.substr(i + 2, end - (i + 2));
				if (token == L"FOCUS_WINDOW_APP_NAME") {
					out += sanitizeSegment(app);
				}
				else {
					bool dateLike = !token.empty();
					for (auto c : token) {
						const bool ok = (c >= L'0' && c <= L'9') || c == L'Y' || c == L'M' || c == L'D'
							|| c == L'H' || c == L'm' || c == L's' || c == L'-' || c == L'_'
							|| c == L':' || c == L'/' || c == L' ' || c == L'.';
						if (!ok) { dateLike = false; break; }
					}
					if (!dateLike) {
						out += L"{{" + token + L"}}";
					}
					else {
						std::wstring pat = token;
						auto replaceAll = [&pat](const std::wstring& from, const std::wstring& to) {
							for (size_t pos = pat.find(from); pos != std::wstring::npos; pos = pat.find(from, pos + to.size()))
								pat.replace(pos, from.size(), to);
						};
						replaceAll(L"YYYY", std::to_wstring(st.wYear));
						replaceAll(L"MM", num(st.wMonth));
						replaceAll(L"DD", num(st.wDay));
						replaceAll(L"HH", num(st.wHour));
						replaceAll(L"mm", num(st.wMinute));
						replaceAll(L"ss", num(st.wSecond));
						out += pat;
					}
				}
				i = end + 2;
			}
			else {
				out += format[i++];
			}
		}
		return out;
	}
}

std::wstring Util::buildSavePath(const std::wstring& format, const std::wstring& fallbackFormat,
	const std::wstring& ext, const std::wstring& focusAppName)
{
	std::wstring stem = expandFileNameTemplate(format, focusAppName);
	if (stem.empty()) stem = expandFileNameTemplate(fallbackFormat, focusAppName);
	if (stem.empty()) return {};
	for (auto& c : stem) if (c == L'\\') c = L'/';
	while (!stem.empty() && stem.front() == L'/') stem.erase(stem.begin());

	auto root = outputDir();
	if (root.empty()) return {};

	// 逐段切分（模板里的 / 代表子目录）
	std::vector<std::wstring> parts;
	size_t pos = 0;
	while (pos <= stem.size()) {
		auto slash = stem.find(L'/', pos);
		std::wstring seg = stem.substr(pos, slash == std::wstring::npos ? std::wstring::npos : slash - pos);
		if (!seg.empty()) parts.push_back(seg);
		if (slash == std::wstring::npos) break;
		pos = slash + 1;
	}
	if (parts.empty()) parts.push_back(L"SnowAir");

	std::wstring fileName = sanitizeSegment(parts.back());
	parts.pop_back();
	if (fileName.find(L'.') == std::wstring::npos && !ext.empty()) fileName += L"." + ext;

	std::filesystem::path dir = root;
	std::error_code ec;
	for (auto& p : parts) {
		dir.append(sanitizeSegment(p));
		if (!std::filesystem::exists(dir, ec)) std::filesystem::create_directories(dir, ec);
	}
	return (dir / fileName).wstring();
}

std::wstring Util::previewFileName(const std::wstring& format, const std::wstring& fallbackFormat,
	const std::wstring& focusAppName)
{
	std::wstring stem = expandFileNameTemplate(format, focusAppName);
	if (stem.empty()) stem = expandFileNameTemplate(fallbackFormat, focusAppName);
	for (auto& c : stem) if (c == L'\\') c = L'/';
	while (!stem.empty() && stem.front() == L'/') stem.erase(stem.begin());
	return stem;
}

std::wstring Util::extensionFromSaveFormat(const std::wstring& fmt)
{
	std::wstring lower = fmt;
	for (auto& c : lower) c = (wchar_t)towlower(c);
	if (lower.find(L"jpg") != std::wstring::npos || lower.find(L"jpeg") != std::wstring::npos) return L"jpg";
	if (lower.find(L"webp") != std::wstring::npos) return L"webp";
	if (lower.find(L"bmp") != std::wstring::npos) return L"bmp";
	if (lower.find(L"avif") != std::wstring::npos) return L"avif";
	if (lower.find(L"jxl") != std::wstring::npos) return L"jxl";
	return L"png";
}

// 当前前台窗口所属进程名（不含扩展名）；本进程自己（截图覆盖窗等）返回空。
// 需在覆盖窗出现前调用，否则前台已是本程序。
std::wstring Util::foregroundAppName()
{
	HWND fg = GetForegroundWindow();
	if (!fg) return L"";
	DWORD pid = 0;
	GetWindowThreadProcessId(fg, &pid);
	if (!pid || pid == GetCurrentProcessId()) return L"";
	return processBaseName(pid);
}

// 删除文件到系统回收站：SHFileOperation 的 FOF_ALLOWUNDO；失败再退回永久删除
bool Util::moveToRecycleBin(const std::wstring& path)
{
	if (path.empty()) return false;
	std::wstring from = path;
	from.push_back(L'\0');   // SHFileOperation 要求双空结尾
	SHFILEOPSTRUCTW op{};
	op.wFunc = FO_DELETE;
	op.pFrom = from.c_str();
	op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
	const bool ok = SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
	if (ok) return true;
	std::error_code ec;
	return std::filesystem::remove(path, ec);
}

// 自动落盘（取消退出共用）：用「自动保存文件名格式」+「保存格式」写到截图目录。
// 返回落盘路径（空 = 失败）。
std::wstring Util::saveAutoCapture(const std::vector<BYTE>& bgra, const int w, const int h,
	const std::wstring& focusAppName)
{
	if (w <= 0 || h <= 0 || bgra.empty()) return {};
	auto* s = Setting::get();
	if (!s) return {};
	const std::wstring ext = extensionFromSaveFormat(s->getSaveFormat());
	const std::wstring path = buildSavePath(s->getAutoSaveFormat(), Setting::defaultAutoSaveFormat(),
		ext, focusAppName);
	if (path.empty()) return {};
	if (!saveToFile(path, w, h, const_cast<BYTE*>(bgra.data()))) return {};
	return path;
}

// 正常完成截图落盘：用「手动保存文件名格式」（和“自动保存”文件名区分开，
// 历史页就能按文件名把两种来源分开）。
std::wstring Util::saveCompletedCapture(const std::vector<BYTE>& bgra, const int w, const int h,
	const std::wstring& focusAppName)
{
	if (w <= 0 || h <= 0 || bgra.empty()) return {};
	auto* s = Setting::get();
	if (!s) return {};
	const std::wstring ext = extensionFromSaveFormat(s->getSaveFormat());
	const std::wstring path = buildSavePath(s->getManualSaveFormat(), Setting::defaultManualSaveFormat(),
		ext, focusAppName);
	if (path.empty()) return {};
	if (!saveToFile(path, w, h, const_cast<BYTE*>(bgra.data()))) return {};
	return path;
}

// 用户主动退出截图（Esc / 关闭）时按「截图自动保存」静默落盘（不弹对话框、不进剪贴板）。
std::wstring Util::quietSaveCapture(const std::vector<BYTE>& bgra, const int w, const int h,
	const std::wstring& focusAppName)
{
	auto* s = Setting::get();
	if (!s || !s->getAutoSaveAfterCapture()) return {};
	return saveAutoCapture(bgra, w, h, focusAppName);
}


