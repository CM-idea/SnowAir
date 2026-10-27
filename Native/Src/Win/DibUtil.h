#pragma once
#include <Windows.h>
#include <vector>
#include <cstring>

// BGRA 像素（自上而下）→ 32bpp DIB，供 HICON / HCURSOR 使用（非预乘 alpha）。
// 托盘图标与截图光标都需要这一步，集中一份，避免两处各写一遍 CreateDIBSection 样板。
inline HBITMAP makeColorDib(int size, const std::vector<BYTE>& bgra)
{
	BITMAPV5HEADER bi{};
	bi.bV5Size = sizeof(bi);
	bi.bV5Width = size;
	bi.bV5Height = -size;   // 自上而下，与 WIC 行序一致
	bi.bV5Planes = 1;
	bi.bV5BitCount = 32;
	bi.bV5Compression = BI_BITFIELDS;
	bi.bV5RedMask = 0x00FF0000;
	bi.bV5GreenMask = 0x0000FF00;
	bi.bV5BlueMask = 0x000000FF;
	bi.bV5AlphaMask = 0xFF000000;
	void* bits = nullptr;
	HDC screen = GetDC(nullptr);
	HBITMAP bmp = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
	ReleaseDC(nullptr, screen);
	if (!bmp || !bits) {
		if (bmp) DeleteObject(bmp);
		return nullptr;
	}
	memcpy(bits, bgra.data(), bgra.size());
	return bmp;
}
