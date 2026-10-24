#pragma once
#include <include/Ling.h>
#include <filesystem>
#include <functional>
#include <winrt/Windows.Data.Json.h>
#include "Ocr/CloudOcr.h"
using namespace winrt::Windows::Data::Json;

class Setting
{
public:
	~Setting();
	static void init();
	static void dispose();
	static Setting* get();
	std::filesystem::path getDataPath();
	// 插件（OCR / 翻译模型等）根目录。留空 = 自动：exe 同目录下已有 plugin 文件夹就用它
	// （绿色包 / 不想占用 C 盘），否则退回 %AppData%\SnowAir\plugin。见 Ocr/PluginPaths.h。
	std::wstring getPluginDir();
	void setPluginDir(const std::wstring& dir);
	const JsonObject getConfigObj();
	void setShortcutKey(const std::wstring& type, const std::vector<std::wstring>& keys);
	std::wstring getShortcutKey(const std::wstring& type);
	// 「全局热键」页的「恢复默认」：清掉用户设过的键位（清空后 getShortcutKey 逐条回落内置默认表），
	// 并按新键位整体重注册
	void restoreShortcutKeyDefaults();
	// 全局热键是否仍是出厂键位（没有任何覆盖项）
	bool shortcutKeyAtDefaults();
	// 全局热键「重复截图」：记住的上一块选区（屏幕坐标）。无记录返回 false。
	bool getLastCaptureRect(int& x, int& y, int& w, int& h);
	void setLastCaptureRect(int x, int y, int w, int h);
	void setAutoStart(bool autoStart);
	bool getAutoStart();
	// 启动时调用：开了「开机自启」就确保登记项指向当前这个 exe。换了安装目录 / 升级更换版本后，
	// 旧的自启（Run 项或计划任务）还指着老 exe，这里按当前 exe 重登记一次把它接管过来。
	void takeOverAutoStart();
	bool getRunAsAdmin();
	void setRunAsAdmin(bool on);
	std::wstring getLang();
	void setLang(const std::wstring& lang);
	void initShortcutKeys();
	// 录制热键期间挂起/恢复全局热键：挂起后系统热键与低级键盘钩子都不再拦截按键，
	// 否则用户在设置页录入热键时，按键会先被全局热键消费而无法录入。
	void suspendShortcutKeys();
	void resumeShortcutKeys();
	void purgeRemovedAiSettings();
	// 启动时清理旧程序 ScreenCapture 留下的历史键位（`cap`，现用 id 为 capture）；见 Setting.cpp
	void purgeLegacyShortcutKeys();
	bool getToolFlag(const std::wstring& tool, const std::wstring& key, bool def);
	void setToolFlag(const std::wstring& tool, const std::wstring& key, bool val);
	float getToolNum(const std::wstring& tool, const std::wstring& key, float def);
	void setToolNum(const std::wstring& tool, const std::wstring& key, float val);
	UINT32 getToolUInt(const std::wstring& tool, const std::wstring& key, UINT32 def);
	void setToolUInt(const std::wstring& tool, const std::wstring& key, UINT32 val);
	std::wstring getToolStr(const std::wstring& tool, const std::wstring& key, const std::wstring& def);
	void setToolStr(const std::wstring& tool, const std::wstring& key, const std::wstring& val);
	// 删除 toolPin/<tool>/<key>（键不存在时什么都不做）。用于「恢复默认」时把覆盖项清掉。
	void removeToolKey(const std::wstring& tool, const std::wstring& key);
	long long getUpdateCheckDay();
	void setUpdateCheckDay(long long day);

	int getOcrEngine();
	void setOcrEngine(int v);
	// OCR 结果版式：0 = 原位覆盖（默认），1 = 文本重排（整段可编辑文本框）
	int getOcrResultMode();
	void setOcrResultMode(int v);
	std::wstring getOcrCloudConfigId();
	void setOcrCloudConfigId(const std::wstring& id);
	std::vector<OcrCloudConfig> getOcrCloudConfigs();
	void setOcrCloudConfigs(const std::vector<OcrCloudConfig>& list);
	OcrCloudConfig getOcrCloudConfig();
	void setOcrCloudConfig(const OcrCloudConfig& cfg);

