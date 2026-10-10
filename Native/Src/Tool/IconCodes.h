#pragma once
#include <cstdint>
// 轻雪图标字码（iconfont 项目 font_5228143_zy5cb71piv，字体族名 font_family）。
// 字库是全集：这里列出全部码点，当前 UI 只用到一部分，其余留给后续功能。
namespace Icon
{
	inline constexpr const wchar_t* Family = L"font_family";
	// 设计稿 24×24：IcoMoon/iconfont 字形高度约等于 font-size
	inline constexpr float Size{ 22.f };
	inline constexpr float SizeSm{ 20.f }; // 信息栏图标（槽 24）
	inline constexpr float DropSize{ 16.f }; // 下拉箭头（略小于 SizeSm）

	// ColorNormal / ColorDisabled 会被 ToolbarTheme::refresh() 按主题重写（工具栏图标随深浅色反转），
	// 故为运行时变量而非 constexpr；其余语义色（选中/取消/完成）保持常量。
	inline uint32_t ColorNormal{ 0x515151FFu }; // 默认图标
	inline uint32_t ColorDisabled{ 0x51515166u }; // 不可用（半透明，光标仍用普通箭头）
	inline constexpr uint32_t ColorActive{ 0x34C759FFu }; // 选中
	inline constexpr uint32_t ColorCancel{ 0xFF383CFFu }; // 取消
	inline constexpr uint32_t ColorDone{ 0x34C759FFu };   // 完成

	// —— 当前工具栏 / 属性栏会用到的 ——
	inline constexpr const wchar_t* DragHandle = L"\ue640"; // 拖拽（工具栏左侧手柄）
	inline constexpr const wchar_t* Lock = L"\ue641";           // 锁定
	inline constexpr const wchar_t* Unlock = L"\ue643";         // 解锁
	inline constexpr const wchar_t* UnlockAlt = L"\ue642";      // 解锁2
	inline constexpr const wchar_t* Play = L"\ue646";           // 开始 / 播放
	inline constexpr const wchar_t* Pause = L"\ue645";          // 暂停
	inline constexpr const wchar_t* Audio = L"\ue644";          // 音频（系统声）
	inline constexpr const wchar_t* Rect = L"\ue632";           // 矩形
	inline constexpr const wchar_t* Ellipse = L"\ue605";        // 椭圆
	inline constexpr const wchar_t* Arrow = L"\ue637";          // 箭头
	inline constexpr const wchar_t* ArrowBig = L"\ue63d";       // 大箭头
	inline constexpr const wchar_t* Number = L"\ue618";         // 数字序号
	inline constexpr const wchar_t* NumberLetter = L"\ue607";   // 字母序号
	inline constexpr const wchar_t* NumberEmoji = L"\ue638";    // 表情序号
	inline constexpr const wchar_t* Line = L"\ue60d";           // 线段
	inline constexpr const wchar_t* Text = L"\ue622";           // 文字
	inline constexpr const wchar_t* Mosaic = L"\ue628";         // 马赛克
	inline constexpr const wchar_t* Eraser = L"\ue606";         // 橡皮擦
	inline constexpr const wchar_t* Undo = L"\ue63f";           // 撤销
	// 重做无工具栏按钮；History::redo + Ctrl+Y 仍可用。字库若日后补「重做」可再挂上。
	inline constexpr const wchar_t* Redo = L"\ue60a";           // 重置（备用）
	inline constexpr const wchar_t* LongShot = L"\ue608";       // 长截图
	inline constexpr const wchar_t* Video = L"\ue630";          // 录屏
	inline constexpr const wchar_t* Ocr = L"\ue61d";            // 识别文字
	inline constexpr const wchar_t* Qrcode = L"\ue604";         // 扫码
	inline constexpr const wchar_t* Save = L"\ue635";           // 保存
	inline constexpr const wchar_t* Cancel = L"\ue624";         // 取消
	inline constexpr const wchar_t* Done = L"\ue616";           // 完成
	inline constexpr const wchar_t* Copy = L"\ue647";           // 复制
	inline constexpr const wchar_t* CopyOk = L"\ue648";         // 复制成功
	inline constexpr const wchar_t* Pin = L"\ue612";            // 贴图
	inline constexpr const wchar_t* Fill = L"\ue629";           // 填充
	inline constexpr const wchar_t* Bold = L"\ue62e";           // 加粗
	inline constexpr const wchar_t* Italic = L"\ue625";         // 斜体
	inline constexpr const wchar_t* Pierce = L"\ue62f";         // 穿透模式（半透明）
	inline constexpr const wchar_t* MicOn = L"\ue63b";          // 打开麦克风
	inline constexpr const wchar_t* MicOff = L"\ue62a";         // 关闭麦克风

