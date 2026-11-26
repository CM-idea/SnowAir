#pragma once
#include <string>

// 跨版本单实例 + 全局热键归属。
//
// 不用 Ling 自带的 refuseSecondInstance：它的身份是编译期随机串，每次编译都变，
// 不同版本互相找不到，会同时跑起来 —— 热键注册两遍、配置互相覆盖。
//
// 身份按代号区分：完整版 SnowAir、Lite 版 SnowAir.Lite（看文件名里有没有 lite）。
// 代号决定互斥体、实例窗口标题和 AUMID，所以两个代号可以并存。
// 同一代号内只留一个实例：版本新的留，版本相同则先启动的留。
// 全局热键只归完整版：Lite 启动时发现完整版在跑就不注册，完整版启动时通知 Lite 让出，
// 完整版退出时通知它取回。
//
// --auto-quit=true 不参与：脚本并发截图是允许的。
struct InstanceDecision
{
	bool continueLaunch{ true };  // false = 本进程应立刻退出（已有同类实例接管）
	bool ownHotkeys{ true };      // false = 热键归完整版，本进程不注册
};

InstanceDecision enforceSingleInstance();
