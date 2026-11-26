#pragma once
#include <vector>
#include <Windows.h>
#include <dxgi1_2.h>

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

	// 丢掉所有缓存会话里“上一帧”的暂存结果，强制下一次 capture 等一帧新画面。
	// 用在遮罩刚挖洞 / 刚改变可见性之后：此时桌面复制可能还没有新帧，
	// 不回退到旧暂存帧才能保证抓到的是“变化后”的画面。
	void dropCachedFrames();
}

// —— 桌面复制的旋转（截图与录屏共用同一份实现） ——
// 经验事实（MS 文档「Desktop Duplication API · Rotating the desktop image」）：
//   AcquireNextFrame 给的表面永远是**未旋转**的，桌面图被"转"在表面里 ——
//   桌面 768x1024 / 90° 时，表面是 1024x768，桌面方向 = 表面顺时针转 90°。
// 所以拿像素时要反过来算：桌面坐标 (dx,dy) → 表面坐标 (sx,sy)。
// 90°/270° 下桌面宽高与表面宽高互换；180°/无旋转时相同。
inline void ScreenCaptureSurfacePoint(DXGI_MODE_ROTATION rotation,
	int dx, int dy, int surfW, int surfH, int& sx, int& sy)
{
	switch (rotation) {
	case DXGI_MODE_ROTATION_ROTATE90:
		sx = dy;             sy = surfH - 1 - dx; break;
	case DXGI_MODE_ROTATION_ROTATE180:
		sx = surfW - 1 - dx; sy = surfH - 1 - dy; break;
	case DXGI_MODE_ROTATION_ROTATE270:
		sx = surfW - 1 - dy; sy = dx;             break;
	default:
		sx = dx;             sy = dy;             break;
	}
}

// 用"复制表面真实尺寸 + 桌面矩形尺寸"校验 Rotation 能不能用：
// 90°/270° 必须横竖互换、180°/无旋转必须完全相等；对不上就不旋转（不猜，至少不比不处理更差）。
// 也兼容个别直接给桌面方向图的驱动：表面尺寸已经等于桌面尺寸时不能再转。
inline DXGI_MODE_ROTATION ScreenCaptureResolveRotation(DXGI_MODE_ROTATION raw,
	int surfW, int surfH, int deskW, int deskH)
{
	DXGI_MODE_ROTATION rot = (raw == DXGI_MODE_ROTATION_UNSPECIFIED)
		? DXGI_MODE_ROTATION_IDENTITY : raw;
	if (surfW <= 0 || surfH <= 0 || deskW <= 0 || deskH <= 0)
		return DXGI_MODE_ROTATION_IDENTITY;

	const bool quarter = rot == DXGI_MODE_ROTATION_ROTATE90 || rot == DXGI_MODE_ROTATION_ROTATE270;
	if (quarter) {
		if (surfW == deskW && surfH == deskH) return DXGI_MODE_ROTATION_IDENTITY;   // 驱动已给桌面方向
		if (surfW != deskH || surfH != deskW) return DXGI_MODE_ROTATION_IDENTITY;   // 横竖都对不上
		return rot;
	}
	if (surfW != deskW || surfH != deskH) return DXGI_MODE_ROTATION_IDENTITY;
	return rot;
}
