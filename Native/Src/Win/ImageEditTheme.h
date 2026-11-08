#pragma once
#include <cstdint>
#include "SettingTheme.h"

// 图片编辑窗口的语义色：跟随「设置 → 外观 → 软件主题」（浅色 / 深色）。
// 用法：SettingTheme::apply() 之后调用 apply() 一次，再构建窗口。
namespace ImageEditTheme {
    inline bool dark{ true };
    inline uint32_t windowBg{ 0x262626FF };    // 窗口与底部工具栏底
    inline uint32_t canvasBg{ 0x171717FF };    // 工作区底
    inline uint32_t trackBg{ 0x232323FF };     // 卡片 / 次级按钮底
    inline uint32_t inputBg{ 0x232323FF };     // 输入框（尺寸 / 行列 / 数值 / 格式下拉）
    inline uint32_t navBg{ 0x232323FF };       // 模式导航胶囊 / 浮动画布胶囊的轨道底
    inline uint32_t hoverBg{ 0x333333FF };
    inline uint32_t hoverSubtle{ 0x2E2E2EFF };
    inline uint32_t fg{ 0xFAFAFAFF };
    inline uint32_t fgMuted{ 0xA3A3A3FF };
    inline uint32_t fgDisabled{ 0x4A4A4AFF };
    inline uint32_t sideLabel{ 0xD9D9D9FF };
    inline uint32_t unitDim{ 0xFFFFFF33 };
    inline uint32_t divider{ 0x222222FF };
    inline uint32_t cardLine{ 0x333333FF };
    inline uint32_t placeholder{ 0x737373FF };
    inline uint32_t accent{ 0x34C759FF };      // 品牌绿：两套主题一致
    inline uint32_t saveHover{ 0xE5E5E5FF };   // 保存按钮悬停底
    inline uint32_t primaryText{ 0x171717FF }; // 保存按钮上的文字
    inline uint32_t sliderThumb{ 0xD9D9D9FF };
    inline uint32_t sliderThumbHover{ 0xFFFFFFFF };
    inline uint32_t cardBg{ 0x333333FF };      // 合并文档卡片底
    inline uint32_t cardText{ 0xA3A3A3FF };
    inline uint32_t cellEmpty{ 0x232323FF };   // 拼图空容器底
    inline uint32_t cellEdge{ 0xFFFFFF1A };    // 空容器描边
    inline uint32_t hintText{ 0xFFFFFF99 };    // 空容器里的「+」
    inline uint32_t hintTextSoft{ 0xFFFFFF80 };// 空容器里的提示文字
    inline uint32_t snapGuide{ 0xFFFFFF80 };   // 吸附参考线
    inline uint32_t gridLine{ 0xFFFFFF0D };    // 工作区十字网格
    inline uint32_t boardHint{ 0x333333FF };   // 无背景时画板位置的示意虚线
    inline uint32_t boardEdge{ 0xBDBDC2FF };   // 画板描边（浅色下无色 / 白色底与工作区同色时）

    inline void apply()
    {
        dark = SettingTheme::isDark();
        if (dark) {
            windowBg = 0x262626FF; canvasBg = 0x171717FF; trackBg = 0x232323FF;
            inputBg = 0x232323FF;
            navBg = 0x232323FF;
            hoverBg = 0x333333FF; hoverSubtle = 0x2E2E2EFF;
            fg = 0xFAFAFAFF; fgMuted = 0xA3A3A3FF; fgDisabled = 0x4A4A4AFF;
            sideLabel = 0xD9D9D9FF; unitDim = 0xFFFFFF33;
            divider = 0x222222FF; cardLine = 0x333333FF; placeholder = 0x737373FF;
            saveHover = 0xE5E5E5FF; primaryText = 0x171717FF;
            sliderThumb = 0xD9D9D9FF; sliderThumbHover = 0xFFFFFFFF;
            cardBg = 0x333333FF; cardText = 0xA3A3A3FF;
            cellEmpty = 0x232323FF; gridLine = 0xFFFFFF0D; boardHint = 0x333333FF;
            boardEdge = 0xFFFFFF1F;
            cellEdge = 0xFFFFFF1A; hintText = 0xFFFFFF99; hintTextSoft = 0xFFFFFF80;
            snapGuide = 0xFFFFFF80;
        }
        else {
            windowBg = 0xF3F3F3FF; canvasBg = 0xFFFFFFFF; trackBg = 0xE5E5E5FF;
            inputBg = 0xFAFAFAFF;
            navBg = 0xE5E5E5FF;
            hoverBg = 0xD9D9D9FF; hoverSubtle = 0xD4D4D8FF;
            fg = 0x18181BFF; fgMuted = 0x71717AFF; fgDisabled = 0xC4C4C8FF;
            sideLabel = 0x3F3F46FF; unitDim = 0x00000055;
            divider = 0xE7E4E4FF; cardLine = 0xD4D4D8FF; placeholder = 0xA1A1AAFF;
            saveHover = 0x3F3F46FF; primaryText = 0xFAFAFAFF;
            sliderThumb = 0x52525BFF; sliderThumbHover = 0x18181BFF;
            cardBg = 0xF0F0F0FF; cardText = 0x52525BFF;
            cellEmpty = 0xF2F2F2FF; gridLine = 0x0000000D; boardHint = 0xBFBFBFFF;
            boardEdge = 0xBDBDC2FF;
            cellEdge = 0x00000014; hintText = 0x00000099; hintTextSoft = 0x00000080;
            snapGuide = 0x00000080;
        }
    }
}
