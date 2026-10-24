#include "pch.h"
#include "ScreenCapture.h"
#include "../Util.h"
#include <d3d11.h>
#include <dxgi1_5.h>
#include <dxgi1_6.h>
#include <dwmapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace {

	struct HdrDisplay
	{
		bool advanced{ false };          // 系统处于高级颜色（广色域 / HDR）模式
		float sdrWhiteNits{ 80.f };      // SDR 白点，Windows 默认会把 SDR 内容抬到这一档
	};

	struct MonitorEntry
	{
		RECT rect{};
		std::wstring device;
	};

	// 每块屏实际生效的色调映射参数（由 DisplayConfig + DXGI 输出信息合成）
	struct HdrCapability
	{
		bool hdr{ false };
		float peakNits{ 1000.f };
		float sdrWhiteNits{ 80.f };
	};

	BOOL CALLBACK collectMonitor(HMONITOR mon, HDC, LPRECT, LPARAM lp)
	{
		MONITORINFOEXW mi{ sizeof(MONITORINFOEXW) };
		if (GetMonitorInfoW(mon, &mi))
			reinterpret_cast<std::vector<MonitorEntry>*>(lp)->push_back({ mi.rcMonitor, mi.szDevice });
		return TRUE;
	}

	std::vector<std::pair<std::wstring, HdrDisplay>> queryHdrDisplays()
	{
		std::vector<std::pair<std::wstring, HdrDisplay>> out;
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

			HdrDisplay display{};
			display.advanced = advanced;
			if (advanced) {
				DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
				white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
				white.header.size = sizeof(white);
				white.header.adapterId = paths[i].targetInfo.adapterId;
				white.header.id = paths[i].targetInfo.id;
				if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS && white.SDRWhiteLevel > 0)
					display.sdrWhiteNits = static_cast<float>(white.SDRWhiteLevel) * 80.f / 1000.f;
			}
			out.push_back({ src.viewGdiDeviceName, display });
		}
		return out;
	}

	HdrDisplay findHdrDisplay(const std::vector<std::pair<std::wstring, HdrDisplay>>& map,
		const std::wstring& device)
	{
		for (const auto& entry : map)
			if (entry.first == device) return entry.second;
		return {};
	}

	// ---- 线性光 → sRGB 编码：查表足够精确，避免每像素 powf ----
	unsigned char linearToSrgb8(float v)
	{
		static const std::vector<unsigned char> lut = [] {
			std::vector<unsigned char> table(4097);
			for (int i = 0; i <= 4096; ++i) {
				const float c = static_cast<float>(i) / 4096.f;
				const float s = c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f;
				table[i] = static_cast<unsigned char>(std::lround(std::clamp(s, 0.f, 1.f) * 255.f));
			}
			return table;
			}();
		const int idx = static_cast<int>(std::clamp(v, 0.f, 1.f) * 4096.f + 0.5f);
		return lut[std::clamp(idx, 0, 4096)];
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

	constexpr float kPqM1{ 0.159301758f };
	constexpr float kPqM2{ 78.84375f };
	constexpr float kPqC1{ 0.8359375f };
	constexpr float kPqC2{ 18.8515625f };
	constexpr float kPqC3{ 18.6875f };
	constexpr float kAbsoluteNits{ 10000.f };
	constexpr float kSdrReferenceNits{ 80.f };
	constexpr float kOutputWhiteNits{ 100.f };
	constexpr float kOutputBlackNits{ 0.1f };
	constexpr float kInputBlackNits{ 0.001f };
	constexpr float kEpsilon{ 1e-6f };
	constexpr float kTransitionStart{ 1.0f + 1e-3f };
	constexpr float kTransitionEnd{ kOutputWhiteNits / kSdrReferenceNits };   // 1.25

	float nitsToPq(float nits)
	{
		const float p = std::pow(std::max(nits, 0.f) / kAbsoluteNits, kPqM1);
		return std::pow((kPqC1 + kPqC2 * p) / (1.f + kPqC3 * p), kPqM2);
	}

	float pqToNits(float v)
	{
		const float p = std::pow(std::max(v, 0.f), 1.f / kPqM2);
		const float numerator = std::max(p - kPqC1, 0.f);
		const float denominator = std::max(kPqC2 - kPqC3 * p, kEpsilon);
		return std::pow(numerator / denominator, 1.f / kPqM1) * kAbsoluteNits;
	}

	// BT.2390 EETF：把 HDR 亮度压进 SDR 输出范围，SDR 白点以下的观感保持不变
	float mapLumaToSdr(float linearLuma, float peakNits)
	{
		const float lIn = std::max(std::max(linearLuma, 0.f) * kSdrReferenceNits, kInputBlackNits);
		const float x = nitsToPq(lIn);
		const float ow = nitsToPq(kOutputWhiteNits);
		const float ob = nitsToPq(kOutputBlackNits);
		const float iw = nitsToPq(std::max(peakNits, kOutputWhiteNits + 1e-3f));
		const float ib = std::min(nitsToPq(kInputBlackNits), ob - 1e-3f);
		const float denom = std::max(iw - ib, kEpsilon);
		const float minLum = (ob - ib) / denom;
		const float maxLum = (ow - ib) / denom;
		const float ks = 1.5f * maxLum - 0.5f;
		float y = (x - ib) / denom;
		if (y >= ks) {
			const float tb = (y - ks) / std::max(1.f - ks, kEpsilon);
			const float tb2 = tb * tb;
			const float tb3 = tb2 * tb;
			y = (2.f * tb3 - 3.f * tb2 + 1.f) * ks
				+ (tb3 - 2.f * tb2 + tb) * (1.f - ks)
				+ (-2.f * tb3 + 3.f * tb2) * maxLum;
		}
		if (y >= 0.f)
			y += minLum * std::pow(std::max(1.f - y, 0.f), 4.f);
		return std::max(pqToNits(std::clamp(y * denom + ib, ob, ow)) / kSdrReferenceNits, 0.f);
	}

	// 输入是 Windows scRGB FP16：先撤掉系统对 SDR 白点的抬升，再只压高光
	void toneMapPixel(float& r, float& g, float& b, const HdrCapability& cap)
	{
		const float invWhite = kSdrReferenceNits / std::max(cap.sdrWhiteNits, 1.f);
		r *= invWhite;
		g *= invWhite;
		b *= invWhite;

		const float srcR = r, srcG = g, srcB = b;
		const float inMax = std::max(r, std::max(g, b));
		if (inMax <= kTransitionStart) return;

		const float luma = std::max(0.2126f * r + 0.7152f * g + 0.0722f * b, 0.f);
		if (luma <= kEpsilon) { r = g = b = 0.f; return; }

		const float scale = mapLumaToSdr(luma, cap.peakNits) / luma;
		r *= scale;
		g *= scale;
		b *= scale;

		const float channelMax = std::max(r, std::max(g, b));
		if (channelMax > 1.f) {
			const float inv = 1.f / channelMax;
			r *= inv;
			g *= inv;
			b *= inv;
		}

		if (inMax < kTransitionEnd) {
			const float t = std::clamp((inMax - kTransitionStart) / (kTransitionEnd - kTransitionStart), 0.f, 1.f);
			const float w = t * t * (3.f - 2.f * t);
			r = srcR + (r - srcR) * w;
			g = srcG + (g - srcG) * w;
			b = srcB + (b - srcB) * w;
		}
	}

	struct AdapterDevice
	{
		ComPtr<IDXGIAdapter1> adapter;
		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> context;
	};

	bool monitorMatches(const RECT& a, const RECT& b)
	{
		return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
	}

	// 桌面复制是"变化流"：新建的会话在桌面没有新帧之前，AcquireNextFrame 返回的是
	// AccumulatedFrames == 0 的空帧（实测整张全黑，不是偶发现象）。让桌面自己重绘一次，
	// DWM 就会合成并推送一帧真实画面；重绘不改变画面内容，用户看不到任何变化。
	void forceDesktopFrame()
	{
		InvalidateRect(GetDesktopWindow(), nullptr, TRUE);
		UpdateWindow(GetDesktopWindow());
		DwmFlush();
	}

	// 找显示器对应的适配器；同一次抓屏里同一适配器只建一个 D3D11 设备
	AdapterDevice* deviceForMonitor(const RECT& mon, std::vector<AdapterDevice>& cache)
	{
		ComPtr<IDXGIFactory1> factory;
		if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory) return nullptr;
		for (UINT a = 0;; ++a) {
			ComPtr<IDXGIAdapter1> ad;
			if (factory->EnumAdapters1(a, &ad) == DXGI_ERROR_NOT_FOUND || !ad) break;
			bool matched = false;
			for (UINT o = 0;; ++o) {
				ComPtr<IDXGIOutput> out;
				if (ad->EnumOutputs(o, &out) == DXGI_ERROR_NOT_FOUND || !out) break;
				DXGI_OUTPUT_DESC d{};
				if (SUCCEEDED(out->GetDesc(&d)) && monitorMatches(d.DesktopCoordinates, mon)) {
					matched = true;
					break;
				}
			}
			if (!matched) continue;

			DXGI_ADAPTER_DESC1 want{};
			ad->GetDesc1(&want);
			for (auto& c : cache) {
				DXGI_ADAPTER_DESC1 have{};
				// 不能比 COM 指针：每次 EnumAdapters1 拿到的接口地址不同
				if (c.adapter && SUCCEEDED(c.adapter->GetDesc1(&have))
					&& have.AdapterLuid.LowPart == want.AdapterLuid.LowPart
					&& have.AdapterLuid.HighPart == want.AdapterLuid.HighPart)
					return &c;
			}

			cache.push_back({});
			auto& c = cache.back();
			c.adapter = ad;
			const D3D_FEATURE_LEVEL levels[] = {
				D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1 };
			const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
			HRESULT hr = D3D11CreateDevice(ad.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
				levels, _countof(levels), D3D11_SDK_VERSION, &c.device, nullptr, &c.context);
			if (FAILED(hr)) {
				// 个别老驱动不支持 11_1，退到 11_0 再试
				hr = D3D11CreateDevice(ad.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
					levels + 1, _countof(levels) - 1, D3D11_SDK_VERSION, &c.device, nullptr, &c.context);
			}
			if (FAILED(hr) || !c.device) {
				cache.pop_back();
				return nullptr;
			}
			return &c;
		}
		return nullptr;
	}

	// 抓一块显示器，贴到目标图的 (dstX, dstY)。失败返回 false。
	bool grabMonitor(AdapterDevice* dev, const RECT& mon, const HdrDisplay& display,
		std::vector<BYTE>& dst, int dstW, int dstH, int dstX, int dstY)
	{
		if (!dev || !dev->device || !dev->context) return false;
		ComPtr<IDXGIDevice> dxgiDev;
		if (FAILED(dev->device.As(&dxgiDev)) || !dxgiDev) return false;
		ComPtr<IDXGIAdapter> ad;
		if (FAILED(dxgiDev->GetParent(IID_PPV_ARGS(&ad))) || !ad) return false;

		ComPtr<IDXGIOutput> output;
		for (UINT i = 0;; ++i) {
			ComPtr<IDXGIOutput> o;
			if (ad->EnumOutputs(i, &o) == DXGI_ERROR_NOT_FOUND || !o) break;
			DXGI_OUTPUT_DESC d{};
			if (SUCCEEDED(o->GetDesc(&d)) && monitorMatches(d.DesktopCoordinates, mon)) {
				output = o;
				break;
			}
		}
		if (!output) return false;

		// 只有"高级颜色 + PQ/BT.2020 输出"才是真 HDR；广色域 SDR 仍按普通画面处理
		HdrCapability cap{};
		cap.sdrWhiteNits = display.sdrWhiteNits;
		if (ComPtr<IDXGIOutput6> out6; SUCCEEDED(output.As(&out6)) && out6) {
			DXGI_OUTPUT_DESC1 d1{};
			if (SUCCEEDED(out6->GetDesc1(&d1))) {
				cap.hdr = display.advanced
					&& (d1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
						|| d1.ColorSpace == DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020);
				if (std::isfinite(d1.MaxLuminance) && d1.MaxLuminance > 0.f)
					cap.peakNits = d1.MaxLuminance;
			}
		}

		ComPtr<IDXGIOutput5> out5;
		if (FAILED(output.As(&out5)) || !out5) return false;
		DXGI_FORMAT formats[2]{ DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM };
		UINT formatCount = 1;
		if (cap.hdr) {
			// HDR 时优先要原始 FP16，拿不到再让系统给 8 位
			formats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
			formats[1] = DXGI_FORMAT_B8G8R8A8_UNORM;
			formatCount = 2;
		}
		ComPtr<IDXGIOutputDuplication> dupl;
		if (FAILED(out5->DuplicateOutput1(dev->device.Get(), 0, formatCount, formats, &dupl)) || !dupl)
			return false;

		bool ok = false;
		bool forced = false;
		// 只接受"真正带了桌面更新"的帧：AccumulatedFrames == 0 是会话刚建立时的空帧，
		// 表面内容全黑，不是当前桌面。空帧丢弃后主动让桌面重绘一次，下一帧就是真实画面。
		for (int attempt = 0; attempt < 6 && !ok; ++attempt) {
			ComPtr<IDXGIResource> res;
			DXGI_OUTDUPL_FRAME_INFO fi{};
			HRESULT hr = dupl->AcquireNextFrame(80, &fi, &res);
			if (FAILED(hr) || !res) {
				if (hr != DXGI_ERROR_WAIT_TIMEOUT) return false;
				if (!forced) { forceDesktopFrame(); forced = true; }
				continue;
			}
			if (fi.AccumulatedFrames == 0) {
				dupl->ReleaseFrame();
				if (!forced) { forceDesktopFrame(); forced = true; }
				continue;
			}

			ComPtr<ID3D11Texture2D> tex;
			if (SUCCEEDED(res.As(&tex)) && tex) {
				D3D11_TEXTURE2D_DESC td{};
				tex->GetDesc(&td);
				const bool halfFloat = td.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
				const int bytesPerPixel = halfFloat ? 8 : 4;
				const int monW = (int)td.Width;
				const int monH = (int)td.Height;
				td.Usage = D3D11_USAGE_STAGING;
				td.BindFlags = 0;
				td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				td.MiscFlags = 0;
				td.ArraySize = 1;
				td.MipLevels = 1;
				ComPtr<ID3D11Texture2D> staging;
				if (SUCCEEDED(dev->device->CreateTexture2D(&td, nullptr, &staging)) && staging) {
					dev->context->CopyResource(staging.Get(), tex.Get());
					D3D11_MAPPED_SUBRESOURCE m{};
					if (SUCCEEDED(dev->context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)) && m.pData) {
						const BYTE* base = reinterpret_cast<const BYTE*>(m.pData);
						// 与目标图求交：选区可能只覆盖显示器一部分，也可能跨屏
						const int sx = (std::max)(0, -dstX);
						const int sy = (std::max)(0, -dstY);
						const int ex = (std::min)(monW, dstW - dstX);
						const int ey = (std::min)(monH, dstH - dstY);
						if (ex > sx && ey > sy) {
							for (int y = sy; y < ey; ++y) {
								const BYTE* src = base + (size_t)y * m.RowPitch + (size_t)sx * bytesPerPixel;
								BYTE* dp = dst.data() + ((size_t)(dstY + y) * dstW + (dstX + sx)) * 4;
								if (!halfFloat) {
									memcpy(dp, src, (size_t)(ex - sx) * 4);
								}
								else {
									const unsigned short* sp = reinterpret_cast<const unsigned short*>(src);
									for (int x = sx; x < ex; ++x, sp += 4, dp += 4) {
										float r = halfToFloat(sp[0]);
										float g = halfToFloat(sp[1]);
										float b = halfToFloat(sp[2]);
										if (cap.hdr) toneMapPixel(r, g, b, cap);
										dp[0] = linearToSrgb8(b);
										dp[1] = linearToSrgb8(g);
										dp[2] = linearToSrgb8(r);
										dp[3] = 0xFF;
									}
								}
								// 桌面复制的 alpha 不保证是 255，统一成不透明
								if (!halfFloat) {
									for (int x = 0; x < ex - sx; ++x)
										dst[((size_t)(dstY + y) * dstW + (dstX + sx + x)) * 4 + 3] = 0xFF;
								}
							}
							ok = true;
						}
						dev->context->Unmap(staging.Get(), 0);
					}
				}
			}
			dupl->ReleaseFrame();
			if (!ok) Sleep(20);
		}
		return ok;
	}
}

