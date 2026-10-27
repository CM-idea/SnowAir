#pragma once

// 无窗口自检：`SnowAir.exe --selftest` 时运行，只覆盖不依赖 D2D/窗口的纯逻辑。
// 返回值 = 失败项数（0 表示全部通过），日志写到 exe 同目录 SnowAir-selftest.log。
int RunSelfTest();
