#include "pch.h"
#include "ScreenCapture.h"
#include "../Util.h"
#include <d3d11.h>
#include <dxgi1_5.h>
#include <dwmapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace {

	BOOL CALLBACK collectMonitor(HMONITOR mon, HDC, LPRECT, LPARAM lp)
	{
		MONITORINFO mi{ sizeof(MONITORINFO) };
		if (GetMonitorInfoW(mon, &mi))
			reinterpret_cast<std::vector<RECT>*>(lp)->push_back(mi.rcMonitor);
		return TRUE;
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

	// 当前是否有一块"高级颜色"（HDR / 广色域）显示处于开启状态。
	// 只有这种屏才值得走 DXGI：SDR 屏继续用原来验证过的 GDI，避免不必要的回归。
	bool hasAdvancedColorDisplay()
	{
		UINT32 pathCount = 0, modeCount = 0;
		if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
			return false;
		std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
		std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
		if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS)
			return false;
		for (const auto& p : paths) {
			DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO info{};
			info.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
			info.header.size = sizeof(info);
			info.header.adapterId = p.targetInfo.adapterId;
			info.header.id = p.targetInfo.id;
			if (DisplayConfigGetDeviceInfo(&info.header) == ERROR_SUCCESS && info.advancedColorEnabled)
				return true;
		}
		return false;
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
	bool grabMonitor(AdapterDevice* dev, const RECT& mon, std::vector<BYTE>& dst,
		int dstW, int dstH, int dstX, int dstY)
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

		// DuplicateOutput1（Win10 1703+）：直接向系统要 BGRA8。
		// HDR 屏默认给 FP16(scRGB)，这里由系统完成 HDR→SDR 的色调映射，
		// 所以 HDR / SDR 屏拿到的是同一种像素，后续保存、剪贴板、历史都不用改。
		ComPtr<IDXGIOutput5> out5;
		if (FAILED(output.As(&out5)) || !out5) return false;
		const DXGI_FORMAT want[] = { DXGI_FORMAT_B8G8R8A8_UNORM };
		ComPtr<IDXGIOutputDuplication> dupl;
		if (FAILED(out5->DuplicateOutput1(dev->device.Get(), 0, 1, want, &dupl)) || !dupl)
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
				// 超时说明桌面暂时没有新帧；被占用 / 访问丢失这类错误重试也没用
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
								const BYTE* src = base + (size_t)y * m.RowPitch + (size_t)sx * 4;
								BYTE* dp = dst.data()
									+ ((size_t)(dstY + y) * dstW + (dstX + sx)) * 4;
								memcpy(dp, src, (size_t)(ex - sx) * 4);
								// 桌面复制的 alpha 不保证是 255，统一成不透明
								for (int x = 0; x < ex - sx; ++x) dp[x * 4 + 3] = 0xFF;
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
	// 只在开了 HDR / 广色域的机器上走 DXGI（这才是 GDI 抓不好的场景）。
	// SDR 屏继续用原来的 GDI：行为与旧版本完全一致，避免为了 HDR 把普通截图带出回归。
	if (!hasAdvancedColorDisplay()) return {};

	std::vector<RECT> mons;
	EnumDisplayMonitors(nullptr, nullptr, collectMonitor, reinterpret_cast<LPARAM>(&mons));
	if (mons.empty()) return {};

	std::vector<BYTE> dst((size_t)w * h * 4, 0);
	for (size_t i = 3; i < dst.size(); i += 4) dst[i] = 0xFF;

	std::vector<AdapterDevice> cache;
	bool any = false;
	for (const auto& mon : mons) {
		const int l = (std::max)((int)mon.left, x);
		const int t = (std::max)((int)mon.top, y);
		const int r = (std::min)((int)mon.right, x + w);
		const int b = (std::min)((int)mon.bottom, y + h);
		if (r <= l || b <= t) continue;

		auto* dev = deviceForMonitor(mon, cache);
		// 任一块失败就整张回退：同一张截图不能混两种色彩管线（会出现色块/接缝）
		if (!dev || !grabMonitor(dev, mon, dst, w, h, (int)mon.left - x, (int)mon.top - y))
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
