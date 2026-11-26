#pragma once
#include <include/Ling.h>
#include "PinShake.h"
#include "PinMenu.h"      // 右键菜单项类型（appendWindowMenu 的签名要用）
#include <string>
#include <utility>
#include <vector>

// 剪切板内容的贴屏窗：
//   · 文字：默认 350×150，内容多时自动长到 350×350，再超出就滚动；可拖边放大，文字可选中复制
//   · 文件：图标 + 文件名，默认 350×75；双击用系统默认程序打开
//
// 多文件 / 混合内容（图片 + 文件夹 + 视频 + 其它）逐行列出：
//   左：36×36 槽位 —— 图片是真缩略图，其余是系统「文件类型图标」（取不到时退回图标字体）；
//   右：两行 —— 第一行文件名，第二行尺寸 / 时长 / 大小 / 子项数（拿得到的才写）。
//   鼠标停在某一行：整行浮出圆角底色，右侧浮出按钮 —— 图片是「贴图」（单独贴成一张贴图），
//   其余是文件夹按钮（在资源管理器里打开所在位置）。缩略图单击复制该条、按住可拖出去。
class ClipPin : public Ling::WinBase
{
public:
	static void showText(const std::wstring& text);
	static void showFiles(const std::vector<std::wstring>& files);
	/* 指定左上角落点；批量贴屏 / 晾衣架夹子用，避免贴到光标下方。 */
	static void showTextAt(const std::wstring& text, POINT topLeft);
	static void showFilesAt(const std::vector<std::wstring>& files, POINT topLeft);
	// 「隐藏/显示贴图」热键：贴屏卡片与钉图属于同一组，必须一起隐藏 / 放回
	static bool hasAny();
	static void hideAll();
	static void showAll();
	// 「软件外观 > 贴图边框」改了颜色 / 粗细 / 圆角：已经贴着的卡片立即跟着重画
	static void refreshBorderAppearance();
	// 同一份内容（文字 / 文件列表）已经贴在屏幕上：不新建，抬到最前并抖动提示
	void pulseDuplicate();
	~ClipPin();
private:
	ClipPin(bool isFiles, std::wstring text, std::vector<std::wstring> paths);
	// 列表里的一行：行节点 + 悬停才浮出的按钮 + 可以拖出去的区域（缩略图 / 色块）
	struct Row
	{
		Ling::Node* node{ nullptr };
		Ling::Button* btn{ nullptr };
		Ling::Node* dragArea{ nullptr };      // 缩略图 / 系统图标 / 色块：从这里按下可拖到别的软件
		Ling::Label* hexLabel{ nullptr };     // 色值行：HEX 那一行（点它复制 HEX）
		Ling::Label* rgbLabel{ nullptr };     // 色值行：RGB 那一行（点它复制 RGB）
		int colorIndex{ -1 };                 // >=0 = 色值行，值是 colorValues_ 的下标
	};
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
	// 两个文件列表是不是同一组（Windows 路径大小写不敏感，顺序也不参与判定）
	static bool sameFileList(const std::vector<std::wstring>& a, const std::vector<std::wstring>& b);
	// 文字贴屏：该点是否落在实际文字（字形）上 / 文本框自己的滚动条上
	bool overTextGlyph(POINT clientPos) const;
	bool overTextScrollBar(POINT clientPos) const;
	// 多文件贴屏：该点落在第几行（-1 = 不在任何一行上）
	int rowAt(POINT clientPos) const;
	// 该点是否落在某一行的悬停按钮上（按钮只在停悬时可见，隐藏时命中测试自然失败）
	bool hitRowButton(POINT clientPos) const;
	// 停悬行切换：旧行收底色 / 收按钮，新行浮出（锁定态一律不浮出）
	void updateRowHover(POINT clientPos);
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

