#include "pch.h"
#include "OcrService.h"
#include "SystemOcr.h"
#include "TesseractOcr.h"
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
	case OcrEngineKind::Tesseract: return L"Tesseract";
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

	auto tryTess = [&](bool asFallback) -> bool {
		OcrResult got = TesseractOcr::recognize(w, h, bgra);
		if (!got.ok) {
			if (!asFallback || r.error.empty())
				r.error = got.error.empty() ? L"Tesseract 识别失败" : got.error;
			return false;
		}
		r = got;
		r.fellBack = asFallback;
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

	switch (preferred) {
	case OcrEngineKind::Tesseract:
		tryTess(false);
		break;
	case OcrEngineKind::System:
		if (!trySystem()) tryTess(true);
		break;
	case OcrEngineKind::PpOcrV6:
		if (!tryPp(OcrPackVariant::Official)) tryTess(true);
		break;
	case OcrEngineKind::PpOcrV5:
		if (!tryPp(OcrPackVariant::Embedded)) tryTess(true);
		break;
	case OcrEngineKind::PpOcrV4:
		if (!tryPp(OcrPackVariant::StableV4)) tryTess(true);
		break;
	case OcrEngineKind::Cloud:
		if (!tryCloud()) tryTess(true);
		break;
	}
	return r;
}
