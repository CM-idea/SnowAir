#pragma once
#include "OcrTypes.h"

class OcrService {
public:
	static OcrService& instance();
	/** 按 Setting::ocrEngine 识别；PP/System/Cloud 失败兜底 Tesseract */
	OcrResult recognize(int w, int h, const BYTE* bgra);
	static std::wstring engineDisplayName(OcrEngineKind kind);
private:
	OcrService() = default;
};
