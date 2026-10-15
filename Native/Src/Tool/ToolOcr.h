#pragma once
#include <include/Ling.h>
#include <functional>
#include "../Ocr/OcrTypes.h"

class Tip;
// 选区 OCR/翻译结果浮层：底色取自选区原图（采样）+ 多块可编辑文本
// 宿主从 WinCap 泛化为 WinBase —— 截图窗（WinCap）与桌面贴图（WinPin）共用同一套面板。
class ToolOcr : public Ling::WinBase
{
public:
	ToolOcr(Ling::WinBase* host, OcrResult result, bool translateMode,
		std::vector<BYTE> image, int imageW, int imageH);
	~ToolOcr();
	void syncToMask(const D2D1_RECT_F& maskRect, int hostX, int hostY, float hostDpi);
	// 宿主是贴图时置 true（须在建窗之前设）：不显示右上角复制按钮；
	// 且只有识别文本框吃鼠标（可选中/复制文本），其余区域穿透 —— 贴图仍能右键弹菜单 / 滚轮缩放。
	void setPinOverlay(bool on) { pinOverlay_ = on; }
	void applyTranslate(const std::vector<std::wstring>& lines);
	// 翻译模式专用：把"识别出的区域 + 译文"一次性装上并重绘一次 —— 中间不显示识别出来的原文，
	// 避免"先闪一下原文、再被译文替换"。
	void setTranslatedResult(OcrResult result, const std::vector<std::wstring>& lines);
	void setResult(OcrResult result);
	void showBanner(const std::wstring& msg, bool error);
	void clearBanner();
	bool translateMode() const { return translateMode_; }
	const OcrResult& result() const { return result_; }
	// 选区原图（BGRA）。翻译模式先建面板、后识别，识别直接复用这份像素，省掉再留一份。
	const std::vector<BYTE>& srcImage() const { return srcImage_; }
	// 宿主注入：把（截图/贴图的）标注画到识别文字**之上**。识别结果浮层是独立置顶窗口，
	// 而标注画在宿主窗口里，两个窗口没法互相穿插 —— 于是把标注"镜像"到本窗口顶层，
	// 视觉上就变成"标注盖在识别结果上面"。
	void setAnnotPainter(std::function<void(ID2D1DeviceContext*)> fn) { annotPainter_ = std::move(fn); }
	// 标注变化时由宿主调用，重画镜像层。
	void repaintOverlay() { if (hwnd) refresh(); }
	// 主动重绘本面板（提示条/文字改动后调用，别指望宿主那次 refresh 会带上本窗口）
	void repaint() { if (hwnd) refresh(); }
private:
	void onCreated() override;
	void layout() override;
	void rebuildEditors();
	// 把译文本行填进各区域的 displayText 并重算整段文字（不重绘）
	void applyTranslateLines(const std::vector<std::wstring>& lines);
	void copyAll();
	// —— 跨行选区（每行是独立控件，选中范围由本类统一算）——
	struct SelPoint { int line{ -1 }; int index{ 0 }; };
	SelPoint hitSelPoint(POINT client) const;
	void applySelection();
	void clearSelection();
	std::wstring selectedText() const;
	LRESULT onHitTest(const POINT pos) override;
	void onMinMaxInfo(MINMAXINFO* mmi) override;
private:
	Ling::WinBase* host;
	// 覆盖矩形（客户区坐标）：onHitTest 用它把选区外的部分设为穿透
	D2D1_RECT_F maskRect_{};
	bool hasMask_{ false };
	bool pinOverlay_{ false };
	// 上次建编辑框时的面板物理尺寸：仅尺寸变化才重建（拖动只挪窗口，不必重建）。
	int panelW_{ 0 };
	int panelH_{ 0 };
	// 每个识别框的"背景块"（严格等于识别框尺寸，负责盖住原文字）与文本框分开存。
	std::vector<Ling::Node*> bgs_;
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
	// —— 跨行选区状态（阅读顺序：行优先；index = 行内字符偏移）——
	bool selActive_{ false };
	int selALine_{ -1 }, selAIdx_{ 0 }, selBLine_{ -1 }, selBIdx_{ 0 };
	// —— 标注镜像层：盖在识别文字之上的画布 ——
	Ling::Canvas* annotCanvas_{ nullptr };
	std::function<void(ID2D1DeviceContext*)> annotPainter_;
	// 选区原图（BGRA，逐行自上而下）——用于采样叠层底色；底面不透明，文字色按底色亮度反相
	std::vector<BYTE> srcImage_;
	uint32_t palBg_{ 0xFFFFFFF2 };
	uint32_t palFg_{ 0x14181CFF };
};
