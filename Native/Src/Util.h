#pragma once
#include <include/Ling.h>
#include <fstream>
#include <array>
#include <filesystem>
#include <string>
#include <utility>

// 图像输出相关的工具函数。data 一律要求 BGRA、top-down、行紧凑（步长 = w*4），
// 这也是 WinPin::getImagePixels 交出来的格式。
class Util
{
public:
	// 同时写入 CF_DIBV5（Office / 微信 / WPS 这类原生程序认）和 "PNG" 注册格式
	//（浏览器 / Electron 程序认），两份都带 alpha
	static void saveToClipboard(const int w, const int h, BYTE* data);
	static bool saveToFile(const std::wstring& path, const int w, const int h, BYTE* data);
	// 弹系统另存为对话框，返回空串表示用户取消。defaultName 非空时作为默认文件名（不含扩展名），
	// defaultDir 非空时作为对话框的初始目录（每次都强制从这里开始，不留用"上次保存过的目录"）
	static std::wstring getSaveFilePath(HWND hwnd, const std::wstring& ext = L"png",
		const std::wstring& defaultName = L"", const std::wstring& defaultDir = L"");
	// 以当前时间生成默认文件名，精确到毫秒，避免连续保存时重名
	static std::wstring createFileName(const std::wstring& ext);
	// GDI 抓屏。返回 BGRA、top-down、行紧凑（步长 = w*4），与本类其他函数的入参格式一致
	static std::vector<BYTE> captureScreen(const int x, const int y, const int w, const int h);
	// BGRA（自上而下、行紧凑）双线性缩放到目标尺寸；OCR 前放大/加边用
	static std::vector<BYTE> scaleBGRA(const BYTE* src, int sw, int sh, int dw, int dh);
	// 把文件路径以 CF_HDROP 写进剪切板，粘贴到资源管理器/聊天窗口就是一个文件
	static void addFileToClipboard(const std::wstring& filePath);
	// 多文件版本（剪切板历史里的「N 个文件」条目复制回剪切板）
	static void addFilesToClipboard(const std::vector<std::wstring>& paths);
	// WIC 从文件解码成 BGRA、top-down、行紧凑（步长 = w*4）；失败返回 {0,0,空}
	static std::tuple<int, int, std::vector<BYTE>> loadImagePixels(const std::wstring& path);
	// 只读图片头拿像素尺寸（不解码像素；失败返回 {0,0}）—— 停悬大图预览按宽高比定窗口用
	static std::pair<int, int> imageSize(const std::wstring& path);
	// 剪切板文本是不是一段 SVG 代码（<svg …>…</svg>）
	static bool isSvgText(const std::wstring& text);
	// 用 Direct2D 的 SVG 能力把代码渲染成 BGRA、top-down、行紧凑（步长 = w*4）；
	// 最长边不超过 maxDim。系统不支持 D2D SVG（Win10 1703 以前）或代码有误时返回 false
	static bool renderSvgText(const std::wstring& svg, int maxDim, int& w, int& h, std::vector<BYTE>& pixels);
	// —— History 操作 —— 根据条目 id 找到 Setting 的历史条目，然后执行对应动作
	static void copyHistoryFileToClipboard(const std::wstring& historyId);
	static void pinHistoryFile(const std::wstring& historyId);
	// 把图存成缓存文件，再交给外部插件 ImageReader.exe 做文字识别。插件先在本 exe
	// 同目录找，再找 %appdata%\SnowAir\plugin，都找不到就用默认浏览器打开它的
	// release 页面让用户自己下。缓存图由插件读完后自己删。
	static bool openWithImageReader(const int w, const int h, BYTE* data);
	// 用 ZXing 识别二维码与常见一维条码（Code128/EAN/UPC 等）。
	// 图里有多个码时用换行拼在一起
	static std::wstring decodeQrCode(const int w, const int h, BYTE* data);

	// —— 光标 ——
	// 当前光标图形的"实体"中心相对热点的偏移，单位物理像素。
	// 热点落在图形的一个像素上（标准箭头在左上角尖角），把圆形色块直接画在热点上会偏离鼠标本体，
	// 故按当前光标位图的非透明包围盒中心换算；取不到位图时返回 0（退回热点）。
	// 结果按光标句柄缓存，可高频调用。
	static void cursorBodyOffset(float& dx, float& dy);

	// 取当前 exe 自身的版本号（X.Y.Z）。从【已加载进内存的模块资源】里读，
	// 不走磁盘文件路径 —— 否则运行期间磁盘上的 exe 一旦被替换/改名/删除
	// （自升级拷贝、重新构建等），同一进程里先后两次调用就会拿到不同的值，
	// 表现为设置页左下角版本号对、而「关于」页（每次打开都重新读）对不上。
	static std::array<int, 3> getAppVersion();
	// GitHub Releases Atom feed 里提取最新 tag（如 v0.1.12-lite）。
	// Atom 不受未认证 API 的 60 次/小时限流影响。
	static std::wstring parseGithubReleaseTag(const std::wstring& atom);

	// —— 管理员 / 提权判定 ——
	// 当前登录账户是否为内置 Administrator（域内 SID 的 RID == 500）。该账户默认不受 UAC 过滤，
	// 进程天然就是管理员：自启直接写 Run 键即可，既不需要 runas 提权、也不需要计划任务。
	static bool isBuiltinAdminAccount();
	// 当前进程是否已处于管理员提权状态。
	static bool isElevated();
	// 用 runas 拉起一个提权的新实例（params 为附加命令行，不含 exe 路径）。用户取消 UAC 时返回 false。
	static bool spawnElevated(const std::wstring& params);

	// —— 截图输出路径 ——
	// 默认输出根目录：图片\SnowAir；取不到「图片」文件夹时退回空路径（不建目录）
	static std::filesystem::path defaultOutputDir();
	// 手动保存（工具栏「保存」）对话框的起始目录：桌面；取不到时退回空路径（对话框用系统默认位置）
	static std::filesystem::path desktopDir();
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
	// 录制收尾：把临时目录里的成片移到「录屏文件路径」（未设置时退回图片\SnowAir），
	// 按「视频录制文件名格式」命名、重名自动加序号。返回最终路径（空 = 失败，源文件原样保留）
	static std::wstring commitRecordedFile(const std::wstring& tempPath);
	// 当前前台窗口所属进程名（不含扩展名），本进程自己（截图覆盖窗等）返回空。
	// 供文件名模板里的 {{FOCUS_WINDOW_APP_NAME}} 用，需在覆盖窗出现前调用。
	static std::wstring foregroundAppName();
	// 删除文件到系统回收站（失败才退回永久删除），避免用户误删
	static bool moveToRecycleBin(const std::wstring& path);
	// 自动落盘（取消/退出截图用）：用「自动保存文件名格式」+「保存格式」写到截图目录
	static std::wstring saveAutoCapture(const std::vector<BYTE>& bgra, const int w, const int h,
		const std::wstring& focusAppName = L"");
	// 正常完成截图落盘：用「手动保存文件名格式」+「保存格式」写到截图目录
	static std::wstring saveCompletedCapture(const std::vector<BYTE>& bgra, const int w, const int h,
		const std::wstring& focusAppName = L"");
	// 用户主动退出截图时按「截图自动保存」开关决定是否落盘（不弹对话框、不进剪贴板）
	static std::wstring quietSaveCapture(const std::vector<BYTE>& bgra, const int w, const int h,
		const std::wstring& focusAppName = L"");
};