	std::wstring getTranslateProvider();
	void setTranslateProvider(const std::wstring& v);
	std::wstring getTranslateSourceLang();
	void setTranslateSourceLang(const std::wstring& v);
	std::wstring getTranslateTargetLang();
	void setTranslateTargetLang(const std::wstring& v);
	bool getTranslateDictMode();
	void setTranslateDictMode(bool on);
	// 「功能提示」总开关：开则在设置项旁显示说明问号（默认开）
	bool getFeatureTips();
	void setFeatureTips(bool on);
	// 「快捷键提示」：工具栏停悬文字后追加当前键位（"矩形" → "矩形（1）"）。默认关闭
	bool getHotkeyTip();
	void setHotkeyTip(bool on);

	bool getFindWindowElements();
	void setFindWindowElements(bool on);
	int getTrayClickAction();
	void setTrayClickAction(int v);
	bool getPinZoomAtCursor();
	void setPinZoomAtCursor(bool on);
	bool getPinAutoOcr();
	void setPinAutoOcr(bool on);
	bool getPinAutoFit();
	void setPinAutoFit(bool on);
	int getPinDoubleClickAction();
	void setPinDoubleClickAction(int v);
	int getPinOpacity();
	void setPinOpacity(int v);
	int getDemoDefaultTool();
	void setDemoDefaultTool(int v);
	int getDemoEdgeBehavior();
	void setDemoEdgeBehavior(int v);
	// 线迹标注类工具（矩形/箭头/画笔/高亮）的默认宽度，1–10
	int getToolDefaultWidth();
	void setToolDefaultWidth(int v);
	// 记忆工具样式：记住上次选中的标注工具样式（如箭头样式），不记忆滑动条
	bool getRememberToolStyle();
	void setRememberToolStyle(bool on);
	// 悬停预览：历史记录 / 剪切板卡片停悬时弹出预览（默认关闭）
	bool getHoverPreview();
	void setHoverPreview(bool on);
	int getLastArrowHead();
	void setLastArrowHead(int v);
	bool getAutoSaveAfterCapture();
	void setAutoSaveAfterCapture(bool on);
	std::wstring getSaveFormat();
	void setSaveFormat(const std::wstring& v);
	bool getCopyAsFile();
	void setCopyAsFile(bool on);
	bool getCancelConfirmDialog();
	void setCancelConfirmDialog(bool on);
	void restoreFunctionScreenshotDefaults();
	// 「功能设置」各子页的「恢复默认」：逐项写回该子页的出厂值（清掉用户覆盖项）
	void restoreFunctionRecordDefaults();     // 视频录制
	void restoreFunctionOutputDefaults();     // 文件输出
	void restoreFunctionOcrDefaults();        // 文本识别
	void restoreFunctionTranslateDefaults(); // 翻译
	// 与上面 5 个 restore 成对：该子页是否仍是出厂值（供「恢复默认」按钮置灰）
	bool functionScreenshotAtDefaults();
	bool functionRecordAtDefaults();
	bool functionOutputAtDefaults();
	bool functionOcrAtDefaults();
	bool functionTranslateAtDefaults();

	int getVideoQuality();
	void setVideoQuality(int v);
	int getVideoFps();
	void setVideoFps(int v);
	int getGifQuality();
	void setGifQuality(int v);
	int getGifFps();
	void setGifFps(int v);
	int getGifFormat();
	void setGifFormat(int v);
	int getRecordMic();
	void setRecordMic(int v);
	int getVideoEncoder();
	void setVideoEncoder(int v);
	int getEncodeSpeed();
	void setEncodeSpeed(int v);
	bool getHwAccel();
	void setHwAccel(bool on);
	bool getHideToolbarInRecord();
	void setHideToolbarInRecord(bool on);
	bool getShowKeystrokes();
	void setShowKeystrokes(bool on);
	// 「选框鼠标跟随」（视频录制）：录制中处于穿透态时，光标贴近选区边缘就让选区自动平移跟随
	bool getSelFollowMouse();
	void setSelFollowMouse(bool on);
	// 录屏工具栏「贴边隐藏」：手动拖到屏幕上/下边缘后自动缩进边缘（见 ToolVideo）
	bool getRecordToolbarPeek();
	void setRecordToolbarPeek(bool on);
	// 录屏默认开启鼠标穿透：开录即穿透，录制中可用快捷键/工具条按钮临时开关
	bool getRecordDefaultPierce();
	void setRecordDefaultPierce(bool on);
	// 「鼠标高亮提醒」（视频录制）：录制中在光标下垫一个半透明圆形色块
	bool getMouseHighlight();
	void setMouseHighlight(bool on);
	// 高亮圆的颜色（0xRRGGBBAA，取色存不透明值；绘制时按固定透明度做半透明色块）
	UINT32 getMouseHighlightColor();
	void setMouseHighlightColor(UINT32 rgba);

