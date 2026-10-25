#include "pch.h"
#include "ScreenCapture.h"
#include "../Util.h"
#include <d3d11.h>
#include <dxgi1_6.h>
#include <dwmapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// HDR 屏截图：DXGI 桌面复制 + 软件色调映射
//
// 关键点（和 HDRSnip 同一套思路）：
//  1. 向系统要 R16G16B16A16_FLOAT（scRGB 线性，1.0 = 80 尼特），8 位兼容表面在 HDR 下
//     会把 >1.0 的高光全部截成 255，这是"白成一片"的根源，所以 8 位通路一律不走。
//  2. 会话（设备 / 复制 / 暂存纹理）按显示器长驻并复用；建设和销毁都要几十到几百毫秒。
//  3. 复制出来的是"变化流"：新会话常常先给一张全黑空帧。这里照 HDRSnip 的做法——
//     黑帧重试、桌面静止时直接返回暂存纹理里的上一帧、ACCESS_LOST 就重建会话。
//  4. 色调映射用 64K 查表（half 只有 65536 种位模式），像素循环里没有 pow：
//     v / (SDR白点 / 80) → clamp → sRGB 编码。SDR 白点取 Windows 的"SDR 内容亮度"。
//  5. 只转换请求矩形覆盖到的行，选区截图不再为整屏买单。
// ---------------------------------------------------------------------------

namespace {

constexpr ULONGLONG kGrabBudgetMs = 3000;    // 单块屏拿不到画面的最长等待
constexpr ULONGLONG kSessionIdleMs = 5000;   // 会话闲置多久释放（保住空载内存）
constexpr int kMaxBlankFrames = 3;           // 新会话连收几张全黑才接受
constexpr int kMaxRebuilds = 3;              // 设备丢失 / 复制失效的重建次数上限
constexpr float kSdrReferenceNits = 80.f;    // scRGB 的参考白：1.0 对应 80 尼特

struct DisplayInfo
{
	bool advanced{ false };              // 高级颜色（HDR / 广色域 / 自动颜色管理）
	float sdrWhiteNits{ kSdrReferenceNits };
};

struct MonitorEntry
{
	RECT rect{};
	std::wstring device;
};

BOOL CALLBACK collectMonitor(HMONITOR mon, HDC, LPRECT, LPARAM lp)
{
	MONITORINFOEXW mi{ sizeof(MONITORINFOEXW) };
	if (GetMonitorInfoW(mon, &mi))
		reinterpret_cast<std::vector<MonitorEntry>*>(lp)->push_back({ mi.rcMonitor, mi.szDevice });
	return TRUE;
}

// 每块屏是否处在高级颜色模式，以及 SDR 白点（Windows「SDR 内容亮度」）
std::vector<std::pair<std::wstring, DisplayInfo>> queryDisplays()
{
	std::vector<std::pair<std::wstring, DisplayInfo>> out;
	UINT32 pathCount = 0, modeCount = 0;
	if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
		return out;
	std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
	std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
	if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS)
		return out;

	for (UINT32 i = 0; i < pathCount; ++i) {
		DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
		src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
		src.header.size = sizeof(src);
		src.header.adapterId = paths[i].sourceInfo.adapterId;
		src.header.id = paths[i].sourceInfo.id;
		if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;

		DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO color{};
		color.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
		color.header.size = sizeof(color);
		color.header.adapterId = paths[i].targetInfo.adapterId;
		color.header.id = paths[i].targetInfo.id;
		const bool advanced = DisplayConfigGetDeviceInfo(&color.header) == ERROR_SUCCESS
			&& color.advancedColorEnabled;

		DisplayInfo info{};
		info.advanced = advanced;
		if (advanced) {
			DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
			white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
			white.header.size = sizeof(white);
			white.header.adapterId = paths[i].targetInfo.adapterId;
			white.header.id = paths[i].targetInfo.id;
			if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS && white.SDRWhiteLevel > 0)
				info.sdrWhiteNits = static_cast<float>(white.SDRWhiteLevel) * 80.f / 1000.f;
		}
		out.push_back({ src.viewGdiDeviceName, info });
	}
	return out;
}

