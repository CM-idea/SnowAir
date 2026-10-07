#include "pch.h"
#include "PropSlider.h"

PropSlider::Pair PropSlider::mount(Ling::Node* parent, float minV, float maxV, float val,
	std::function<void(float)> onChange)
{
	Pair out{};
	if (!parent) return out;

	const float marginV = (ToolbarTheme::propBtnSize - ToolbarTheme::propIconInner) * 0.5f;
	const float mH = marginH();

	out.slider = parent->makeChild<Ling::Slider>();
	out.slider->setWidth(ToolbarTheme::sliderWidth);
	out.slider->setHeight(ToolbarTheme::propIconInner);
	out.slider->setMargin(mH, marginV, 0.f, marginV);
	out.slider->setRange(minV, maxV);
	out.slider->setValue(val);
	out.slider->setStep(1.f);
	out.slider->setThumbColor(ToolbarTheme::sliderThumb);
	out.slider->setHoverThumbColor(ToolbarTheme::sliderThumb);
	out.slider->setTrackColor(ToolbarTheme::sliderTrack);
	out.slider->setFillColor(ToolbarTheme::sliderFill);

	out.value = parent->makeChild<Ling::Label>();
	out.value->setWidth(ToolbarTheme::sliderValueWidth);
	out.value->setHeight(ToolbarTheme::propIconInner);
	out.value->setMargin(ToolbarTheme::sliderValueGap, marginV, 0.f, marginV);
	out.value->setAlignItems(Ling::Align::Center);
	out.value->setJustifyContent(Ling::Justify::Center);
	out.value->setFontSize(12.f);
	out.value->setColor(Icon::ColorNormal);
	out.value->setText(std::format(L"{}", static_cast<int>(std::round(val))));

	out.slider->onValueChanged.add([onChange, label = out.value](Ling::Slider*, float v) {
		if (label) label->setText(std::format(L"{}", static_cast<int>(std::round(v))));
		if (onChange) onChange(v);
	});
	return out;
}
