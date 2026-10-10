#pragma once
#include <include/Ling.h>
#include "../Ocr/OcrTypes.h"

class WinCap;
class Tip;
// 选区 OCR/翻译结果浮层：底色取自选区原图（采样）+ 多块可编辑文本
class ToolOcr : public Ling::WinBase
{
public:
	ToolOcr(WinCap* win, OcrResult result, bool translateMode,
		std::vector<BYTE> image, int imageW, int imageH);
	~ToolOcr();
	void syncToMask(const D2D1_RECT_F& maskRect, int hostX, int hostY, float hostDpi);
	void applyTranslate(const std::vector<std::wstring>& lines);
	void setResult(OcrResult result);
	void showBanner(const std::wstring& msg, bool error);
	void clearBanner();
	bool translateMode() const { return translateMode_; }
	const OcrResult& result() const { return result_; }
private:
	void onCreated() override;
	void rebuildEditors();
	void copyAll();
	LRESULT onHitTest(const POINT pos) override;
	void onMinMaxInfo(MINMAXINFO* mmi) override;
private:
	WinCap* win;
	OcrResult result_;
	bool translateMode_{ false };
	std::wstring banner_;
	bool bannerError_{ false };
	Ling::Node* bannerNode{ nullptr };
	Ling::Label* bannerLabel{ nullptr };
	std::vector<Ling::TextBox*> editors;
	Ling::Button* copyBtn{ nullptr };
	std::unique_ptr<Tip> tip;
	int imgW{ 1 }, imgH{ 1 };
	// 选区原图（BGRA，逐行自上而下）——用于采样叠层底色；底面不透明，文字色按底色亮度反相
	std::vector<BYTE> srcImage_;
	uint32_t palBg_{ 0xFFFFFFF2 };
	uint32_t palFg_{ 0x14181CFF };
};
