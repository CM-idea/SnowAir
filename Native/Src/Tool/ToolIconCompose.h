#pragma once
#include <include/Ling.h>
#include "IconCodes.h"

// 复合图标：底字形 + 半透明叠字形（例如「模糊」= 模糊2 + 模糊1 @25%）。
// 设置页工具行 / 预览条 / 显示区 chip / 实际工具栏共用，保证几处图标一致。
namespace ToolIcon {
	inline uint32_t withAlpha(uint32_t rgba, uint32_t a) { return (rgba & 0xFFFFFF00u) | (a & 0xFFu); }

	inline Ling::Node* overlayGlyph(Ling::Node* host, const wchar_t* base, const wchar_t* top,
		float fontSize, uint32_t baseColor, uint32_t topColor)
	{
		if (!host) return nullptr;
		auto* stack = host->makeChild<Ling::Node>();
		stack->setPositionType(Ling::Position::Absolute);
		stack->setPosition(Ling::Edge::Left, 0.f);
		stack->setPosition(Ling::Edge::Right, 0.f);
		stack->setPosition(Ling::Edge::Top, 0.f);
		stack->setPosition(Ling::Edge::Bottom, 0.f);
		auto addGlyph = [&](const wchar_t* code, uint32_t color) {
			auto* lb = stack->makeChild<Ling::Label>();
			lb->setText(code);
			lb->setFontFamily(Icon::Family);
			lb->setFontSize(fontSize);
			lb->setColor(color);
			lb->setPositionType(Ling::Position::Absolute);
			lb->setPosition(Ling::Edge::Left, 0.f);
			lb->setPosition(Ling::Edge::Right, 0.f);
			lb->setPosition(Ling::Edge::Top, 0.f);
			lb->setPosition(Ling::Edge::Bottom, 0.f);
			lb->setJustifyContent(Ling::Justify::Center);
			lb->setAlignItems(Ling::Align::Center);
		};
		addGlyph(base, baseColor);
		addGlyph(top, topColor);
		return stack;
	}
}
