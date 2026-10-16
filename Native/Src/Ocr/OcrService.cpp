#include "pch.h"
#include "OcrService.h"
#include "SystemOcr.h"
#include "LwPpOcrEngine.h"
#include "CloudOcr.h"
#include "../Setting.h"

OcrService& OcrService::instance()
{
	static OcrService inst;
	return inst;
}

std::wstring OcrService::engineDisplayName(OcrEngineKind kind)
{
	switch (kind) {
	case OcrEngineKind::PpOcrV6: return L"PP-OCRv6";
	case OcrEngineKind::System: return L"系统 OCR";
	case OcrEngineKind::Cloud: return L"云识别";
	}
	return L"OCR";
}

// 选中的引擎失败时的兜底链：
//   PP-OCRv6 ⇄ 系统 OCR（互为兜底），云识别失败也回落到 PP-OCRv6。
// 旧配置里残留的 1/2/3（v5 / v4 / Tesseract，均已下架）与任何非法值都落进 default，
// 走 PP-OCRv6，不会出现"选了但什么都没跑"。
OcrResult OcrService::recognize(int w, int h, const BYTE* bgra)
{
	OcrResult r;
	const int pref = Setting::get()->getOcrEngine();

	auto tryV6 = [&]() -> bool {
		OcrResult got = LwPpOcrEngine::recognize(w, h, bgra);
		if (!got.ok) {
			r.error = got.error.empty() ? L"PP-OCRv6 识别失败" : got.error;
			return false;
		}
		r = got;
		return true;
	};
	auto trySystem = [&]() -> bool {
		OcrResult got = SystemOcr::recognize(w, h, bgra);
		if (!got.ok) {
			r.error = got.error.empty() ? L"系统 OCR 识别失败" : got.error;
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

	switch (static_cast<OcrEngineKind>(pref)) {
	case OcrEngineKind::System:
		if (!trySystem()) { r.fellBack = true; tryV6(); }
		break;
	case OcrEngineKind::Cloud:
		if (!tryCloud()) { r.fellBack = true; tryV6(); }
		break;
	case OcrEngineKind::PpOcrV6:
	default:
		if (!tryV6()) { r.fellBack = true; trySystem(); }
		break;
	}
	return r;
}
