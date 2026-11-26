#pragma once
#include <thread>
#include <vector>
#include <memory>
#include <include/Ling.h>

// 录制参数只在 CapVideo.cpp 里构造，头文件用前向声明就够 ——
// VideoMp4.hpp 会拉进整套 MediaFoundation 头，不该让 ToolVideo / WinCap 这些
// 只是引用 CapVideo 的编译单元都跟着吃一遍。
namespace VideoMp4 { struct DESKTOPCAPTUREPARAMS; }
namespace VideoGif { struct GifParam; }
namespace LensScene { class Tap; }

class WinCap;
class ToolVideo;
// 屏幕录制。区域已经由 WinCap 定好了，所以它不是窗口，只管工具条和编码线程。
class CapVideo
{
public:
	CapVideo(WinCap* win);
	~CapVideo();
	// ToolCap 原地换成 ToolVideo
	void makeTool();
	// ToolVideo 的摆放规则（就是 WinCap 那套通用规则）。建窗口时走一遍，
	// 工具条或宿主的 DPI 变了之后回头再走一遍
	void layoutTool();
	// 宿主窗口要销毁了：停掉录制、收掉工具条
	void dispose();
	// 放弃本次录制（ESC / 关闭）：停止编码并删掉临时文件，不落盘
	void discard();
	bool isRecording() const;
	bool isPaused() const { return paused; }
	void setPaused(bool on);
	// 「鼠标单击效果 → 光标放大」：把当前倍率交给采集线程，让它把真实光标画大。
	// 1 = 原始大小；只在录制中有效，没有录制时是空操作。
	void setCursorZoom(float scale);
	// 放大动画期间采集侧不画光标，改由主覆盖层画放大的实时光标（截图/视频都只有一份）
	void setCursorHidden(bool hidden);
	// 录制中临时开关「局部放大」（演示画布临时触发 / 设置里切换效果）
	void setLocalMagnifier(bool on);
	void setLocalMagnifierZoom(float zoom);
	// 单击层独立排除捕获后，把同一批动画状态同步给采集线程，在放大镜之后合成
	void syncClickOverlay();
	void startMp4(bool useSpeaker, bool useMic);
	void startGif();
	// 录制中选区被挪动：把「相对开录时的平移量」交给采集线程，让录出来的画面跟着选区走
	void syncCaptureMove();
	// 录屏期间「选区能跨过去的那几块屏」（虚拟桌面坐标）：只有与 targetDesk 所在屏挂在**同一个
	// DXGI 适配器**上的屏，采集侧才换得过去（见 VideoMp4 的换屏跟随）。
	static std::vector<RECT> reachableMonitors(const RECT& targetDesk);
	// 本机有没有能输出 H.265 的编码器 MFT（只查注册列表，不建实例，结果缓存一次）。
	// 「编码模式」选「仅 H.265」而这里为 false 时录制必然失败：设置页据此标注该项，
	// 采集侧据此给出明确原因，而不是笼统的「录屏失败」。
	static bool hasHevcEncoder();
	// 选区能不能被挪到这块屏上继续录：同适配器、且采集侧没在它上面换失败过。
	// 动图那条走 GDI 抓屏，覆盖整块虚拟桌面，不受显卡分组限制。
	bool canSwitchToMonitor(const RECT& mon) const;
	// 停止录制并返回录好的临时文件路径；没在录制时返回空串。
	// hideHost=false：演示画布内录屏用 —— 停止后不能把画布窗口一起隐藏
	std::wstring stop(bool hideHost = true, bool userAbort = false);
	// Ctrl+S / Ctrl+C 转给工具条上的"存文件" / "存剪切板"；没在录制时返回 false
	bool onSaveKey(bool toClipboard);
	void setToolVisible(bool visible);
	void notifyHistoryChanged();
	void cancelAnnotSelect();
	void syncPierceBtn(bool on);
	HWND toolHwnd() const;
	bool queryBarAnchor(float& x, float& y, float& w, float& h, float& btnCenterX, float& dpi) const;
	ToolVideo* toolBar() const { return tool.get(); }
private:
	WinCap* win;
	std::unique_ptr<ToolVideo> tool;
	std::unique_ptr<VideoMp4::DESKTOPCAPTUREPARAMS> mp4Param;
	std::unique_ptr<VideoGif::GifParam> gifParam;
	// 开录那一刻选区的原点（虚拟桌面坐标）：录制中选区的位移都相对它算
	int capBaseX{ 0 }, capBaseY{ 0 };
	// 开录时算好的「能跨过去的那几块屏」（同显卡适配器）：选区跨屏时现取，避免每次移动都查 DXGI
	std::vector<RECT> reachable_;
	// 屏幕上圆形镜头预览的取景源：开录时交给采集线程写入，停止后置为不活跃。
	std::shared_ptr<LensScene::Tap> lensTap_;
	std::jthread captureThread;
	bool paused{ false };
};
