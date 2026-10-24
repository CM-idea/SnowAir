#pragma once
#include <include/Ling.h>
#include <string>
#include <vector>

// 剪切板内容的贴屏窗：
//   · 文字：默认 350×150，内容多时自动长到 350×350，再超出就滚动；可拖边放大，文字可选中复制
//   · 文件：图标 + 文件名，默认 350×75；双击用系统默认程序打开
class ClipPin : public Ling::WinBase
{
public:
	static void showText(const std::wstring& text);
	static void showFiles(const std::vector<std::wstring>& files);
	~ClipPin();
private:
	ClipPin(bool isFiles, std::wstring text, std::vector<std::wstring> paths);
	void onCreated() override;
	void layout() override;
	BOOL setCursor() override;
	LRESULT onHitTest(POINT screenPos) override;
	void onClosed();
	void closeSelf();
	// 双击 / 右键「打开」共用的打开通道：链接交给浏览器，图片 / 视频 / 文件夹 / 其它文件
	// 交给系统默认程序；不可打开时返回空
	std::wstring openTarget() const;
	void openContent();
	// 打开第 index 个文件，并盯一下它被哪个窗口接住（之后单击贴屏把那个窗口拉回前台）
	void openFileAt(size_t index);
	// 单击贴屏（文件类型）：把该文件上次打开的窗口拉回前台；没有就什么也不做
	bool focusOpenedWindow(int index);
	// 把窗口位置夹进光标所在显示器的工作区
	void clampToWorkArea();
	// 文字贴屏：该点是否落在实际文字（字形）上 / 文本框自己的滚动条上
	bool overTextGlyph(POINT clientPos) const;
	bool overTextScrollBar(POINT clientPos) const;
	// 多文件贴屏：该点落在第几行（-1 = 不在任何一行上）
	int fileRowAt(POINT clientPos) const;
	// 文字里第 textIndex 个字符所在的链接（附带它在正文里的字符范围；不在链接上时 text 为空）
	struct LinkHit {
		std::wstring text;
		int start{ 0 };
		int end{ 0 };
	};
	LinkHit linkHitAt(int textIndex) const;
	// 文字贴屏：悬停到链接上时把它标蓝（离开 / 移到普通文字上则恢复）
	void updateLinkHover(POINT clientPos);
	// 打开之后隔一小会儿看前台窗口是谁（默认程序可能起得慢，多试几次）
	void onOpenWatchTimer();

	// —— 右键菜单 ——
	enum class Kind { Text, Link, Color, Image, Video, Folder, File };
	Kind contentKind() const;
	std::wstring copyLabel() const;
	void copyToClipboard() const;
	void saveAs();
	void showContextMenu(POINT screenPt);
	void setTopMost(bool on);
	void setBorderVisible(bool on);
	// 停悬控件：左上锁定 / 右上另存为、关闭（与贴图的停悬控件同款）
	void updateHoverChrome(bool inside);
	void refreshHoverChrome();

	// —— 自绘拖边缩放（边掩码：1 左 / 2 右 / 4 上 / 8 下）——
	int edgeHit(POINT clientPos) const;
	void beginResize(POINT screenPt, int edges);
	void updateResize();
	void endResize();

private:
	bool isFiles_{ false };
	std::wstring text_;
	std::vector<std::wstring> files_;
	Ling::Node* card_{ nullptr };
	Ling::TextBox* textBox_{ nullptr };   // 文字贴屏的正文（文件贴屏为空）
	Ling::Button* btnLock_{ nullptr };
	Ling::Button* btnSave_{ nullptr };
	Ling::Button* btnClose_{ nullptr };
	bool locked_{ false };
	bool hoverShown_{ false };
	bool canSave_{ true };
	// 拖动
	bool dragging_{ false };
	POINT dragStartScreen_{};
	POINT dragStartWin_{};
	// 拖边缩放
	bool resizing_{ false };
	int resizeEdges_{ 0 };
	POINT resizeStartScreen_{};
	int resizeStartX_{ 0 }, resizeStartY_{ 0 };
	float resizeStartW_{ 0.f }, resizeStartH_{ 0.f };
	bool topMost_{ true };
	bool borderVisible_{ true };
	// 双击 = 打开（图片 / 视频 / 文件夹 / 文件）
	ULONGLONG lastDownTick_{ 0 };
	POINT lastDownPos_{};
	// 多文件贴屏的每一行（双击某一行打开对应文件）
	std::vector<Ling::Node*> fileRows_;
	// 每个文件上次被打开后接住它的窗口（与 files_ 一一对应；0 = 还没打开过 / 已失效）
	std::vector<HWND> openedWnds_;
	int pendingOpenIndex_{ -1 };   // 正在等"打开后前台窗口"的文件下标
	int openWatchTries_{ 0 };      // 前台窗口还没变时的重试次数
	// 文件贴屏：本次按下落在第几个文件上（-1 = 空白处），松开时判断是不是"单击"
	int pressFileIndex_{ -1 };
	// 文字贴屏：本次按下是否落在文字上，以及按下位置（松开时判断是不是"单击链接"）
	bool pressOnText_{ false };
	POINT pressPos_{};
	// 当前悬停的链接范围（0/0 = 没有）
	int hoverLinkStart_{ 0 }, hoverLinkEnd_{ 0 };
	// 链接打开去抖：双击不要连开两次
	ULONGLONG lastLinkOpenTick_{ 0 };
	POINT lastLinkOpenPos_{};
	// 另存为对话框收尾期间吞掉 Esc：取消对话框的按键会落到贴屏上，导致贴屏被误关
	ULONGLONG suppressEscUntil_{ 0 };
	// 对话框关闭后的短暂窗口：系统可能把取消点击回放到贴屏上，这段内的按下直接忽略
	ULONGLONG suppressMouseUntil_{ 0 };
};
