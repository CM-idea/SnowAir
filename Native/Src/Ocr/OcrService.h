#pragma once
#include "OcrTypes.h"

class OcrService {
public:
	static OcrService& instance();
	/** 按 Setting::ocrEngine 识别；首选失败时兜底「已安装的 PP 离线模型」 */
	OcrResult recognize(int w, int h, const BYTE* bgra);
	static std::wstring engineDisplayName(OcrEngineKind kind);
private:
	OcrService() = default;
};
