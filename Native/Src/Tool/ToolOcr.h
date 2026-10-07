#pragma once
#include <include/Ling.h>
#include "../Ocr/OcrTypes.h"

class WinCap;
// 选区 OCR/翻译结果浮层：半透明底 + 多块可编辑文本
class ToolOcr : public Ling::WinBase
{
public:
	ToolOcr(WinCap* win, OcrResult result, bool translateMode);
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
	int imgW{ 1 }, imgH{ 1 };
};
