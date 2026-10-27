#pragma once
#include <string>
#include <vector>
#include "../Setting.h"
#include "IconCodes.h"

// 「外观 > 工具栏外观 > 初始属性」的工具默认形态表（设置页 / 预览条 / 工具栏共用同一份）。
// 拖到第一位的形态 = 该工具的默认形态：各处只换图标；工具栏按钮的停悬文案仍是工具名
// （箭头 / 画笔 / 矩形 / 标注 / 序号），不随默认形态变化。
namespace ToolInitial {

	struct InitialChoice {
		int value;
		const wchar_t* glyph;
		const wchar_t* tipKey;
		bool icon;   // true = 图标字形（Icon::Family），false = 短文字
		const wchar_t* overlayGlyph{ nullptr };   // 非空 = 复合图标：底字形 + 半透明叠字形
	};
	struct InitialNum {
		const wchar_t* key;        // toolDesign 里存的键
		const wchar_t* labelKey;   // 滑条前的文案
		int min, max, def;
	};
	struct InitialToolDef {
		const wchar_t* id;         // 初始属性工具 id
		const wchar_t* nameKey;
		const wchar_t* icon;       // 未设置形态时的通用图标
		const wchar_t* designId;   // 写盘用的工具 id
		const wchar_t* formKey;    // 形态键（nullptr = 没有形态行）
		const std::vector<InitialChoice>* forms;
		const std::vector<InitialNum>* nums;
	};

	// 形态枚举值（与 ToolSub 的 ArrowHead / LineKind / NumberStyle 对齐）
	// 顺序与箭头属性栏一致：常规箭头 / 大箭头 / 趣味箭头 / 双箭头 / 标注箭头
	inline const std::vector<InitialChoice> kArrowForms = {
		{ 0, Icon::Arrow,     L"tool.arrowDefault", true },   // ArrowDefault
		{ 1, Icon::ArrowBig,  L"tool.arrowBig",     true },   // ArrowBig
		{ 4, Icon::FunArrow,  L"tool.arrowFun",     true },   // ArrowFun
		{ 2, Icon::ArrowBoth, L"tool.arrowBoth",    true },   // ArrowBoth
		{ 3, Icon::AnnotBoth, L"tool.arrowAnnot",   true },   // ArrowAnnot
	};
	// 标注：默认形态是「尺寸」（表首项），其余：气泡 / 爆裂 / 角度 / 波浪线。
	// 直线不在标注里：它是箭头属性栏的「线段」临时开关。
	inline const std::vector<InitialChoice> kLineForms = {
		{ 4, Icon::Dimension,      L"tool.dimension",   true },   // LineDimension
		{ 2, Icon::Bubble,         L"tool.bubble",      true },   // LineBubble
		{ 5, Icon::ExplosionCloud, L"tool.burst",       true },   // LineBurst
		{ 3, Icon::Angle,          L"tool.angleAnnot",  true },   // LineAngle
		{ 1, Icon::WaveLine,       L"tool.waveLine",    true },   // LineWave
	};
	inline const std::vector<InitialChoice> kRectForms = {
		{ 0, Icon::Rect,    L"tool.rectDefault", true },   // 方形
		{ 1, Icon::Ellipse, L"tool.ellipse",     true },
		{ 2, Icon::Polygon, L"tool.polygon",     true },
		{ 3, Icon::Star,    L"tool.star",        true },
	};
	// 顺序与画笔属性栏一致：常规画笔 / 荧光笔 / 软笔（虚线不进初始属性）
	inline const std::vector<InitialChoice> kPenForms = {
		{ 0, Icon::Pen,     L"tool.penDefault", true },   // 常规画笔
		{ 3, Icon::Marker,  L"tool.marker",     true },   // form=3 = 荧光笔（与 ToolSub 约定）
		{ 1, Icon::SoftPen, L"tool.softPen",    true },
	};
	inline const std::vector<InitialChoice> kNumberForms = {
		{ 0, Icon::Number,       L"tool.numberDefault", true },   // 数字序号
		{ 1, Icon::NumberLetter, L"tool.numberLetter",  true },   // NumberLetter
		{ 2, Icon::NumberEmoji,  L"tool.numberEmoji",   true },   // NumberEmoji
	};
	// 模糊：实际属性栏是「模糊2 + 浅色模糊1」复合图标
	inline const std::vector<InitialChoice> kMosaicForms = {
		{ 0, Icon::Mosaic, L"tool.mosaic", true },
		{ 1, Icon::Blur2,  L"tool.blur",   true, Icon::Blur },
	};
	// 属性栏里用「矩形 / 椭圆」图标，方便辨认（工具本身的图标不随形态变化）
	inline const std::vector<InitialChoice> kHighlightForms = {
		{ 0, Icon::Rect,      L"tool.highlightRect",    true },
		{ 1, Icon::Ellipse,   L"tool.highlightEllipse", true },
	};