	// —— 字库其余（后续功能） ——
	inline constexpr const wchar_t* Drag = L"\ue610";           // 拖动（旧字形，手柄用 DragHandle）
	inline constexpr const wchar_t* Hand = L"\ue614";           // 抓手
	inline constexpr const wchar_t* Shadow = L"\ue61b";         // 阴影
	inline constexpr const wchar_t* Html = L"\ue639";           // HTML
	inline constexpr const wchar_t* Gif = L"\ue63a";            // GIF
	inline constexpr const wchar_t* Patina = L"\ue63c";         // 包浆
	inline constexpr const wchar_t* RoundCorner = L"\ue61e";    // 圆角
	inline constexpr const wchar_t* ArrowBoth = L"\ue61f";      // 双向箭头
	inline constexpr const wchar_t* Right = L"\ue620";          // 右
	inline constexpr const wchar_t* AnnotBoth = L"\ue621";      // 双向标注
	inline constexpr const wchar_t* TranslateGoogle = L"\ue623";// 谷歌翻译
	inline constexpr const wchar_t* Angle = L"\ue626";          // 角度
	inline constexpr const wchar_t* Blur = L"\ue627";           // 模糊（模糊1）
	inline constexpr const wchar_t* Blur2 = L"\ue633";          // 模糊2（与 Blur 叠加成复合图标）
	inline constexpr const wchar_t* PatinaWatermark = L"\ue62b";// 包浆水印
	inline constexpr const wchar_t* Markdown = L"\ue62c";       // MD文件
	inline constexpr const wchar_t* TranslateMs = L"\ue62d";    // 微软翻译
	inline constexpr const wchar_t* Translate = L"\ue631";      // 翻译
	inline constexpr const wchar_t* SoftPen = L"\ue64f";        // 软笔（字库码位 0xE64F）
	inline constexpr const wchar_t* Cursor = L"\ue634";         // 光标
	inline constexpr const wchar_t* Logo = L"\ue636";           // LOGO
	inline constexpr const wchar_t* CornerMidBR = L"\ue60b";    // 中下右
	inline constexpr const wchar_t* Dash = L"\ue60c";           // 虚线
	inline constexpr const wchar_t* Watermark = L"\ue60e";      // 水印
	inline constexpr const wchar_t* CornerBR = L"\ue60f";       // 下右
	inline constexpr const wchar_t* UnlockAspect = L"\ue611";   // 长宽比锁定（选中变色）
	inline constexpr const wchar_t* GreenScreen = L"\ue613";    // 绿镜
	inline constexpr const wchar_t* DashRect = L"\ue615";       // 虚线框
	inline constexpr const wchar_t* Eyedropper = L"\ue617";     // 吸管
	inline constexpr const wchar_t* FadePen = L"\ue619";        // 渐隐画笔
	inline constexpr const wchar_t* Pen = L"\ue61a";            // 画笔
	inline constexpr const wchar_t* Highlight = L"\ue603";      // 高亮
	inline constexpr const wchar_t* Folder = L"\ue61c";         // 文件夹
	inline constexpr const wchar_t* RoundCap = L"\ue609";       // 圆头

	// —— 设置页 / 下拉 / 折叠（新字库 e649-e651）——
	inline constexpr const wchar_t* Plugin = L"\ue649";         // 插件集成
	inline constexpr const wchar_t* Dropdown = L"\ue64a";       // 下拉
	inline constexpr const wchar_t* Appearance = L"\ue64b";     // 外观设置
	inline constexpr const wchar_t* Function = L"\ue64c";       // 功能设置
	inline constexpr const wchar_t* Hotkey = L"\ue64d";         // 热键设置
	inline constexpr const wchar_t* Quick = L"\ue64e";          // 快捷翻译
	inline constexpr const wchar_t* Collapse = L"\ue650";       // 折叠
	inline constexpr const wchar_t* Expand = L"\ue651";         // 展开

	// —— 加载动画（热键设置「转圈圈」，8 帧顺时针，加载1→加载8）——
	inline constexpr const wchar_t* Loading1 = L"\ue663";       // 加载1
	inline constexpr const wchar_t* Loading2 = L"\ue662";       // 加载2
	inline constexpr const wchar_t* Loading3 = L"\ue661";       // 加载3
	inline constexpr const wchar_t* Loading4 = L"\ue660";       // 加载4
	inline constexpr const wchar_t* Loading5 = L"\ue65f";       // 加载5
	inline constexpr const wchar_t* Loading6 = L"\ue65e";       // 加载6
	inline constexpr const wchar_t* Loading7 = L"\ue65d";       // 加载7
	inline constexpr const wchar_t* Loading8 = L"\ue65c";       // 加载8

	// —— 快速翻译顶栏（新字库 e664）——
	inline constexpr const wchar_t* Swap = L"\ue664";           // 切换（源/目标语言互换）

	// —— 设置页 / 快速功能（新字库 e652-e65b）——
	inline constexpr const wchar_t* Add = L"\ue652";            // 加号
	inline constexpr const wchar_t* Frame = L"\ue653";          // 翻译2
	inline constexpr const wchar_t* Send = L"\ue654";           // 发送
	inline constexpr const wchar_t* Send2 = L"\ue655";          // 发送2
	inline constexpr const wchar_t* Fast = L"\ue656";           // 快速
	inline constexpr const wchar_t* Problem = L"\ue657";        // 问题
	inline constexpr const wchar_t* Youdao = L"\ue658";         // 有道
	inline constexpr const wchar_t* Baidu = L"\ue659";          // 百度
	inline constexpr const wchar_t* Download = L"\ue65a";       // 下载
	inline constexpr const wchar_t* Remove = L"\ue65b";         // 删除
}
