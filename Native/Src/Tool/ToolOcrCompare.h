#pragma once
#include <include/Ling.h>
#include <vector>
#include <wrl/client.h>

// OCR / 翻译结果的「对比原图」浮窗：显示选区原图（超过当前屏可用空间时等比缩小），
// 初始贴在结果面板左侧，放不下依次换右侧、上方；按住任意位置可拖动。
// 由 ToolOcr 持有，随其结果面板一起销毁。
class ToolOcrCompare : public Ling::WinBase
{
public:
	ToolOcrCompare(const std::vector<BYTE>& pixels, int imgW, int imgH, float dpi);
	~ToolOcrCompare();
	// panel：结果面板的屏幕矩形（物理像素）
	void placeNear(const RECT& panel);
private:
	void onCreated() override;
	void layout() override;
	BOOL setCursor() override;
private:
	Ling::Canvas* canvas_{ nullptr };
	Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap_;
	int imgW_{ 1 }, imgH_{ 1 };
	bool dragging_{ false };
	POINT dragCursor_{};
	int dragStartX_{ 0 }, dragStartY_{ 0 };
};