	inline const std::vector<InitialNum> kWidthNum = { { L"width", L"setting.tbInitialWidth", 1, 64, 4 } };
	inline const std::vector<InitialNum> kPolyNums = {
		{ L"sides", L"setting.tbInitialSides", 3, 20, 5 },
		{ L"width", L"setting.tbInitialWidth", 1, 64, 4 },
	};
	inline const std::vector<InitialNum> kStarNums = {
		{ L"points", L"setting.tbInitialPoints", 3, 20, 5 },
		{ L"width", L"setting.tbInitialWidth", 1, 64, 4 },
	};
	inline const std::vector<InitialNum> kNumberNums = { { L"radius", L"setting.tbInitialSize", 6, 200, 24 } };
	inline const std::vector<InitialNum> kTextNums   = { { L"fontSize", L"setting.tbInitialSize", 8, 333, 24 } };
	inline const std::vector<InitialNum> kMosaicNums = { { L"width", L"setting.tbInitialSize", 0, 100, 30 } };
	// 包浆只调强度
	inline const std::vector<InitialNum> kPatinaNums = { { L"width", L"tool.patinaSize", 0, 100, 15 } };
	// 水印：只调字号（默认角度不进初始属性）
	inline const std::vector<InitialNum> kWatermarkNums = {
		{ L"fontSize", L"setting.tbInitialSize", 12, 200, 25 },
	};

	inline const std::vector<InitialToolDef> kInitialToolDefs = {
		{ L"show-cursor", L"tool.showCursor", Icon::Cursor, L"show-cursor", nullptr, nullptr, nullptr },
		{ L"arrow", L"tool.arrow", Icon::Arrow, L"arrow", L"head", &kArrowForms, &kWidthNum },
		{ L"line", L"tool.line", Icon::Line, L"line", L"form", &kLineForms, &kWidthNum },
		{ L"rect", L"tool.rect", Icon::Rect, L"rect", L"form", &kRectForms, &kWidthNum },
		{ L"pen", L"tool.pen", Icon::Pen, L"pen", L"form", &kPenForms, &kWidthNum },
		{ L"laser", L"tool.fadePen", Icon::FadePen, L"pen", nullptr, nullptr, &kWidthNum },
		{ L"number", L"tool.number", Icon::Number, L"number", L"form", &kNumberForms, &kNumberNums },
		{ L"text", L"tool.text", Icon::Text, L"text", nullptr, nullptr, &kTextNums },
		{ L"mosaic", L"tool.mosaic", Icon::Mosaic, L"mosaic", L"form", &kMosaicForms, &kMosaicNums },
		{ L"patina", L"tool.patina", Icon::Patina, L"patina", nullptr, nullptr, &kPatinaNums },
		{ L"watermark", L"tool.watermark", Icon::Watermark, L"watermark", nullptr, nullptr, &kWatermarkNums },
		{ L"highlight", L"tool.highlight", Icon::Highlight, L"highlight", L"form", &kHighlightForms, &kWidthNum },
	};

	inline const InitialToolDef* find(const std::wstring& id)
	{
		for (auto& d : kInitialToolDefs)
			if (id == d.id) return &d;
		return nullptr;
	}

	// 工具栏槽位 id（ToolbarStore / ToolCap）→ 初始属性工具 id
	inline std::wstring defIdOfSlot(const std::wstring& slotId)
	{
		if (slotId == L"laser-pointer" || slotId == L"laser") return L"laser";
		if (slotId == L"serial-number" || slotId == L"number") return L"number";
		return slotId;
	}

	// 当前默认形态（没有形态/未设置时取表首项）
	inline const InitialChoice* currentForm(const InitialToolDef& d)
	{
		if (!d.formKey || !d.forms || d.forms->empty()) return nullptr;
		int v = d.forms->front().value;
		if (auto* s = Setting::get())
			v = s->getToolDesignInt(d.designId, d.formKey, v);
		for (auto& c : *d.forms)
			if (c.value == v) return &c;
		return &d.forms->front();
	}

	// 该工具当前默认形态的图标（无形态时回落通用图标）
	inline std::wstring iconOf(const InitialToolDef& d)
	{
		// 高亮固定沿用工具原图标（不随默认形态变化）
		if (wcscmp(d.id, L"highlight") == 0) return std::wstring(d.icon);
		const auto* c = currentForm(d);
		return c ? std::wstring(c->glyph) : std::wstring(d.icon);
	}

	// 工具栏槽位应显示的图标（空 = 保持原图标）
	inline std::wstring iconForSlot(const std::wstring& slotId)
	{
		const auto* d = find(defIdOfSlot(slotId));
		if (!d || !d->formKey) return {};
		if (wcscmp(d->id, L"highlight") == 0) return {};   // 高亮：保持原图标
		return iconOf(*d);
	}

	// 马赛克默认形态是否已切到「模糊」
	inline bool isMosaicBlur()
	{
		const auto* d = find(L"mosaic");
		const auto* c = d ? currentForm(*d) : nullptr;
		return c && c->value == 1;
	}

	// 该槽位需要替换的停悬文案 key（目前只有马赛克切模糊时改名，其余返回 nullptr 用原文案）
	inline const wchar_t* nameKeyForSlot(const std::wstring& slotId)
	{
		if (defIdOfSlot(slotId) == L"mosaic" && isMosaicBlur()) return L"tool.blur";
		return nullptr;
	}
}
