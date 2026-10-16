#pragma once
#include <include/Ling.h>
#include <filesystem>
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
	// 否则用户在设置页录入热键时按键会先被全局热键吞掉而录不进去。
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
	std::wstring getFocusedWindowFormat();
	void setFocusedWindowFormat(const std::wstring& v);
	std::wstring getVideoRecordFormat();
	void setVideoRecordFormat(const std::wstring& v);
	// 文件输出目录：空串表示用默认目录（图片\SnowAir）
	std::wstring getScreenshotDir();
	void setScreenshotDir(const std::wstring& v);
	std::wstring getRecordDir();
	void setRecordDir(const std::wstring& v);

	static std::wstring defaultManualSaveFormat();
	static std::wstring defaultAutoSaveFormat();
	static std::wstring defaultFocusedWindowFormat();
	static std::wstring defaultVideoRecordFormat();
	int getHistoryRetentionDays();
	void setHistoryRetentionDays(int days);
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

	// —— History 目录/条目/过期清理 ——
	struct HistoryItem {
		std::wstring id;
		std::wstring filePath;
		std::wstring source;       // "fullscreen" / "focused" / "capture"
		int width{ 0 };
		int height{ 0 };
		long long createdAt{ 0 };  // ms epoch
	};
	std::filesystem::path getHistoryDir();
	std::vector<HistoryItem> getHistoryItems();
	// 记一条截图历史：图像写进历史目录（历史条目自带一份，与用户输出目录解耦，
	// 清空历史不会误删用户自己的截图），返回落盘文件路径（失败返回空）
	std::wstring addHistoryItem(const std::vector<BYTE>& bgra, int width, int height, const std::wstring& source);
	void pruneExpiredHistory();
	void removeHistoryItem(const std::wstring& id);
	void clearAllHistory();
private:
	Setting();
	// —— 全局热键 ——
	// 注册/重注册全部全局热键：先走系统 RegisterHotKey；被别的程序占了就退到低级键盘钩子兜底
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