	// —— 多文件列表的构建与条目操作 ——
	void buildFileList(Ling::Node* card);
	// 色值贴屏：一行两个色值，每个是色块 + HEX / RGB 两行；超过可视高度的部分滚动
	void buildColorRows(Ling::Node* card);
	// 纯文本进剪贴板（色值的 HEX / RGB / 原始文字都走这里）
	void copyPlainText(const std::wstring& s) const;
	// 复制成功的即时反馈：被点的那行文字临时变绿，几百毫秒后复原
	void flashCopiedColor(int rowIndex, bool hex, bool rgb);
	void endColorFlash();
	// 色值行的右键菜单：复制这一条色值 + 窗口级项
	void showColorRowMenu(int rowIndex, POINT screenPt);
	// 从缩略图 / 色块拖出去：文件走 CF_HDROP（等同从资源管理器拖），色值走文字
	void beginItemDrag(int rowIndex);
	// 单击缩略图 / 色块 = 复制这一条（与晾衣架的交互一致）；不计入剪切板历史
	void copyRowItem(int rowIndex);
	// 把第 rowIndex 条放进剪贴板：色值 → 原始文字，文件 / 文件夹 → CF_HDROP，图片默认给位图。
	// imageAsFile 为 true 时图片也给文件（拖动要的是"从资源管理器拖这个文件"的效果）。
	bool putRowOnClipboard(int rowIndex, bool imageAsFile);
	// 名字按可用宽度截断并加省略号（Label 会折行，先截好保证一行放得下）
	std::wstring ellipsize(const std::wstring& text, float maxLogicalW) const;
	// 图片条目「贴图」：解码后按普通贴图贴到光标所在显示器的工作区中央
	void pinImageAt(size_t index);
	// 文件夹按钮：文件夹打开它自己，其它文件在资源管理器里定位（选中该文件）
	void openContainerAt(size_t index);
	void copyRowToClipboard(size_t index) const;
	// 图片另存为（可换格式，按图片格式编码）；文件另存为（原样复制一份）
	void saveImageAs(size_t index);
	void saveFileAsAt(size_t index);
	// 单个条目的右键菜单（打开 / 复制 / 另存为 + 窗口级：置顶 / 显示边框 / 锁定 / 关闭）
	void showRowMenu(size_t index, POINT screenPt);
	// 菜单尾巴（窗口级）：窗口置顶 / 显示边框 ── 锁定贴图 / 关闭贴图。
	// 空白区域右键只有这一组；单文件、条目右键在这组前面加内容项。
	void appendWindowMenu(std::vector<PinMenu::Item>& items);
	// 菜单里的「锁定贴图」与左上角那颗锁按钮是同一个状态
	void toggleLocked();
	// 系统对话框会抢走鼠标捕获：弹对话框前先把拖拽 / 缩放 / 按下状态复位
	void resetInteraction();

	// —— 右键菜单 ——
	enum class Kind { Text, Link, Color, Image, Video, Folder, File };
	Kind contentKind() const;
	std::wstring copyLabel() const;
	void copyToClipboard() const;
	void saveAs();
	void showContextMenu(POINT screenPt);
	void setTopMost(bool on);
	void setBorderVisible(bool on);
	// 按「显示边框 + 软件外观的贴图边框」重刷卡片外框：关掉时回到默认主题圆角、不描边
	void applyBorder();
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
	Ling::TextBox* textBox_{ nullptr };   // 文字贴屏的正文（文件贴屏 / 色值贴屏为空）
	// —— 色值贴屏 ——
	bool isColorCard_{ false };           // 正文全是色值：走"每个色值一行"的色块列表
	// 每一条：（贴进来时的原始文字, 0xRRGGBB）
	std::vector<std::pair<std::wstring, uint32_t>> colorValues_;
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
	// 贴屏卡片默认不描边（多个文件 / 文字这类"非贴图"内容）；打开「显示边框」才按
	// 软件外观里的「贴图边框」样式画（颜色 / 粗细 / 圆角都走那一套）
	bool borderVisible_{ false };
	// 双击 = 打开（图片 / 视频 / 文件夹 / 文件）
	ULONGLONG lastDownTick_{ 0 };
	POINT lastDownPos_{};
	// 多文件贴屏的每一行（双击 / 右键 / 停悬按钮都按行命中）
	std::vector<Row> rows_;
	int hoverRow_{ -1 };
	// 每个文件上次被打开后接住它的窗口（与 files_ 一一对应；0 = 还没打开过 / 已失效）
	std::vector<HWND> openedWnds_;
	int pendingOpenIndex_{ -1 };   // 正在等"打开后前台窗口"的文件下标
	int openWatchTries_{ 0 };      // 前台窗口还没变时的重试次数
	// 文件贴屏：本次按下落在第几个文件上（-1 = 空白处），松开时判断是不是"单击"
	int pressFileIndex_{ -1 };
	// 缩略图 / 色块上按下后还没拖动：超过系统拖拽阈值就从这里拖出去
	int pendingDragRow_{ -1 };
	POINT pendingDragStart_{};
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
	// —— 重复内容提示 ——
	PinShake::State shakeState_;
};
