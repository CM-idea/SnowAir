#pragma once
#include "OcrTypes.h"
#include "OcrPackVariant.h"

namespace PpOcrEngine {
	bool isInstalled(OcrPackVariant v);
	OcrResult recognize(int w, int h, const BYTE* bgra, OcrPackVariant v);
}
