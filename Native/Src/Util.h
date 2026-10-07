#pragma once
#include <include/Ling.h>
#include <fstream>
#include <array>
#include <filesystem>
#include <string>

// 图像输出相关的工具函数。data 一律要求 BGRA、top-down、行紧凑（步长 = w*4），
// 这也是 WinPin::getImagePixels 交出来的格式。
class Util
{
public:
	// 同时写入 CF_DIBV5（Office / 微信 / WPS 这类原生程序认）和 "PNG" 注册格式
	//（浏览器 / Electron 程序认），两份都带 alpha
	static void saveToClipboard(const int w, const int h, BYTE* data);
	static bool saveToFile(const std::wstring& path, const int w, const int h, BYTE* data);
	// 弹系统另存为对话框，返回空串表示用户取消
	static std::wstring getSaveFilePath(HWND hwnd, const std::wstring& ext = L"png");
	// 以当前时间生成默认文件名，精确到毫秒，避免连续保存时重名
	static std::wstring createFileName(const std::wstring& ext);
	// GDI 抓屏。返回 BGRA、top-down、行紧凑（步长 = w*4），与本类其他函数的入参格式一致
	static std::vector<BYTE> captureScreen(const int x, const int y, const int w, const int h);
	// 把文件路径以 CF_HDROP 写进剪切板，粘贴到资源管理器/聊天窗口就是一个文件
	static void addFileToClipboard(const std::wstring& filePath);
	// —— History 操作 —— 根据条目 id 找到 Setting 的历史条目，然后执行对应动作
	static void copyHistoryFileToClipboard(const std::wstring& historyId);
	static void pinHistoryFile(const std::wstring& historyId);
	// 把图存成缓存文件，再交给外部插件 ImageReader.exe 做文字识别。插件先在本 exe
	// 同目录找，再找 %appdata%\SnowAir\plugin，都找不到就用默认浏览器打开它的
	// release 页面让用户自己下。缓存图由插件读完后自己删。
	static bool openWithImageReader(const int w, const int h, BYTE* data);
	// 用 ZXing 识别二维码与常见一维条码（Code128/EAN/UPC 等）；失败时退回 quirc 仅扫 QR。
	// 图里有多个码时用换行拼在一起
	static std::wstring decodeQrCode(const int w, const int h, BYTE* data);

	// —— 截图输出路径 ——
	// 默认输出根目录：图片\SnowAir；取不到「图片」文件夹时退回空路径（不建目录）
	static std::filesystem::path defaultOutputDir();
	// 截屏输出根目录：用户在「文件输出」自定义了截图目录就用它，否则用 defaultOutputDir()；不存在就建
	static std::filesystem::path outputDir();
	// 文件名模板 → 绝对路径。
	//   {{YYYY}}/{{MM}}/{{DD}}/{{HH}}/{{mm}}/{{ss}} 按当前时间展开，{{FOCUS_WINDOW_APP_NAME}} 换成传入的应用名；
	//   模板里出现的 / 或 \ 会被当成子目录（逐级建目录），非法字符替换成 '_'；
	//   format 为空时用 fallbackFormat；文件名没带扩展名时补上 ext。
	static std::wstring buildSavePath(const std::wstring& format, const std::wstring& fallbackFormat,
		const std::wstring& ext = L"png", const std::wstring& focusAppName = L"");
	// 文件名模板 → 展开后的示例名（只做占位符展开，**不建目录、不落盘**）。
	// 供「文件输出」格式输入框的悬停预览用。
	static std::wstring previewFileName(const std::wstring& format, const std::wstring& fallbackFormat,
		const std::wstring& focusAppName = L"");
	// 「保存格式」设置（如 "JPEG (*.jpg)"）→ 扩展名；认不出来一律 png
	static std::wstring extensionFromSaveFormat(const std::wstring& fmt);
};
