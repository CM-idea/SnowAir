#pragma once
#include <include/Ling.h>
#include <string>

// 自动升级。
// 时机：应用回到"只剩托盘图标待命"的状态时（App::trimMemoryLater 里调），才去访问 GitHub —— 
// 启动时就请求会跟截图抢资源，用户也可能是拿快捷键抓一张就走的。
// 每天最多查一次（Setting::getUpdateCheckDay 记账），查到的新版本号缓存在内存里，
// 「关于」页打开时直接显示「发现新版本 x.y.z」，点检查更新会跳到 releases 页。
class Update
{
public:
	// 每次回到空闲状态都会调，真正干活的条件见上。多调无害
	static void checkLater();
	// 每日检查发现的新版本号（如 "0.1.12"）；空 = 没有新版 / 今天还没查
	static std::wstring pendingVersion();

	// 晾衣架插件更新：与主程序检查同时跑。force=true 供「关于」页手动检查重查。
	static void checkRackUpdate(bool force = false);
	// 已查到的最新晾衣架版本（如 "0.1.1"）；空 = 尚未查到
	static std::wstring rackLatestVersion();
	// 最新版本 > 已安装插件版本时返回 true（未安装返回 false）
	static bool rackUpdateAvailable();
	// 晾衣架更新状态变化（查到最新版本后触发，UI 线程）
	static winrt::event<winrt::delegate<>>& onRackUpdateChanged();
};