DisplayInfo findDisplay(const std::vector<std::pair<std::wstring, DisplayInfo>>& map, const std::wstring& device)
{
	for (const auto& entry : map)
		if (entry.first == device) return entry.second;
	return {};
}

bool sameRect(const RECT& a, const RECT& b)
{
	return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

// ---------------------------------------------------------------- 诊断
// 只在异常路径写，正常运行一行不打。要看现场数据就设环境变量 SNOWAIR_HDR_LOG=1，
// 日志落到 %TEMP%\SnowAir-hdr.log（UTF-8）。
void trace(const std::wstring& text)
{
	static auto* count = new std::atomic<int>(0);
	if (count->fetch_add(1) >= 80) return;
	const std::wstring line = L"[SnowAir/HDR] " + text + L"\n";
	OutputDebugStringW(line.c_str());

	static const bool toFile = [] {
		wchar_t value[8]{};
		return GetEnvironmentVariableW(L"SNOWAIR_HDR_LOG", value, _countof(value)) > 0;
	}();
	if (!toFile) return;

	wchar_t dir[MAX_PATH]{};
	if (!GetTempPathW(MAX_PATH, dir)) return;
	const std::wstring path = std::wstring(dir) + L"SnowAir-hdr.log";
	FILE* file = nullptr;
	if (_wfopen_s(&file, path.c_str(), L"ab") != 0 || !file) return;
	const int size = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), nullptr, 0, nullptr, nullptr);
	if (size > 0) {
		std::string utf8(static_cast<size_t>(size), '\0');
		WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), utf8.data(), size, nullptr, nullptr);
		fwrite(utf8.data(), 1, utf8.size(), file);
	}
	fclose(file);
}

// ---------------------------------------------------------------- 颜色
unsigned char linearToSrgbByte(float v)
{
	if (!(v > 0.f)) return 0;
	if (v >= 1.f) return 255;
	const float s = v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f;
	const int q = static_cast<int>(std::lround(std::clamp(s, 0.f, 1.f) * 255.f));
	return static_cast<unsigned char>(std::clamp(q, 0, 255));
}

float halfToFloat(unsigned short h)
{
	const unsigned int sign = (h & 0x8000u) << 16;
	unsigned int exp = (h >> 10) & 0x1Fu;
	unsigned int mant = h & 0x3FFu;
	unsigned int bits;
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
		bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
	}
	float out;
	memcpy(&out, &bits, sizeof(out));
	return out;
}

// ---------------------------------------------------------------- 会话
struct SessionKey
{
	LUID luid{};
	UINT outputIndex{ 0 };
	bool operator==(const SessionKey& other) const
	{
		return luid.LowPart == other.luid.LowPart && luid.HighPart == other.luid.HighPart
			&& outputIndex == other.outputIndex;
	}
};

struct DxgiSession
{
	std::mutex lock;                       // 同一块屏的抓取串行化（长截图 / 多入口并发）
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	ComPtr<IDXGIOutputDuplication> dupl;
	ComPtr<ID3D11Texture2D> staging;
	DXGI_FORMAT format{ DXGI_FORMAT_UNKNOWN };
	RECT monitor{};
	int width{ 0 };
	int height{ 0 };
	bool haveFrame{ false };               // staging 里是否已有一张可信画面
	float whiteNits{ kSdrReferenceNits };
	bool lutValid{ false };
	float lutWhite{ 0.f };
	std::array<unsigned char, 65536> lut{};    // half 位模式 → 8 位 sRGB
	std::atomic<ULONGLONG> lastUse{ 0 };
};

bool isLost(HRESULT hr)
{
	return hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_ACCESS_DENIED
		|| hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET
		|| hr == DXGI_ERROR_DEVICE_HUNG || hr == DXGI_ERROR_SESSION_DISCONNECTED;
}

void resetSession(DxgiSession& s)
{
	s.dupl.Reset();
	s.staging.Reset();
	s.context.Reset();
	s.device.Reset();
	s.format = DXGI_FORMAT_UNKNOWN;
	s.monitor = {};
	s.width = s.height = 0;
	s.haveFrame = false;
}

