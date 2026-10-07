#pragma once
#include "OcrTypes.h"

namespace SystemOcr {
	bool isAvailable();
	// BGRA top-down row-tight
	OcrResult recognize(int w, int h, const BYTE* bgra);
}