	std::wstring getManualSaveFormat();
	void setManualSaveFormat(const std::wstring& v);
	std::wstring getAutoSaveFormat();
	void setAutoSaveFormat(const std::wstring& v);
	std::wstring getVideoRecordFormat();
	void setVideoRecordFormat(const std::wstring& v);
	// 文件输出目录：空串表示用默认目录（图片\SnowAir）
	std::wstring getScreenshotDir();
	void setScreenshotDir(const std::wstring& v);
	std::wstring getRecordDir();
	void setRecordDir(const std::wstring& v);

	static std::wstring defaultManualSaveFormat();
	static std::wstring defaultAutoSaveFormat();
	static std::wstring defaultVideoRecordFormat();
	int getHistoryRetentionDays();
	void setHistoryRetentionDays(int days);
	// 「停用截图历史」：开启后设置中心不再显示「截图历史」菜单（不影响已生成的文件与索引）
	bool getDisableCaptureHistory();
	void setDisableCaptureHistory(bool on);
	int getAppTheme();
	void setAppTheme(int v);

	// —— 新增 (贴图边框 / 浮动按钮 / 窗口黑名单) ——
	bool getPinBorderDefaultEnabled();
	void setPinBorderDefaultEnabled(bool on);
	int getPinBorderWidth();
	void setPinBorderWidth(int v);
	int getPinBorderRadius();
	void setPinBorderRadius(int v);
	UINT32 getPinBorderColor();
	void setPinBorderColor(UINT32 argb);

	// —— 托盘图标（开关 + 样式 + 自定义路径）——
	bool getTrayEnabled();
	void setTrayEnabled(bool on);
	int getTrayIconStyle();               // 0 跟随系统 / 1 主题 / 2 自定义颜色 / 3 自定义图标
	void setTrayIconStyle(int v);
	UINT32 getTrayCustomColor();          // 「自定义颜色」的字形颜色（RGBA）
	void setTrayCustomColor(UINT32 rgba);
	std::wstring getTrayCustomIconPath();
	void setTrayCustomIconPath(const std::wstring& path);

	bool getFloatingCaptureButtonShow();
	void setFloatingCaptureButtonShow(bool on);
	int getFloatingCaptureButtonClick();   // 0 截图/1 录屏/2 快捷翻译/3 无操作
	void setFloatingCaptureButtonClick(int v);
	int getFloatingLongPressMs();
	void setFloatingLongPressMs(int ms);

	std::vector<std::wstring> getWindowBlacklist();
	void setWindowBlacklist(const std::vector<std::wstring>& titles);

	// —— 历史：只索引真实存在的输出文件，不复制内容 ——
	struct HistoryItem {
		std::wstring id;
		// 真实文件路径（截图/长图/静默截图在截图目录，录屏在用户保存的位置）
		std::wstring filePath;
		// "fullscreen" / "focused" / "capture" / "long" / "record" / "clipboard"
		std::wstring source;
		int width{ 0 };
		int height{ 0 };
		long long createdAt{ 0 };  // ms epoch
		// 仅剪切板条目的界面数据（不落盘）
		std::wstring text;
		std::vector<std::wstring> files;
	};
	// 录屏封面缩略图缓存目录（只存小图，不是历史副本）
	std::filesystem::path getHistoryThumbDir();
	// 索引里的条目（只含登记过的真实文件）
	std::vector<HistoryItem> getHistoryItems();
	// 历史页用：索引 + 扫描截图/录屏目录里已有的文件（可见即所得）
	std::vector<HistoryItem> getHistoryItemsForUi(bool forceRescan = false);
	// 登记一条已落盘的历史文件；文件不存在则不登记。返回条目 id
	std::wstring addHistoryFile(const std::wstring& filePath, int width, int height,
		const std::wstring& source);
	// 录屏：登记视频路径，coverBgra 只写进缩略图缓存当网格缩略图
	std::wstring addHistoryMedia(const std::wstring& mediaPath, int width, int height,
		const std::wstring& source, const std::vector<BYTE>& coverBgra = {});
	// 自定义标签：按真实文件路径存（目录扫描出来、未登记的条目也能打标签）
	std::vector<std::wstring> getHistoryTags(const std::wstring& filePath);
	void setHistoryTags(const std::wstring& filePath, const std::vector<std::wstring>& tags);
	// 所有可用标签（去重 + 字典序，只保留文件仍存在的条目）
	std::vector<std::wstring> getAllHistoryTags();
	void pruneExpiredHistory();
	// deleteFile=false：只删历史记录/索引，不动磁盘上的文件（设置页单条删除用）；
	// 目录扫描出来的用户文件没有索引，改为记入隐藏列表（同样不删文件）。
	void removeHistoryItem(const std::wstring& id, bool deleteFile = true);
	// sourceTab：0 全部 / 1 只清截图 / 2 只清录屏（与历史页导航一致）
	void clearAllHistory(int sourceTab = 0);
	// 「其他」：只清目录里用户自己放进去的图片/视频（移入回收站），不碰索引里的软件产物
	void clearOtherFiles();