void ensureLut(DxgiSession& s)
{
	const float white = s.whiteNits > 1.f ? s.whiteNits : kSdrReferenceNits;
	if (s.lutValid && std::fabs(s.lutWhite - white) < 0.5f) return;

	// 撤掉系统对 SDR 白点的抬升，再编码回 sRGB；超过白点的高光先裁平，避免死白
	const float inv = 1.f / (std::max)(white / kSdrReferenceNits, 0.01f);
	for (int bits = 0; bits < 65536; ++bits) {
		float v = halfToFloat(static_cast<unsigned short>(bits)) * inv;
		if (!(v > 0.f)) v = 0.f;
		else if (v > 1.f) v = 1.f;
		s.lut[bits] = linearToSrgbByte(v);
	}
	s.lutWhite = white;
	s.lutValid = true;
}

bool createSession(DxgiSession& s, IDXGIAdapter1* adapter, IDXGIOutput* output, const RECT& monitor, const DisplayInfo& info)
{
	resetSession(s);
	if (!adapter || !output) return false;
	s.monitor = monitor;

	const D3D_FEATURE_LEVEL levels[] = {
		D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1 };
	const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
	HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
		levels, _countof(levels), D3D11_SDK_VERSION, &s.device, nullptr, &s.context);
	if (FAILED(hr)) {
		// 个别老驱动不支持 11_1，退到 11_0 再试
		hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
			levels + 1, _countof(levels) - 1, D3D11_SDK_VERSION, &s.device, nullptr, &s.context);
	}
	if (FAILED(hr) || !s.device || !s.context) {
		resetSession(s);
		trace(L"device create failed");
		return false;
	}

	ComPtr<IDXGIOutput5> out5;
	if (FAILED(output->QueryInterface(IID_PPV_ARGS(&out5))) || !out5) {
		resetSession(s);
		trace(L"IDXGIOutput5 unavailable");
		return false;
	}

	// 高级颜色屏要原生的 FP16(scRGB)；拿不到再退 8 位，让系统自己转
	DXGI_FORMAT want[2]{ DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM };
	UINT count = 1;
	if (info.advanced) {
		want[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
		want[1] = DXGI_FORMAT_B8G8R8A8_UNORM;
		count = 2;
	}
	hr = out5->DuplicateOutput1(s.device.Get(), 0, count, want, &s.dupl);
	if (FAILED(hr) && count == 2) {
		want[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
		hr = out5->DuplicateOutput1(s.device.Get(), 0, 1, want, &s.dupl);
	}
	if (FAILED(hr) || !s.dupl) {
		resetSession(s);
		trace(L"duplication failed, hr=" + std::to_wstring(static_cast<long>(hr)));
		return false;
	}

	// 新会话的第一帧要等 DWM 合成一次才有内容
	DwmFlush();
	return true;
}

bool ensureStaging(DxgiSession& s, const D3D11_TEXTURE2D_DESC& source)
{
	if (s.staging && s.width == (int)source.Width && s.height == (int)source.Height && s.format == source.Format)
		return true;

	s.staging.Reset();
	s.haveFrame = false;
	s.width = (int)source.Width;
	s.height = (int)source.Height;
	s.format = source.Format;

	D3D11_TEXTURE2D_DESC desc = source;
	desc.Usage = D3D11_USAGE_STAGING;
	desc.BindFlags = 0;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	desc.MiscFlags = 0;
	desc.ArraySize = 1;
	desc.MipLevels = 1;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	if (FAILED(s.device->CreateTexture2D(&desc, nullptr, &s.staging)) || !s.staging) {
		s.staging.Reset();
		return false;
	}
	trace(L"format=" + std::to_wstring((int)s.format)
		+ L" size=" + std::to_wstring(s.width) + L"x" + std::to_wstring(s.height));
	return true;
}

// 把暂存纹理里与请求矩形相交的行转成 BGRA8。blank 供调用方判断"是不是空帧"。
bool readStaging(DxgiSession& s, std::vector<BYTE>& dst, int dstW, int dstH, int dstX, int dstY, bool& blank)
{
	blank = false;
	if (!s.staging || !s.context) return false;

	D3D11_MAPPED_SUBRESOURCE map{};
	if (FAILED(s.context->Map(s.staging.Get(), 0, D3D11_MAP_READ, 0, &map)) || !map.pData)
		return false;

	const bool halfFloat = s.format == DXGI_FORMAT_R16G16B16A16_FLOAT;

	// 抽查整块屏是否全黑（不是只看选区）：选区恰好落在黑色区域时不该误判，
	// 而整屏突然全黑说明复制表面已经死了（休眠恢复后常见）
	if (halfFloat) {
		const BYTE* base = reinterpret_cast<const BYTE*>(map.pData);
		const int rowStep = (std::max)(1, s.height / 64);
		const int colStep = (std::max)(1, s.width / 128);
		bool any = false;
		for (int y = 0; y < s.height && !any; y += rowStep) {
			const unsigned short* row = reinterpret_cast<const unsigned short*>(base + (size_t)y * map.RowPitch);
			for (int x = 0; x < s.width; x += colStep) {
				const unsigned short* px = row + (size_t)x * 4;
				if (halfToFloat(px[0]) > 1e-3f || halfToFloat(px[1]) > 1e-3f || halfToFloat(px[2]) > 1e-3f) {
					any = true;
					break;
				}
			}
		}
		blank = !any;
	}

	const int sx = (std::max)(0, -dstX);
	const int sy = (std::max)(0, -dstY);
	const int ex = (std::min)(s.width, dstW - dstX);
	const int ey = (std::min)(s.height, dstH - dstY);

	if (!blank && ex > sx && ey > sy) {
		if (halfFloat) {
			ensureLut(s);
			const unsigned char* lut = s.lut.data();
			for (int y = sy; y < ey; ++y) {
				const unsigned short* src = reinterpret_cast<const unsigned short*>(
					reinterpret_cast<const BYTE*>(map.pData) + (size_t)y * map.RowPitch) + (size_t)sx * 4;
				BYTE* dp = dst.data() + ((size_t)(dstY + y) * dstW + (dstX + sx)) * 4;
				for (int x = sx; x < ex; ++x, src += 4, dp += 4) {
					dp[0] = lut[src[2]];
					dp[1] = lut[src[1]];
					dp[2] = lut[src[0]];
					dp[3] = 0xFF;
				}
			}
		}
		else {
			const int bytes = (ex - sx) * 4;
			for (int y = sy; y < ey; ++y) {
				const BYTE* src = reinterpret_cast<const BYTE*>(map.pData) + (size_t)y * map.RowPitch + (size_t)sx * 4;
				BYTE* dp = dst.data() + ((size_t)(dstY + y) * dstW + (dstX + sx)) * 4;
				memcpy(dp, src, (size_t)bytes);
				// 桌面复制的 alpha 不保证是 255，统一成不透明
				for (int x = 0; x < ex - sx; ++x) dp[x * 4 + 3] = 0xFF;
			}
		}
	}

	s.context->Unmap(s.staging.Get(), 0);
	return true;
}

bool grabMonitor(DxgiSession& s, IDXGIAdapter1* adapter, IDXGIOutput* output, const RECT& monitor,
	const DisplayInfo& info, std::vector<BYTE>& dst, int dstW, int dstH, int dstX, int dstY)
{
	std::lock_guard<std::mutex> guard(s.lock);
	s.lastUse = GetTickCount64();
	s.whiteNits = info.sdrWhiteNits;

	// 显示器排列变化后，LUID+索引可能指向另一块屏：对不上就重建
	if (s.dupl && !sameRect(s.monitor, monitor)) resetSession(s);
	if (!s.dupl || !s.device || !s.context) {
		if (!createSession(s, adapter, output, monitor, info)) return false;
	}

	const ULONGLONG deadline = GetTickCount64() + kGrabBudgetMs;
	bool flushed = false;
	int blanks = 0;
	int rebuilds = 0;

	for (;;) {
		// 有缓存帧时不等待：有新帧就取，没有就直接用暂存纹理里的上一张（静止桌面 0ms）
		ComPtr<IDXGIResource> res;
		DXGI_OUTDUPL_FRAME_INFO fi{};
		HRESULT hr = s.dupl->AcquireNextFrame(s.haveFrame ? 0u : 200u, &fi, &res);

		if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
			if (s.haveFrame && s.staging) {
				bool blank = false;
				return readStaging(s, dst, dstW, dstH, dstX, dstY, blank);
			}
			if (GetTickCount64() >= deadline) {
				trace(L"acquire timeout, no frame");
				return false;
			}
			if (!flushed) { flushed = true; DwmFlush(); }
			else Sleep(10);
			continue;
		}

		if (isLost(hr)) {
			if (++rebuilds > kMaxRebuilds) return false;
			trace(L"duplication lost, hr=" + std::to_wstring(static_cast<long>(hr)));
			resetSession(s);
			if (!createSession(s, adapter, output, monitor, info)) return false;
			continue;
		}
		if (FAILED(hr)) {
			resetSession(s);
			trace(L"acquire failed, hr=" + std::to_wstring(static_cast<long>(hr)));
			return false;
		}

		bool copied = false;
		if (res) {
			ComPtr<ID3D11Texture2D> tex;
			if (SUCCEEDED(res.As(&tex)) && tex) {
				D3D11_TEXTURE2D_DESC td{};
				tex->GetDesc(&td);
				if (ensureStaging(s, td)) {
					s.context->CopyResource(s.staging.Get(), tex.Get());
					copied = true;
				}
			}
		}
		s.dupl->ReleaseFrame();

		if (!copied) {
			if (++rebuilds > kMaxRebuilds) return false;
			resetSession(s);
			if (!createSession(s, adapter, output, monitor, info)) return false;
			continue;
		}

		bool blank = false;
		if (!readStaging(s, dst, dstW, dstH, dstX, dstY, blank)) return false;

		if (blank) {
			if (s.haveFrame) {
				// 热会话突然全黑：复制表面多半已经死了（休眠恢复后常见），重建
				if (++rebuilds > kMaxRebuilds) return false;
				trace(L"warm session went black, rebuilding");
				resetSession(s);
				if (!createSession(s, adapter, output, monitor, info)) return false;
				continue;
			}
			if (++blanks < kMaxBlankFrames) continue;
			trace(L"accepting black frame after retries");
		}

		s.haveFrame = true;
		return true;
	}
}

