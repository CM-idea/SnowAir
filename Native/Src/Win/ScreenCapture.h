#pragma once
#include <vector>
#include <Windows.h>

// 屏幕抓取（HDR 感知）。
// 优先走 DXGI 桌面复制：HDR 屏向系统要原始 FP16(scRGB)，在软件里做 HDR→SDR 色调映射；
// 普通屏仍要 BGRA8。两者最终都输出 8 位 BGRA，剪贴板/PNG/历史记录都不用改。
// 任一路失败（旧系统、被其他进程占用、受保护内容…）返回空，由调用方回退 GDI。
namespace ScreenCapture
{
	// 抓取虚拟桌面矩形（物理像素、屏幕坐标）。返回 top-down BGRA，alpha = 255。
	// 只做 DXGI 这一路，失败返回空 vector。
	std::vector<BYTE> capture(int x, int y, int w, int h);

	// 截图入口用：先尝试 DXGI（HDR 友好），失败再退回原来的 GDI 抓屏。
	std::vector<BYTE> captureOrGdi(int x, int y, int w, int h);
}
