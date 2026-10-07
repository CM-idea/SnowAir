#include "pch.h"
#include "RoundBox.h"
#include <algorithm>

using Microsoft::WRL::ComPtr;

namespace RoundBox {

float maxRadius(const D2D1_RECT_F& r)
{
	const float w = std::max(0.f, r.right - r.left);
	const float h = std::max(0.f, r.bottom - r.top);
	return std::min(w, h) * 0.5f;
}

void clamp(const D2D1_RECT_F& r, const float in[4], float out[4])
{
	const float w = std::max(0.f, r.right - r.left);
	const float h = std::max(0.f, r.bottom - r.top);
	float v[4]{};
	for (int i = 0; i < 4; i++) v[i] = std::max(0.f, in[i]);
	// 同一条边上两角半径之和超过边长时，四角**等比**缩（CSS border-radius 的做法）：
	// 等比才不会出现"对称输入被压成一只角大、三角小"的怪结果。
	float f = 1.f;
	auto edge = [&](float a, float b, float len) {
		const float s = a + b;
		if (s > len && s > 0.f) f = std::min(f, len / s);
	};
	edge(v[0], v[1], w); // 顶边
	edge(v[3], v[2], w); // 底边
	edge(v[0], v[3], h); // 左边
	edge(v[1], v[2], h); // 右边
	for (int i = 0; i < 4; i++) out[i] = v[i] * f;
}

float maxRadiusFor(const D2D1_RECT_F& r, const float radii[4], int slot)
{
	const float w = std::max(0.f, r.right - r.left);
	const float h = std::max(0.f, r.bottom - r.top);
	float m = 0.f;
	switch (slot) {
	case 0: m = std::min(w - radii[1], h - radii[3]); break; // TL 与 TR 共顶边、与 BL 共左边
	case 1: m = std::min(w - radii[0], h - radii[2]); break; // TR 与 TL 共顶边、与 BR 共右边
	case 2: m = std::min(w - radii[3], h - radii[1]); break; // BR 与 BL 共底边、与 TR 共右边
	default: m = std::min(w - radii[2], h - radii[0]); break; // BL 与 BR 共底边、与 TL 共左边
	}
	return std::max(0.f, m);
}

bool isSquare(const float radii[4])
{
	for (int i = 0; i < 4; i++) if (radii[i] > 0.01f) return false;
	return true;
}

ComPtr<ID2D1PathGeometry> build(ID2D1Factory* factory, const D2D1_RECT_F& r, const float radii[4])
{
	ComPtr<ID2D1PathGeometry> geo;
	if (!factory) return geo;
	float v[4]{};
	clamp(r, radii, v);
	if (FAILED(factory->CreatePathGeometry(geo.GetAddressOf())) || !geo) return {};
	ComPtr<ID2D1GeometrySink> sink;
	if (FAILED(geo->Open(sink.GetAddressOf())) || !sink) return {};

	const float L = r.left, T = r.top, R = r.right, B = r.bottom;
	const float rTL = v[0], rTR = v[1], rBR = v[2], rBL = v[3];
	auto arcTo = [&](float x, float y, float rad) {
		sink->AddArc(D2D1::ArcSegment(
			D2D1::Point2F(x, y), D2D1::SizeF(rad, rad), 0.f,
			D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
	};

	sink->SetFillMode(D2D1_FILL_MODE_WINDING);
	sink->BeginFigure(D2D1::Point2F(L + rTL, T), D2D1_FIGURE_BEGIN_FILLED);
	sink->AddLine(D2D1::Point2F(R - rTR, T));
	if (rTR > 0.01f) arcTo(R, T + rTR, rTR);
	sink->AddLine(D2D1::Point2F(R, B - rBR));
	if (rBR > 0.01f) arcTo(R - rBR, B, rBR);
	sink->AddLine(D2D1::Point2F(L + rBL, B));
	if (rBL > 0.01f) arcTo(L, B - rBL, rBL);
	sink->AddLine(D2D1::Point2F(L, T + rTL));
	if (rTL > 0.01f) arcTo(L + rTL, T, rTL);
	sink->EndFigure(D2D1_FIGURE_END_CLOSED);
	if (FAILED(sink->Close())) return {};
	return geo;
}

} // namespace RoundBox