// ---------------------------------------------------------------- 会话缓存
std::vector<std::pair<SessionKey, std::shared_ptr<DxgiSession>>>& sessionStore()
{
	static auto* store = new std::vector<std::pair<SessionKey, std::shared_ptr<DxgiSession>>>;
	return *store;
}

std::mutex& sessionMutex()
{
	static auto* mutex = new std::mutex;
	return *mutex;
}

std::atomic<bool>& purgePending()
{
	static auto* pending = new std::atomic<bool>(false);
	return *pending;
}

std::shared_ptr<DxgiSession> acquireSession(const SessionKey& key)
{
	std::lock_guard<std::mutex> lock(sessionMutex());
	for (auto& entry : sessionStore()) {
		if (entry.first == key) {
			entry.second->lastUse = GetTickCount64();
			return entry.second;
		}
	}
	auto session = std::make_shared<DxgiSession>();
	session->lastUse = GetTickCount64();
	sessionStore().emplace_back(key, session);
	return session;
}

void purgeIdleSessions(ULONGLONG now)
{
	std::vector<std::shared_ptr<DxgiSession>> dying;
	{
		std::lock_guard<std::mutex> lock(sessionMutex());
		auto& store = sessionStore();
		for (auto it = store.begin(); it != store.end();) {
			if (now > it->second->lastUse.load() + kSessionIdleMs) {
				dying.push_back(std::move(it->second));
				it = store.erase(it);
			}
			else {
				++it;
			}
		}
	}
	// 抓屏工具是常驻进程，闲置后必须把设备/复制/暂存（4K 下几十 MB）还给系统
	for (auto& session : dying) {
		std::lock_guard<std::mutex> lock(session->lock);
		resetSession(*session);
	}
}

