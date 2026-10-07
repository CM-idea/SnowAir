#pragma once
#include "OcrTypes.h"

namespace TesseractOcr {
	OcrResult recognize(int w, int h, const BYTE* bgra);
}
