#pragma once
#include <windows.h>
#include <vector>
#include <string>

namespace ImageExport {
    // BGRA（top-down、行紧凑）→ JPEG 字节流；成功返回 true。
    bool encodeJpeg(const std::vector<BYTE>& bgra, int w, int h, int quality, std::vector<BYTE>& out);
}