	// —— 剪切板历史 ——
	// 只记录文本与图片；图片落盘到 dataPath\Clipboard，文本直接存索引（不限条数）
	struct ClipboardItem {
		std::wstring id;
		std::wstring type;      // "text" / "image" / "files"
		std::wstring text;      // 文本内容（type=text）
		std::wstring filePath;  // 位图落盘文件（type=image）
		std::vector<std::wstring> files;   // 文件列表（type=files，可能多个；只记路径）
		int width{ 0 };
		int height{ 0 };
		long long createdAt{ 0 };
		unsigned hash{ 0 };     // 去重用（图片像素 / 文件路径串的 FNV-1a）
		std::vector<std::wstring> tags;   // 自定义标签（筛选「全部」时可把剪切板条目筛出来）
	};
	std::filesystem::path getClipboardDir();
	std::vector<ClipboardItem> getClipboardItems();
	std::vector<std::wstring> getClipboardTags(const std::wstring& id);
	void setClipboardTags(const std::wstring& id, const std::vector<std::wstring>& tags);
	void addClipboardText(const std::wstring& text);
	void addClipboardImage(int w, int h, const std::vector<BYTE>& bgra);
	// 文件列表（资源管理器 / Word / PPT 等复制的文件）：只记路径，可以一次多个
	void addClipboardFiles(const std::vector<std::wstring>& files);
	void removeClipboardItem(const std::wstring& id);
	// 复制历史里的剪切板条目：把该条挪到最前面（更新时间），不生成重复条目。返回是否真的挪动了
	bool touchClipboardItem(const std::wstring& id);
	void clearClipboardHistory();
	// 本软件自己往剪切板写入内容后调用：记住当前序列号，监听端不再把这次写入记成新条目
	void markOwnClipboardWrite();
	// 剪切板历史变化通知：设置页停在「剪切板」页时据此即时刷新列表
	std::function<void()> clipboardChanged;
private:
	Setting();
	// 剪切板索引写回（含上限裁剪）：新条目在前
	void writeClipboardItems(std::vector<ClipboardItem>& items);
	// 历史页目录扫描缓存：鼠标移动/滚动会频繁取列表，不能每次都遍历目录
	std::vector<HistoryItem> historyUiCache_;
	unsigned long long historyUiCacheAt_{ 0 };
	bool historyUiCacheDirty_{ true };
	// —— 全局热键 ——
	// 注册/重注册全部全局热键：先走系统 RegisterHotKey；注册失败则退到低级键盘钩子兜底
	void applyHotkeys();
	// 执行一条全局热键的动作（id 取自 Setting.cpp 的 kHotkeys）
	void runHotkeyAction(const std::wstring& id);
	// 注册失败时的低频重试（一旦 RegisterHotKey 成功就摘掉钩子），由消息泵上的定时器驱动
	static void CALLBACK onHotkeyRetry(HWND, UINT, UINT_PTR id, DWORD);
	void startHotkeyRetry();
	void stopHotkeyRetry();
	JsonObject featuresObj();
	JsonObject getToolObj(const std::wstring& tool);
	std::filesystem::path initDataPath();
	std::filesystem::path initConfigPath();
	void save();
	bool featBool(const wchar_t* key, bool def);
	void setFeatBool(const wchar_t* key, bool v);
	int featInt(const wchar_t* key, int def);
	void setFeatInt(const wchar_t* key, int v);
	std::wstring featStr(const wchar_t* key, const wchar_t* def);
	void setFeatStr(const wchar_t* key, const std::wstring& v);
	JsonArray featArray(const wchar_t* key);
	void setFeatArray(const wchar_t* key, const JsonArray& arr);
private:
	const std::filesystem::path dataPath;
	const std::filesystem::path configPath;
	JsonObject configObj;
	UINT_PTR hotkeyRetryTimer{ 0 };
	int hotkeyRetryTick{ 0 };
};
