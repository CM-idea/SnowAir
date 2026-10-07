#pragma once
#include <include/Ling.h>
#include <vector>

class App
{
	public:
		~App();
		static void init();
		// 消息循环退出后、Ling::dispose 之前调用：单例和窗口对象里存着 WinRT / D2D 对象
		//（配置和语言是 JsonObject，贴图窗口攥着位图和画刷），等到进程退出后的静态析构时
		// CoUninitialize 早跑完了，析构里那句 Release 打到的是已经拆掉的对象 ——
		// 表现为读取访问权限冲突
		static void dispose();
		static App* get();
		void takeScreenShot(int x, int y, int w, int h,ID2D1Bitmap1** img);
		// 由 BGRA 缓冲（top-down，w*4*h）在 **UI 线程**建 D2D 位图。
		// 抓屏的 GDI 部分（BitBlt+GetDIBits，整屏分辨率下要几十~几百毫秒）可放到工作线程做，
		// 只有这一步的 CreateBitmap 必须在 UI 线程 —— 目的是别让 UI 线程在热键回调里长时间阻塞
		//（否则系统会短暂显示"忙"光标）。
		static void createBitmapFromBGRA(int w, int h, const std::vector<BYTE>& data, ID2D1Bitmap1** img);
		std::tuple<int, int, int, int> getScreenArea();
		// 把窗口从屏幕捕获里摘出去：它在屏幕上照常显示、照常能点，但录屏和抓屏都拿不到它。
		// 录屏工具条压在录制区内部时（全屏录制必然如此）靠这个才不会被录进去。
		// MP4 走的 DXGI 桌面复制和 GIF 走的 BitBlt 抓屏都认这个标记（后者实测验证过）。
		// 需要 Windows 10 2004（build 19041）以上；更老的系统由实现自己拦掉，退回"照旧被录进去"—— 别指望调用会失败，它在老系统上照样返回成功，见实现里的注释。
		// 只适合小窗口：铺满整屏的窗口一旦被涂黑（老系统）就是整段录像全黑，
		// 那种窗口该做的是"别在录制区里画东西"，而不是靠这个标记
		static void excludeFromCapture(HWND hwnd);
		// 取消 excludeFromCapture（长截图结束后恢复）
		static void clearCaptureExclusion(HWND hwnd);
		// 静默整屏截图（托盘「截取全屏」）：不建任何窗口，抓整个虚拟桌面 →
		// 按「全屏截图文件名格式」存到 图片\SnowAir → 复制到剪贴板（勾了"以文件形式复制"就复制文件）
		// → 记一条截图历史。全程无 UI，失败时什么也不做。
		void captureFullScreenSilent();
	private:
		App();
};