std::vector<BYTE> ScreenCapture::capture(int x, int y, int w, int h)
{
	if (w <= 0 || h <= 0) return {};

	const auto hdrMap = queryHdrDisplays();
	std::vector<MonitorEntry> mons;
	EnumDisplayMonitors(nullptr, nullptr, collectMonitor, reinterpret_cast<LPARAM>(&mons));
	if (mons.empty()) return {};

	bool anyAdvanced = false;
	for (const auto& mon : mons) {
		if (findHdrDisplay(hdrMap, mon.device).advanced) {
			anyAdvanced = true;
			break;
		}
	}
	if (!anyAdvanced) return {};

	std::vector<BYTE> dst((size_t)w * h * 4, 0);
	for (size_t i = 3; i < dst.size(); i += 4) dst[i] = 0xFF;

	std::vector<AdapterDevice> cache;
	bool any = false;
	for (const auto& mon : mons) {
		const int l = (std::max)((int)mon.rect.left, x);
		const int t = (std::max)((int)mon.rect.top, y);
		const int r = (std::min)((int)mon.rect.right, x + w);
		const int b = (std::min)((int)mon.rect.bottom, y + h);
		if (r <= l || b <= t) continue;

		auto* dev = deviceForMonitor(mon.rect, cache);
		const HdrDisplay display = findHdrDisplay(hdrMap, mon.device);
		// 任一块失败就整张回退：同一张截图不能混两种色彩管线（会出现色块/接缝）
		if (!dev || !grabMonitor(dev, mon.rect, display, dst, w, h,
			(int)mon.rect.left - x, (int)mon.rect.top - y))
			return {};
		any = true;
	}
	return any ? dst : std::vector<BYTE>{};
}

std::vector<BYTE> ScreenCapture::captureOrGdi(int x, int y, int w, int h)
{
	auto data = capture(x, y, w, h);
	if (data.empty()) data = Util::captureScreen(x, y, w, h);
	return data;
}
