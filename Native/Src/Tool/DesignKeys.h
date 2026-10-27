#pragma once

// 「初始属性」(toolDesign) 的工具名与键名常量：调用点不再写裸字符串，
// 拼写错误在编译期即可发现。
namespace ToolKey
{
	inline constexpr wchar_t Rect[] = L"rect", Arrow[] = L"arrow", Line[] = L"line",
		Pen[] = L"pen", Marker[] = L"marker", Text[] = L"text", Mosaic[] = L"mosaic",
		Patina[] = L"patina", Number[] = L"number", Highlight[] = L"highlight",
		Watermark[] = L"watermark", ShowCursor[] = L"show-cursor";
}

namespace DesignKey
{
	inline constexpr wchar_t Form[] = L"form", Width[] = L"width", Head[] = L"head",
		Opacity[] = L"opacity", Border[] = L"border", Angle[] = L"angle",
		Sides[] = L"sides", Points[] = L"points", Enabled[] = L"enabled";
}
