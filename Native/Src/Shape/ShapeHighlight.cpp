#include "pch.h"
#include "Win/AnnotHost.h"
#include "Win/WinCap.h"
#include "Win/CutMask.h"
#include "Tool/ToolSub.h"
#include "ShapeHighlight.h"

ShapeHighlight::ShapeHighlight(AnnotHost* win) : ShapeArea(win)
{
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(win->toolSub->getSelectedColor(), brushBorder.GetAddressOf());
	applyStyle();
}

void ShapeHighlight::applyStyle()
{
	if (!win->toolSub) return;
	strokeWidth = win->toolSub->getSliderVal();
	isEllipse = win->toolSub->isHighlightEllipse;
	showBorder = win->toolSub->isHighlightBorder;
	if (brushBorder) brushBorder->SetColor(win->toolSub->getSelectedColor());
}

D2D1_RECT_F ShapeHighlight::viewportOf(AnnotHost* win)
{
	if (auto* cap = dynamic_cast<WinCap*>(win)) {
		if (cap->cutMask && cap->cutMask->hasRect())
			return cap->cutMask->maskRect;
	}
	return D2D1::RectF(0.f, 0.f, (float)win->w, (float)win->h);
}

void ShapeHighlight::paintMerged(ID2D1DeviceContext* ctx, AnnotHost* win,
	const std::vector<ShapeHighlight*>& holes)
{
	if (holes.empty()) return;
	const auto vp = viewportOf(win);
	if (vp.right - vp.left < 2.f || vp.bottom - vp.top < 2.f) return;

	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> dim;
	Ling::D2D::get()->deviceContext->CreateSolidColorBrush(
		D2D1::ColorF(0.f, 0.f, 0.f, 0.45f), dim.GetAddressOf());
	if (!dim) return;

	auto factory = Ling::D2D::get()->d2dFactory.Get();
	Microsoft::WRL::ComPtr<ID2D1RectangleGeometry> outer;
	factory->CreateRectangleGeometry(vp, outer.GetAddressOf());

	// 逐个挖洞：outer EXCLUDE hole → 并集透明区
	Microsoft::WRL::ComPtr<ID2D1Geometry> cur = outer;
	for (auto* h : holes) {
		const auto& r = h->hole();
		if (r.right - r.left < 1.f || r.bottom - r.top < 1.f) continue;
		auto holeGeo = h->makeAreaGeometry(h->ellipseHole());
		if (!holeGeo) continue;
		Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
		factory->CreatePathGeometry(path.GetAddressOf());
		Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
		if (FAILED(path->Open(sink.GetAddressOf()))) continue;
		cur->CombineWithGeometry(holeGeo.Get(), D2D1_COMBINE_MODE_EXCLUDE, nullptr, sink.Get());
		sink->Close();
		cur = path;
	}
	ctx->FillGeometry(cur.Get(), dim.Get());

	// 描边：有边框的洞做 UNION，再画一条外轮廓（重叠处不叠线）
	Microsoft::WRL::ComPtr<ID2D1Geometry> borderUnion;
	ID2D1SolidColorBrush* borderBrush = nullptr;
	float borderW = 1.f;
	for (auto* h : holes) {
		if (!h->bordered() || !h->borderBrush()) continue;
		const auto& r = h->hole();
		if (r.right - r.left < 1.f || r.bottom - r.top < 1.f) continue;
		auto holeGeo = h->makeAreaGeometry(h->ellipseHole());
		if (!holeGeo) continue;
		borderBrush = h->borderBrush();
		borderW = std::max(borderW, h->borderWidth());
		if (!borderUnion) {
			borderUnion = holeGeo;
			continue;
		}
		Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
		factory->CreatePathGeometry(path.GetAddressOf());
		Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
		if (FAILED(path->Open(sink.GetAddressOf()))) continue;
		borderUnion->CombineWithGeometry(holeGeo.Get(), D2D1_COMBINE_MODE_UNION, nullptr, sink.Get());
		sink->Close();
		borderUnion = path;
	}
	if (borderUnion && borderBrush)
		ctx->DrawGeometry(borderUnion.Get(), borderBrush, std::max(1.f, borderW));
}

void ShapeHighlight::paint(ID2D1DeviceContext* ctx)
{
	(void)ctx; // 压暗由 AnnotHost 调 paintMerged 统一画
}
