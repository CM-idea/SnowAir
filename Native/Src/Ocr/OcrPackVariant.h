#pragma once
#include <string>

enum class OcrPackVariant {
	Official = 0,   // PP-OCRv6 (python / optional)
	Embedded = 1,   // PP-OCRv5 RapidOCR.dll
	StableV4 = 2,   // PP-OCRv4 RapidOCR.dll
};

namespace OcrPack {
	inline OcrPackVariant clampVariant(int v)
	{
		if (v < 0) return OcrPackVariant::Official;
		if (v > 2) return OcrPackVariant::StableV4;
		return static_cast<OcrPackVariant>(v);
	}
	inline OcrPackVariant fromEngineId(int e)
	{
		if (e == 0) return OcrPackVariant::Official;
		if (e == 1) return OcrPackVariant::Embedded;
		if (e == 2) return OcrPackVariant::StableV4;
		return OcrPackVariant::Embedded;
	}
	inline const wchar_t* label(OcrPackVariant v)
	{
		switch (v) {
		case OcrPackVariant::Official: return L"PP-OCRv6";
		case OcrPackVariant::Embedded: return L"PP-OCRv5";
		case OcrPackVariant::StableV4: return L"PP-OCRv4";
		}
		return L"PP-OCR";
	}
	inline const wchar_t* folderName(OcrPackVariant v)
	{
		switch (v) {
		case OcrPackVariant::Official: return L"ppocr-v6";
		case OcrPackVariant::Embedded: return L"ppocr-v5";
		case OcrPackVariant::StableV4: return L"ppocr-v4";
		}
		return L"ppocr";
	}
}
