#include "pch.h"
#include "OcrService.h"
#include "SystemOcr.h"
#include "PpOcrEngine.h"
#include "CloudOcr.h"
#include "OcrPackVariant.h"
#include "../Setting.h"
#include <algorithm>

OcrService& OcrService::instance()
{
	static OcrService inst;
	return inst;
}

std::wstring OcrService::engineDisplayName(OcrEngineKind kind)
{
	switch (kind) {
	case OcrEngineKind::PpOcrV6: return L"PP-OCRv6";
	case OcrEngineKind::PpOcrV5: return L"PP-OCRv5";
	case OcrEngineKind::PpOcrV4: return L"PP-OCRv4";
	case OcrEngineKind::System: return L"系统 OCR";
	case OcrEngineKind::Cloud: return L"云识别";
	}
	return L"OCR";
}

OcrResult OcrService::recognize(int w, int h, const BYTE* bgra)
{
	OcrResult r;
	const int pref = Setting::get()->getOcrEngine();
	const auto preferred = static_cast<OcrEngineKind>(std::clamp(pref, 0, 5));

	auto trySystem = [&]() -> bool {
		OcrResult got = SystemOcr::recognize(w, h, bgra);
		if (!got.ok) {
			r.error = got.error.empty() ? L"系统 OCR 识别失败" : got.error;
			return false;
		}
		r = got;
		return true;
	};
	auto tryPp = [&](OcrPackVariant v) -> bool {
		OcrResult got = PpOcrEngine::recognize(w, h, bgra, v);
		if (!got.ok) {
			r.error = got.error.empty() ? L"PP-OCR 识别失败" : got.error;
			return false;
		}
		r = got;
		return true;
	};
	auto tryCloud = [&]() -> bool {
		OcrCloudConfig cfg = Setting::get()->getOcrCloudConfig();
		OcrResult got = CloudOcr::recognize(w, h, bgra, cfg);
		if (!got.ok) {
			r.error = got.error.empty() ? L"云识别失败" : got.error;
			return false;
		}
		r = got;
		return true;
	};

	// 兜底：已安装的 PP 离线模型（v6 → v5 → v4）。
	auto tryInstalledPp = [&]() -> bool {
		const OcrPackVariant order[] = {
			OcrPackVariant::Official, OcrPackVariant::Embedded, OcrPackVariant::StableV4 };
		for (auto v : order) {
			if (!PpOcrEngine::isInstalled(v)) continue;
			r.fellBack = true;
			return tryPp(v);
		}
		r.error = L"未安装可用的文字识别模型，请在「插件集成」下载 PP-OCRv5";
		return false;
	};

	switch (preferred) {
	case OcrEngineKind::System:
		if (!trySystem()) tryInstalledPp();
		break;
	case OcrEngineKind::PpOcrV6:
		if (!tryPp(OcrPackVariant::Official)) tryInstalledPp();
		break;
	case OcrEngineKind::PpOcrV5:
		if (!tryPp(OcrPackVariant::Embedded)) tryInstalledPp();
		break;
	case OcrEngineKind::PpOcrV4:
		if (!tryPp(OcrPackVariant::StableV4)) tryInstalledPp();
		break;
	case OcrEngineKind::Cloud:
		if (!tryCloud()) tryInstalledPp();
		break;
	default:
		// 旧配置里可能残留已移除引擎的数值：兜底到已安装的 PP 离线模型
		tryInstalledPp();
		break;
	}
	return r;
}
