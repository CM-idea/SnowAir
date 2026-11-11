#pragma once
/**
 * 放大镜（取色器）外壳的落位计算 —— 纯几何、不碰窗口 API。
 *
 * 为什么要单独拎出来：外壳位置只在真截图会话里才看得见，出了问题很难覆盖到；
 * 这里做成纯函数后，--selftest 可以直接把四个角、四条边、多屏边界都测一遍。
 *
 * 规则（只避让，不记忆）：
 *   · 默认贴在光标**右下角**，与准星留 gap 间距，永不压住取样点；
 *   · 右下放不下（贴到屏幕/显示器边缘）时，依次试 左下 → 右上 → 左上；
 *   · 一旦右下重新放得下，立刻回到右下（不做「记住上次那一侧」，避免光标离开边缘后
 *     放大镜还赖在左上/左下）。
 */

#include <algorithm>

struct PixLensBox
{
    float x{ 0.f };   // 外壳左上角（客户区坐标）
    float y{ 0.f };
};

// 返回选中的象限（0=右下 1=左下 2=右上 3=左上）；四个都放不下（area 比外壳还小）时
// 返回 -1，此时 out 按「右下 + 夹进 area」兜底，保证外壳至少不会跑出 area。
inline int pixLensQuadrant(float cursorX, float cursorY,
    float shellW, float shellH, float gap,
    float areaLeft, float areaTop, float areaRight, float areaBottom,
    PixLensBox* out)
{
    const float xs[4] = {
        cursorX + gap,                // 0 右下
        cursorX - gap - shellW,       // 1 左下
        cursorX + gap,                // 2 右上
        cursorX - gap - shellW        // 3 左上
    };
    const float ys[4] = {
        cursorY + gap,                // 0 右下
        cursorY + gap,                // 1 左下
        cursorY - gap - shellH,       // 2 右上
        cursorY - gap - shellH        // 3 左上
    };

    auto fits = [&](int q) {
        return xs[q] >= areaLeft && ys[q] >= areaTop
            && xs[q] + shellW <= areaRight && ys[q] + shellH <= areaBottom;
    };

    int pick = -1;
    for (int q = 0; q < 4 && pick < 0; ++q)
        if (fits(q)) pick = q;

    PixLensBox box{};
    if (pick >= 0) {
        box.x = xs[pick];
        box.y = ys[pick];
    }
    else {
        // 极窄/极矮的显示器：没有完整解，退回右下并夹进 area（宁可贴边也不出屏）
        box.x = std::clamp(xs[0], areaLeft, std::max(areaLeft, areaRight - shellW));
        box.y = std::clamp(ys[0], areaTop, std::max(areaTop, areaBottom - shellH));
    }
    if (out) *out = box;
    return pick;
}