bool sessionStoreEmpty()
{
	std::lock_guard<std::mutex> lock(sessionMutex());
	return sessionStore().empty();
}

// 抓屏结束后起一个后台清理线程：会话闲置够久就释放，全部释放完自己退出。
// 不能只在下次抓屏时才清理 —— 那意味着用户截完图后几十 MB 会一直挂着。
void scheduleIdlePurge()
{
	std::atomic<bool>& pending = purgePending();
	if (pending.exchange(true)) return;
	std::thread([] {
		for (;;) {
			Sleep(1000);
			purgeIdleSessions(GetTickCount64());
			if (sessionStoreEmpty()) {
				purgePending() = false;
				return;
			}
		}
	}).detach();
}

bool findOutput(IDXGIFactory1* factory, const RECT& monitor,
	ComPtr<IDXGIAdapter1>& adapter, ComPtr<IDXGIOutput>& output, UINT& outputIndex)
{
	UINT index = 0;
	for (UINT a = 0;; ++a) {
		ComPtr<IDXGIAdapter1> candidate;
		if (factory->EnumAdapters1(a, &candidate) == DXGI_ERROR_NOT_FOUND || !candidate) break;
		for (UINT o = 0;; ++o) {
			ComPtr<IDXGIOutput> out;
			if (candidate->EnumOutputs(o, &out) == DXGI_ERROR_NOT_FOUND || !out) break;
			DXGI_OUTPUT_DESC desc{};
			if (SUCCEEDED(out->GetDesc(&desc)) && sameRect(desc.DesktopCoordinates, monitor)) {
				adapter = candidate;
				output = out;
				outputIndex = index;
				return true;
			}
			++index;
		}
	}
	return false;
}

} // namespace

