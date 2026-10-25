#pragma once
#include <string>
#include <vector>

enum class OcrEngineKind {
	PpOcrV6 = 0,
	// 1 原为 PP-OCRv5、2 原为 PP-OCRv4（RapidOCR.dll）、3 原为 Tesseract：都已移除。
	// 空出来不用，避免旧配置里的数值错位。
	System = 4,
	// 5 原为云识别（API 配置）：Lite 版不提供，保留数值只为兼容旧配置（运行时按 PP-OCRv6 处理）。
	Cloud = 5,
};

struct OcrRegion {
	std::wstring text;
	std::wstring displayText;
	float x{ 0 }, y{ 0 }, w{ 0 }, h{ 0 }; // 相对识别图左上角
	// 竖排（日文/中文竖写）：检测框高明显大于宽时置位。叠加层要逐字竖着摆，
	// 否则横排文字会被 54px 宽的窄框压成一条线，看起来就是空白。
	bool vertical{ false };
};

struct OcrResult {
	bool ok{ false };
	std::wstring text;
	OcrEngineKind engineUsed{ OcrEngineKind::System };
	std::wstring error;
	bool fellBack{ false };
	std::vector<OcrRegion> regions;
};
