#pragma once
#include <string>
#include <vector>

enum class OcrEngineKind {
	PpOcrV6 = 0,
	PpOcrV5 = 1,
	PpOcrV4 = 2,
	Tesseract = 3,
	System = 4,
	Cloud = 5,
};

struct OcrRegion {
	std::wstring text;
	std::wstring displayText;
	float x{ 0 }, y{ 0 }, w{ 0 }, h{ 0 }; // 相对识别图左上角
};

struct OcrResult {
	bool ok{ false };
	std::wstring text;
	OcrEngineKind engineUsed{ OcrEngineKind::System };
	std::wstring error;
	bool fellBack{ false };
	std::vector<OcrRegion> regions;
};