std::vector<BYTE> ScreenCapture::capture(int x, int y, int w, int h)
{
	if (w <= 0 || h <= 0) return {};

	std::vector<MonitorEntry> mons;
	EnumDisplayMonitors(nullptr, nullptr, collectMonitor, reinterpret_cast<LPARAM>(&mons));
	if (mons.empty()) return {};

	const auto displays = queryDisplays();

	bool anyAdvanced = false;
	for (const auto& mon : mons) {
		if (findDisplay(displays, mon.device).advanced) {
			anyAdvanced = true;
			break;
		}
	}
	// 纯 SDR 屏交给调用方的 GDI：更快，也不需要色调映射
	if (!anyAdvanced) return {};

	purgeIdleSessions(GetTickCount64());

	ComPtr<IDXGIFactory1> factory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory) return {};

	std::vector<BYTE> dst((size_t)w * h * 4, 0);
	for (size_t i = 3; i < dst.size(); i += 4) dst[i] = 0xFF;

	for (const auto& mon : mons) {
		const int l = (std::max)((int)mon.rect.left, x);
		const int t = (std::max)((int)mon.rect.top, y);
		const int r = (std::min)((int)mon.rect.right, x + w);
		const int b = (std::min)((int)mon.rect.bottom, y + h);
		if (r <= l || b <= t) continue;

		ComPtr<IDXGIAdapter1> adapter;
		ComPtr<IDXGIOutput> output;
		UINT outputIndex = 0;
		if (!findOutput(factory.Get(), mon.rect, adapter, output, outputIndex)) {
			trace(L"output not found for monitor " + mon.device);
			return {};
		}

		DXGI_ADAPTER_DESC1 desc{};
		if (FAILED(adapter->GetDesc1(&desc))) return {};

		const auto session = acquireSession({ desc.AdapterLuid, outputIndex });
		const DisplayInfo info = findDisplay(displays, mon.device);

		// 任一块失败就整张回退：同一张截图不能混两种色彩管线（会出现色块/接缝）
		if (!grabMonitor(*session, adapter.Get(), output.Get(), mon.rect, info,
			dst, w, h, (int)mon.rect.left - x, (int)mon.rect.top - y)) {
			trace(L"grab failed on " + mon.device + L", advanced=" + std::to_wstring(info.advanced ? 1 : 0)
				+ L" white=" + std::to_wstring((int)info.sdrWhiteNits));
			return {};
		}
	}

	scheduleIdlePurge();
	return dst;
}

std::vector<BYTE> ScreenCapture::captureOrGdi(int x, int y, int w, int h)
{
	auto data = capture(x, y, w, h);
	if (data.empty()) data = Util::captureScreen(x, y, w, h);
	return data;
}
