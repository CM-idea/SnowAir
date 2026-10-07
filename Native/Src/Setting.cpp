#include "pch.h"
#include <include/Ling.h>
#include <chrono>
#include "Setting.h"
#include "Util.h"
#include "Lang.h"
#include "Win/WinCap.h"
#include "Win/HotkeyHook.h"
#include "Win/WinSetting.h"
#include "App.h"
#include <algorithm>

namespace {
    std::unique_ptr<Setting> setting;
    constexpr int capShortcutMsgId{ 100 };
    constexpr UINT_PTR kCaptureRetryTimerId{ 0x5A18 };
    // 配置文件的默认内容。空文件、坏 JSON、缺键都拿它兜底，所以这里列出的每一项
    // 都是代码里会直接按名字取的（见 getLang / getAutoStart / initShortcutKeys）
    constexpr std::wstring_view defaultConfig{ LR"""({"common":{"autoStart":false,"runAsAdmin":false,"language":"zh-CN"},"shortcutKey":{"cap":"Ctrl+Alt+A"}})""" };

    // 静默执行一条命令行（不弹出控制台窗口），用于 schtasks 注册/删除管理员自启任务。
    void runQuiet(std::wstring cmd)
    {
        STARTUPINFOW si{ sizeof(si) };
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        std::wstring full = std::format(L"cmd.exe /c {}", cmd);
        std::vector<wchar_t> buf(full.begin(), full.end());
        buf.push_back(0);
        if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, INFINITE);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }
}


Setting::Setting() :dataPath{ initDataPath() }, configPath{ initConfigPath() }
{
    if (std::filesystem::exists(configPath)) {
        auto content = Ling::Util::readFileText(configPath);
        if (content.empty() || content.find_first_not_of(L" \t\r\n") == std::wstring::npos) {
            configObj = JsonObject::Parse(defaultConfig);
            save();
            return;
        }
        JsonObject obj{ nullptr };
        if (JsonObject::TryParse(content, obj)) {
            configObj = obj;
            return;
        }
        MessageBox(nullptr, L"config.json parse error，use default config", L"SnowAir", MB_OK | MB_ICONWARNING);
    }
    configObj = JsonObject::Parse(defaultConfig); 
}



Setting::~Setting()
{

}

void Setting::init()
{
    auto ptr = new Setting();
    setting.reset(ptr);
    // 启动时清理旧版本遗留的 AI 配置（AI 对话 / AI 视觉 OCR / AI 翻译）
    setting->purgeRemovedAiSettings();
}

void Setting::dispose()
{
    setting.reset();
}

Setting* Setting::get()
{
    return setting.get();
}

std::filesystem::path Setting::getDataPath()
{
    return dataPath; //复制一份路径对象，不允许就地修改
}

const JsonObject Setting::getConfigObj()
{
    return configObj;
}

void Setting::setShortcutKey(const std::wstring& type, const std::vector<std::wstring>& keys)
{
    std::wstring str;
    for (size_t i = 0; i < keys.size(); i++)
    {
        str += L"+" + keys[i];
    }
    str.erase(0,1);
    auto shortcutKey = configObj.GetNamedObject(L"shortcutKey");
    shortcutKey.SetNamedValue(type, JsonValue::CreateStringValue(str));
    save();
    // 目前只有「截图」这一项真的接了全局热键实现（其余各行先只落配置），
    // 所以这里只对 capture 重新注册 —— 旧代码不管设的是哪一行都去注册 cap 那个 id：
    // 在会话里"看起来生效了"，可配置写的是另一行，下次启动读 cap 又回到老组合。
    if (type == L"capture") applyCaptureHotkey(str);
}

std::wstring Setting::getShortcutKey(const std::wstring& type)
{
    // 一路用带默认值的重载：启动时 ensureDefaults 已经补齐过，这里只是别让运行期
    // 意外（配置被外部改动、问了个没配过的 type）变成一次崩溃
    auto obj = configObj.GetNamedObject(L"shortcutKey", nullptr);
    if (!obj) return L"";
    std::wstring val{ obj.GetNamedString(type, L"") };
    // 老配置里截图热键叫 cap，设置页写的是 capture：统一按 capture 走，读不到再回退老键名
    if (val.empty() && type == L"capture") val = std::wstring{ obj.GetNamedString(L"cap", L"") };
    return val;
}

// 旧版本写入的 AI 相关配置：AI 对话（模型 / 新建会话 / 对话 API 配置）、AI 视觉 OCR
// （视觉理解模型 / HTML、Markdown 提示词）、AI 翻译（翻译提示词 / 领域 / 自定义 API）。
// 这些功能已整体移除，启动时顺手从 config.json 里删掉——既不留无效键，也不把用户填过的
// API Key 之类敏感项长期留在磁盘上。只在确实删到了东西时才回写文件。
void Setting::purgeRemovedAiSettings()
{
    static const wchar_t* const kRemovedFeatKeys[] = {
        // AI 对话
        L"chatModel", L"chatNewSessionOnOpen", L"chatNewSessionOnClose", L"chatApiConfigs",
        // AI 视觉 OCR
        L"htmlVisionModel", L"ocrHtmlPrompt", L"ocrMarkdownPrompt",
        // AI 翻译
        L"translatePrompt", L"translateEnabledModels",
        L"translateApiProvider", L"translateApiUrl", L"translateApiKey", L"translateApiModel",
        L"translateApiAppId", L"translateApiSecret", L"translateApiName",
        L"translateDomain", L"translateDomainCustom",
    };
    bool changed = false;
    if (configObj.HasKey(L"features")) {
        auto features = configObj.GetNamedObject(L"features");
        for (auto* key : kRemovedFeatKeys) {
            if (features.HasKey(key)) {
                features.Remove(key);
                changed = true;
            }
        }
    }
    // 旧「AI 对话」历史（toolPin/chat）：快捷翻译改用独立键 quickTranslate/history，
    // 旧会话不再展示，直接清掉。
    auto toolRoot = configObj.GetNamedObject(L"toolPin", nullptr);
    if (toolRoot && toolRoot.HasKey(L"chat")) {
        toolRoot.Remove(L"chat");
        changed = true;
    }
    if (changed) save();
}

void Setting::setAutoStart(bool autoStart)
{
    std::wstring runKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    // 无论开关与否，先移除两种自启方式，避免残留旧的注册表项/计划任务。
    {
        HKEY hKey;
        if (RegOpenKeyEx(HKEY_CURRENT_USER, runKey.data(), 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
            RegDeleteValue(hKey, L"SnowAir");
            RegCloseKey(hKey);
        }
    }
    runQuiet(L"schtasks /Delete /TN \"SnowAir\" /F");

    if (autoStart) {
        wchar_t buffer[MAX_PATH];
        GetModuleFileName(nullptr, buffer, MAX_PATH);
        auto curPath = std::filesystem::path(buffer);
        std::wstring commandLine = std::format(L"\"{}\" --auto-start", curPath.wstring());

        if (getRunAsAdmin()) {
            // 以管理员身份运行：通过计划任务（最高权限）在登录时启动。
            runQuiet(std::format(
                L"schtasks /Create /TN \"SnowAir\" /SC ONLOGON /RL HIGHEST /F /TR \"{}\"", commandLine));
        }
        else {
            // 普通自启：写入当前用户注册表 Run 键。
            HKEY hKey;
            if (RegOpenKeyEx(HKEY_CURRENT_USER, runKey.data(), 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
                RegSetValueEx(hKey, L"SnowAir", 0, REG_SZ, (const BYTE*)commandLine.data(), (DWORD)((commandLine.size() + 1) * sizeof(wchar_t)));
                RegCloseKey(hKey);
            }
        }
    }
    auto common = configObj.GetNamedObject(L"common", nullptr);
    if (!common) {
        common = JsonObject();
        configObj.SetNamedValue(L"common", common);
    }
    common.SetNamedValue(L"autoStart", JsonValue::CreateBooleanValue(autoStart));
    save();
}

bool Setting::getAutoStart()
{
    auto common = configObj.GetNamedObject(L"common", nullptr);
    return common && common.GetNamedBoolean(L"autoStart", false);
}

bool Setting::getRunAsAdmin()
{
    auto common = configObj.GetNamedObject(L"common", nullptr);
    return common && common.GetNamedBoolean(L"runAsAdmin", false);
}

void Setting::setRunAsAdmin(bool on)
{
    auto common = configObj.GetNamedObject(L"common", nullptr);
    if (!common) {
        common = JsonObject();
        configObj.SetNamedValue(L"common", common);
    }
    common.SetNamedValue(L"runAsAdmin", JsonValue::CreateBooleanValue(on));
    save();
    // 关机自启若处于开启状态，按新的权限方式重新注册。
    if (getAutoStart()) setAutoStart(true);
}

std::filesystem::path Setting::initDataPath()
{
    PWSTR pathTmp;
    auto hr = SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &pathTmp);
    if (FAILED(hr)) {
        _ASSERT_EXPR(FALSE, L"get roaming path，error");
        return L"";
    }
    auto dataPath = std::filesystem::path{ pathTmp };
    CoTaskMemFree(pathTmp);
    dataPath.append("SnowAir");
    if (!std::filesystem::exists(dataPath)) {
        if (!std::filesystem::create_directories(dataPath)) {
            _ASSERT_EXPR(FALSE, L"create data path，error");
        }
    }
    return dataPath;
}

std::filesystem::path Setting::initConfigPath()
{
    // 与插件的查找顺序一致（见 Util.cpp 里的 findImageReader）：先看 exe 同目录。
    // 只有那份文件本来就存在时才认它 —— 不存在就不要在程序目录里新建，
    // 装在 Program Files 下时那儿通常没有写权限，况且默认位置该是 appdata
    wchar_t buffer[MAX_PATH]{};
    GetModuleFileName(nullptr, buffer, MAX_PATH);
    auto path = std::filesystem::path{ buffer }.parent_path().append(L"config.json");
    if (std::filesystem::exists(path)) return path;
    auto fallback = this->dataPath; //复制一份路径对象，append 会就地改
    return fallback.append(L"config.json");
}

void Setting::save()
{
    std::wstring str{ configObj.Stringify() };
    Ling::Util::saveFile(configPath.wstring(), str);
}

std::wstring Setting::getLang()
{
    auto common = configObj.GetNamedObject(L"common", nullptr);
    if (!common) return L"zh-CN";
    return std::wstring{ common.GetNamedString(L"language", L"zh-CN") };
}

void Setting::setLang(const std::wstring& langCode)
{
    auto common = setting->configObj.GetNamedObject(L"common", nullptr);
    if (!common) {
        common = JsonObject();
        setting->configObj.SetNamedValue(L"common", common);
    }
    common.SetNamedValue(L"language", JsonValue::CreateStringValue(langCode));
    setting->save();
	Lang::get()->initLang(langCode);
}

JsonObject Setting::getToolObj(const std::wstring& tool)
{
    // 用带默认值的重载：这两层在旧配置文件里都不存在，直接 GetNamedObject 会抛异常，
    // 值被手工改成非对象时它也一样返回默认值，不会炸
    auto root = configObj.GetNamedObject(L"toolPin", nullptr);
    if (!root) {
        root = JsonObject();
        configObj.SetNamedValue(L"toolPin", root);
    }
    auto obj = root.GetNamedObject(tool, nullptr);
    if (!obj) {
        obj = JsonObject();
        root.SetNamedValue(tool, obj);
    }
    return obj;
}

bool Setting::getToolFlag(const std::wstring& tool, const std::wstring& key, bool def)
{
    return getToolObj(tool).GetNamedBoolean(key, def);
}

void Setting::setToolFlag(const std::wstring& tool, const std::wstring& key, bool val)
{
    getToolObj(tool).SetNamedValue(key, JsonValue::CreateBooleanValue(val));
    save();
}

float Setting::getToolNum(const std::wstring& tool, const std::wstring& key, float def)
{
    return static_cast<float>(getToolObj(tool).GetNamedNumber(key, def));
}

void Setting::setToolNum(const std::wstring& tool, const std::wstring& key, float val)
{
    getToolObj(tool).SetNamedValue(key, JsonValue::CreateNumberValue(val));
    save();
}

UINT32 Setting::getToolUInt(const std::wstring& tool, const std::wstring& key, UINT32 def)
{
    auto obj = getToolObj(tool);
    if (!obj.HasKey(key)) return def;
    return static_cast<UINT32>(obj.GetNamedNumber(key));
}

void Setting::setToolUInt(const std::wstring& tool, const std::wstring& key, UINT32 val)
{
    getToolObj(tool).SetNamedValue(key, JsonValue::CreateNumberValue(static_cast<double>(val)));
    save();
}

std::wstring Setting::getToolStr(const std::wstring& tool, const std::wstring& key, const std::wstring& def)
{
    auto obj = getToolObj(tool);
    if (!obj.HasKey(key)) return def;
    return std::wstring{ obj.GetNamedString(key, def) };
}

void Setting::setToolStr(const std::wstring& tool, const std::wstring& key, const std::wstring& val)
{
    getToolObj(tool).SetNamedValue(key, JsonValue::CreateStringValue(val));
    save();
}

long long Setting::getUpdateCheckDay()
{
    auto common = configObj.GetNamedObject(L"common", nullptr);
    if (!common) return 0;
    return static_cast<long long>(common.GetNamedNumber(L"updateCheckDay", 0));
}

void Setting::setUpdateCheckDay(long long day)
{
    auto common = configObj.GetNamedObject(L"common", nullptr);
    if (!common) return;
    // 这项不写进 defaultConfig：它是程序自己的记账，不是给用户改的配置
    common.SetNamedValue(L"updateCheckDay", JsonValue::CreateNumberValue(static_cast<double>(day)));
    save();
}

JsonObject Setting::featuresObj()
{
	if (!configObj.HasKey(L"features")) {
		configObj.SetNamedValue(L"features", JsonObject{});
	}
	return configObj.GetNamedObject(L"features");
}

bool Setting::featBool(const wchar_t* key, bool def)
{
	return featuresObj().GetNamedBoolean(key, def);
}
void Setting::setFeatBool(const wchar_t* key, bool v)
{
	featuresObj().SetNamedValue(key, JsonValue::CreateBooleanValue(v));
	save();
}
int Setting::featInt(const wchar_t* key, int def)
{
	return (int)featuresObj().GetNamedNumber(key, def);
}
void Setting::setFeatInt(const wchar_t* key, int v)
{
	featuresObj().SetNamedValue(key, JsonValue::CreateNumberValue(v));
	save();
}
std::wstring Setting::featStr(const wchar_t* key, const wchar_t* def)
{
	return featuresObj().GetNamedString(key, def).c_str();
}
void Setting::setFeatStr(const wchar_t* key, const std::wstring& v)
{
	featuresObj().SetNamedValue(key, JsonValue::CreateStringValue(v));
	save();
}
JsonArray Setting::featArray(const wchar_t* key)
{
	auto f = featuresObj();
	if (!f.HasKey(key) || f.GetNamedValue(key).ValueType() != JsonValueType::Array)
		return JsonArray{};
	return f.GetNamedArray(key);
}
void Setting::setFeatArray(const wchar_t* key, const JsonArray& arr)
{
	featuresObj().SetNamedValue(key, arr);
	save();
}

int Setting::getOcrEngine() { return featInt(L"ocrEngine", 4); }
void Setting::setOcrEngine(int v) { setFeatInt(L"ocrEngine", v); }
std::wstring Setting::getOcrCloudConfigId() { return featStr(L"ocrCloudConfigId", L""); }
void Setting::setOcrCloudConfigId(const std::wstring& id) { setFeatStr(L"ocrCloudConfigId", id); }

static OcrCloudConfig cloudFromJson(const JsonObject& o)
{
	OcrCloudConfig c;
	c.id = o.GetNamedString(L"id", L"").c_str();
	c.provider = o.GetNamedString(L"provider", L"baidu").c_str();
	c.name = o.GetNamedString(L"name", L"").c_str();
	c.appId = o.GetNamedString(L"appId", L"").c_str();
	c.apiKey = o.GetNamedString(L"apiKey", L"").c_str();
	c.secret = o.GetNamedString(L"apiSecret", L"").c_str();
	c.endpoint = o.GetNamedString(L"endpoint", L"").c_str();
	return c;
}
static JsonObject cloudToJson(const OcrCloudConfig& c)
{
	JsonObject o;
	o.SetNamedValue(L"id", JsonValue::CreateStringValue(c.id));
	o.SetNamedValue(L"provider", JsonValue::CreateStringValue(c.provider));
	o.SetNamedValue(L"name", JsonValue::CreateStringValue(c.name));
	o.SetNamedValue(L"appId", JsonValue::CreateStringValue(c.appId));
	o.SetNamedValue(L"apiKey", JsonValue::CreateStringValue(c.apiKey));
	o.SetNamedValue(L"apiSecret", JsonValue::CreateStringValue(c.secret));
	o.SetNamedValue(L"endpoint", JsonValue::CreateStringValue(c.endpoint));
	return o;
}

std::vector<OcrCloudConfig> Setting::getOcrCloudConfigs()
{
	std::vector<OcrCloudConfig> list;
	for (auto const& v : featArray(L"ocrCloudConfigs")) {
		if (v.ValueType() != JsonValueType::Object) continue;
		auto c = cloudFromJson(v.GetObjectW());
		if (!c.id.empty()) list.push_back(c);
	}
	if (list.empty()) {
		OcrCloudConfig legacy;
		auto f = featuresObj();
		legacy.id = f.GetNamedString(L"ocrCloudId", L"").c_str();
		legacy.provider = f.GetNamedString(L"ocrCloudProvider", L"baidu").c_str();
		legacy.apiKey = f.GetNamedString(L"ocrCloudApiKey", L"").c_str();
		legacy.secret = f.GetNamedString(L"ocrCloudSecret", L"").c_str();
		legacy.endpoint = f.GetNamedString(L"ocrCloudEndpoint", L"").c_str();
		if (legacy.isConfigured()) {
			if (legacy.id.empty()) legacy.id = L"legacy";
			list.push_back(legacy);
		}
	}
	return list;
}
void Setting::setOcrCloudConfigs(const std::vector<OcrCloudConfig>& list)
{
	JsonArray arr;
	for (auto& c : list) arr.Append(cloudToJson(c));
	setFeatArray(L"ocrCloudConfigs", arr);
}
OcrCloudConfig Setting::getOcrCloudConfig()
{
	auto id = getOcrCloudConfigId();
	auto list = getOcrCloudConfigs();
	for (auto& c : list) if (c.id == id) return c;
	if (!list.empty()) return list.front();
	OcrCloudConfig cfg;
	auto f = featuresObj();
	cfg.id = f.GetNamedString(L"ocrCloudId", L"default").c_str();
	cfg.provider = f.GetNamedString(L"ocrCloudProvider", L"baidu").c_str();
	cfg.apiKey = f.GetNamedString(L"ocrCloudApiKey", L"").c_str();
	cfg.secret = f.GetNamedString(L"ocrCloudSecret", L"").c_str();
	cfg.endpoint = f.GetNamedString(L"ocrCloudEndpoint", L"").c_str();
	return cfg;
}
void Setting::setOcrCloudConfig(const OcrCloudConfig& cfg)
{
	auto list = getOcrCloudConfigs();
	bool found = false;
	for (auto& c : list) {
		if (c.id == cfg.id) { c = cfg; found = true; break; }
	}
	if (!found) list.push_back(cfg);
	setOcrCloudConfigs(list);
	setOcrCloudConfigId(cfg.id);
}

std::wstring Setting::getTranslateProvider() { return featStr(L"translateProvider", L"microsoft"); }
void Setting::setTranslateProvider(const std::wstring& v) { setFeatStr(L"translateProvider", v); }
std::wstring Setting::getTranslateSourceLang() { return featStr(L"translateSourceLang", L"auto"); }
void Setting::setTranslateSourceLang(const std::wstring& v) { setFeatStr(L"translateSourceLang", v); }
// 默认目标语言为英语（即默认中译英）
std::wstring Setting::getTranslateTargetLang() { return featStr(L"translateTargetLang", L"en"); }
void Setting::setTranslateTargetLang(const std::wstring& v) { setFeatStr(L"translateTargetLang", v); }
bool Setting::getTranslateDictMode() { return featBool(L"translateDictMode", false); }
void Setting::setTranslateDictMode(bool on) { setFeatBool(L"translateDictMode", on); }
bool Setting::getFeatureTips() { return featBool(L"featureTips", true); }
void Setting::setFeatureTips(bool on) { setFeatBool(L"featureTips", on); }

bool Setting::getFindWindowElements() { return featBool(L"findWindowElements", true); }
void Setting::setFindWindowElements(bool on) { setFeatBool(L"findWindowElements", on); }
int Setting::getTrayClickAction() { return featInt(L"trayClickAction", 0); }
void Setting::setTrayClickAction(int v) { setFeatInt(L"trayClickAction", v); }
bool Setting::getPinZoomAtCursor() { return featBool(L"pinZoomAtCursor", true); }
void Setting::setPinZoomAtCursor(bool on) { setFeatBool(L"pinZoomAtCursor", on); }
bool Setting::getPinAutoOcr() { return featBool(L"pinAutoOcr", false); }
void Setting::setPinAutoOcr(bool on) { setFeatBool(L"pinAutoOcr", on); }
bool Setting::getPinAutoFit() { return featBool(L"pinAutoFit", true); }
void Setting::setPinAutoFit(bool on) { setFeatBool(L"pinAutoFit", on); }
int Setting::getPinDoubleClickAction() { return featInt(L"pinDoubleClickAction", 0); }
void Setting::setPinDoubleClickAction(int v) { setFeatInt(L"pinDoubleClickAction", v); }
int Setting::getDemoDefaultTool() { return featInt(L"demoDefaultTool", 0); }
void Setting::setDemoDefaultTool(int v) { setFeatInt(L"demoDefaultTool", v); }
int Setting::getDemoEdgeBehavior() { return featInt(L"demoEdgeBehavior", 0); }
void Setting::setDemoEdgeBehavior(int v) { setFeatInt(L"demoEdgeBehavior", v); }
bool Setting::getAutoSaveAfterCapture() { return featBool(L"autoSaveAfterCapture", false); }
void Setting::setAutoSaveAfterCapture(bool on) { setFeatBool(L"autoSaveAfterCapture", on); }
std::wstring Setting::getSaveFormat() { return featStr(L"saveFormat", L"PNG (*.png)"); }
void Setting::setSaveFormat(const std::wstring& v) { setFeatStr(L"saveFormat", v); }
bool Setting::getCopyAsFile() { return featBool(L"copyAsFile", false); }
void Setting::setCopyAsFile(bool on) { setFeatBool(L"copyAsFile", on); }
bool Setting::getCancelConfirmDialog() { return featBool(L"cancelConfirmDialog", true); }
void Setting::setCancelConfirmDialog(bool on) { setFeatBool(L"cancelConfirmDialog", on); }

void Setting::restoreFunctionScreenshotDefaults()
{
	setFindWindowElements(true);
	setCancelConfirmDialog(true);
	setAutoSaveAfterCapture(false);
	setCopyAsFile(false);
	setSaveFormat(L"PNG (*.png)");
	setManualSaveFormat(L"");
	setAutoSaveFormat(L"");
	setFullScreenFormat(L"");
	setFocusedWindowFormat(L"");
	setVideoRecordFormat(L"");
	setTrayClickAction(0);
	setDemoDefaultTool(0);
	setDemoEdgeBehavior(0);
	setPinZoomAtCursor(true);
	setPinAutoOcr(false);
	setPinAutoFit(true);
	setPinDoubleClickAction(0);
}

int Setting::getVideoQuality() { return featInt(L"videoQuality", 2); }
void Setting::setVideoQuality(int v) { setFeatInt(L"videoQuality", v); }
int Setting::getVideoFps() { return featInt(L"videoFps", 0); }
void Setting::setVideoFps(int v) { setFeatInt(L"videoFps", v); }
int Setting::getGifQuality() { return featInt(L"gifQuality", 1); }
void Setting::setGifQuality(int v) { setFeatInt(L"gifQuality", v); }
int Setting::getGifFps() { return featInt(L"gifFps", 1); }
void Setting::setGifFps(int v) { setFeatInt(L"gifFps", v); }
int Setting::getGifFormat() { return featInt(L"gifFormat", 0); }
void Setting::setGifFormat(int v) { setFeatInt(L"gifFormat", v); }
int Setting::getRecordMic() { return featInt(L"recordMic", 0); }
void Setting::setRecordMic(int v) { setFeatInt(L"recordMic", v); }
int Setting::getVideoEncoder() { return featInt(L"videoEncoder", 0); }
void Setting::setVideoEncoder(int v) { setFeatInt(L"videoEncoder", v); }
int Setting::getEncodeSpeed() { return featInt(L"encodeSpeed", 1); }
void Setting::setEncodeSpeed(int v) { setFeatInt(L"encodeSpeed", v); }
bool Setting::getHwAccel() { return featBool(L"hwAccel", true); }
void Setting::setHwAccel(bool on) { setFeatBool(L"hwAccel", on); }
bool Setting::getHideToolbarInRecord() { return featBool(L"hideToolbarInRecord", true); }
void Setting::setHideToolbarInRecord(bool on) { setFeatBool(L"hideToolbarInRecord", on); }
bool Setting::getShowKeystrokes() { return featBool(L"showKeystrokes", true); }
void Setting::setShowKeystrokes(bool on) { setFeatBool(L"showKeystrokes", on); }

std::wstring Setting::defaultManualSaveFormat() { return L"SnowAir_{{YYYY-MM-DD_HH-mm-ss}}"; }
std::wstring Setting::defaultAutoSaveFormat() { return L"SnowAir_auto_{{YYYY-MM-DD_HH-mm-ss}}"; }
std::wstring Setting::defaultFullScreenFormat() { return L"SnowAir_full_{{YYYY-MM-DD_HH-mm-ss}}"; }
std::wstring Setting::defaultFocusedWindowFormat() { return L"{{FOCUS_WINDOW_APP_NAME}}/SnowAir_{{YYYY-MM-DD_HH-mm-ss}}"; }
std::wstring Setting::defaultVideoRecordFormat() { return L"SnowAir_Video_{{YYYY-MM-DD_HH-mm-ss}}"; }

std::wstring Setting::getManualSaveFormat() { return featStr(L"manualSaveFormat", L""); }
void Setting::setManualSaveFormat(const std::wstring& v) { setFeatStr(L"manualSaveFormat", v); }
std::wstring Setting::getAutoSaveFormat() { return featStr(L"autoSaveFormat", L""); }
void Setting::setAutoSaveFormat(const std::wstring& v) { setFeatStr(L"autoSaveFormat", v); }
std::wstring Setting::getFullScreenFormat() { return featStr(L"fullScreenFormat", L""); }
void Setting::setFullScreenFormat(const std::wstring& v) { setFeatStr(L"fullScreenFormat", v); }
std::wstring Setting::getFocusedWindowFormat() { return featStr(L"focusedWindowFormat", L""); }
void Setting::setFocusedWindowFormat(const std::wstring& v) { setFeatStr(L"focusedWindowFormat", v); }
std::wstring Setting::getVideoRecordFormat() { return featStr(L"videoRecordFormat", L""); }
void Setting::setVideoRecordFormat(const std::wstring& v) { setFeatStr(L"videoRecordFormat", v); }

std::wstring Setting::getScreenshotDir() { return featStr(L"screenshotDir", L""); }
void Setting::setScreenshotDir(const std::wstring& v) { setFeatStr(L"screenshotDir", v); }
std::wstring Setting::getRecordDir() { return featStr(L"recordDir", L""); }
void Setting::setRecordDir(const std::wstring& v) { setFeatStr(L"recordDir", v); }

int Setting::getHistoryRetentionDays() { return featInt(L"historyRetentionDays", 7); }
void Setting::setHistoryRetentionDays(int days) { setFeatInt(L"historyRetentionDays", days); }
int Setting::getAppTheme() { return featInt(L"appTheme", 0); }
void Setting::setAppTheme(int v) { setFeatInt(L"appTheme", v); }

// 全局截图热键。RegisterHotKey 是"先注册先独占"：组合键已被别的程序（浏览器插件、聊天工具、
// 输入法…）占着时我们这次注册直接失败，系统里也没有"提高优先级"这回事。所以这里的策略是：
//   1) 先正常 RegisterHotKey；
//   2) 失败就装低级键盘钩子兜底 —— 钩子比任何程序的热键/加速键都先拿到按键，匹配上就吃掉，
//      效果上等于"这个组合归我们"，浏览器抢不走；
//   3) 同时起一个低频重试，等对方把组合让出来再切回干净的 RegisterHotKey（并摘掉钩子）。
void Setting::applyCaptureHotkey(const std::wstring& chord)
{
    captureChord = chord;
    auto lingApp = Ling::App::get();
    if (!lingApp) return;
    lingApp->unRegHotKey(capShortcutMsgId);
    const bool ok = !chord.empty() && lingApp->regHotKey(chord, capShortcutMsgId);
    if (ok) {
        HotkeyHook::uninstall();
        stopCaptureHotkeyRetry();
        return;
    }
    HotkeyHook::install(chord, []() { WinCap::init(); });
    startCaptureHotkeyRetry();
}

// 设置页开始录制热键前调用：临时注销系统热键、摘掉低级钩子，让按键能正常落到设置窗口。
// 只挂起不忘记组合键 —— captureChord 仍保留，resumeShortcutKeys() 据此原样恢复。
void Setting::suspendShortcutKeys()
{
    auto lingApp = Ling::App::get();
    if (lingApp) lingApp->unRegHotKey(capShortcutMsgId);
    HotkeyHook::uninstall();
    stopCaptureHotkeyRetry();
}

// 录制结束（保存或取消）后调用：按挂起前的组合键重新注册/装钩子
void Setting::resumeShortcutKeys()
{
    if (captureChord.empty()) return;
    applyCaptureHotkey(captureChord);
}

void Setting::startCaptureHotkeyRetry()
{
    if (captureRetryTimer) return;
    captureRetryTick = 0;
    // hwnd 传 NULL 的定时器：回调在装它的线程（UI 线程）的消息泵里跑，不需要窗口
    captureRetryTimer = SetTimer(nullptr, kCaptureRetryTimerId, 4000, &Setting::onCaptureHotkeyRetry);
}

void Setting::stopCaptureHotkeyRetry()
{
    if (!captureRetryTimer) return;
    KillTimer(nullptr, captureRetryTimer);
    captureRetryTimer = 0;
    captureRetryTick = 0;
}

void CALLBACK Setting::onCaptureHotkeyRetry(HWND, UINT, UINT_PTR id, DWORD)
{
    if (id != kCaptureRetryTimerId) return;
    auto* self = get();
    if (!self) return;
    // 试满 ~1 分钟就停：钩子还在兜着，热键照样能用，不用一直占着定时器
    if (++self->captureRetryTick > 15) {
        self->stopCaptureHotkeyRetry();
        return;
    }
    auto lingApp = Ling::App::get();
    if (!lingApp || self->captureChord.empty()) return;
    if (lingApp->regHotKey(self->captureChord, capShortcutMsgId)) {
        HotkeyHook::uninstall();
        self->stopCaptureHotkeyRetry();
    }
}

void Setting::initShortcutKeys()
{
    auto lingApp = Ling::App::get();
    // 取不到就用默认的那个组合：热键注册不上顶多是快捷键不好用，不该让程序起不来
    std::wstring capStr{ getShortcutKey(L"capture") };
    if (capStr.empty()) capStr = L"Ctrl+Alt+A";
    applyCaptureHotkey(capStr);

    lingApp->onHotKey.add([this](UINT msg) {
        if (msg == capShortcutMsgId) {
            WinCap::init();
        }
    });
    // 已经有一个实例在托盘里时，再运行一次 exe：打开设置中心，而不是直接进截图模式。
    // （"打开就只挂托盘待命"这条规则的延伸：重新打开 = 把软件的窗口调出来）
    lingApp->onSecondInstance.add([this]() {
        WinSetting::init();
    });
}

// ================ 贴图边框外观 ================
bool Setting::getPinBorderDefaultEnabled() { return featBool(L"pinBorderDefaultEnabled", true); }
void Setting::setPinBorderDefaultEnabled(bool on) { setFeatBool(L"pinBorderDefaultEnabled", on); }
int Setting::getPinBorderWidth() { return featInt(L"pinBorderWidth", 2); }
void Setting::setPinBorderWidth(int v) { setFeatInt(L"pinBorderWidth", v); }
int Setting::getPinBorderRadius() { return featInt(L"pinBorderRadius", 6); }
void Setting::setPinBorderRadius(int v) { setFeatInt(L"pinBorderRadius", v); }
UINT32 Setting::getPinBorderColor()
{
    auto f = featuresObj();
    if (!f.HasKey(L"pinBorderColor")) return 0x34C759FF; // 默认主题绿
    return static_cast<UINT32>(f.GetNamedNumber(L"pinBorderColor"));
}
void Setting::setPinBorderColor(UINT32 argb)
{
    featuresObj().SetNamedValue(L"pinBorderColor", JsonValue::CreateNumberValue(static_cast<double>(argb)));
    save();
}

// ================ 托盘图标 ================
bool Setting::getTrayEnabled() { return featBool(L"trayEnabled", true); }
void Setting::setTrayEnabled(bool on) { setFeatBool(L"trayEnabled", on); }
int Setting::getTrayIconStyle()
{
    // 用新键 trayIconStyle2 存 4 档（0 跟随系统 / 1 主题 / 2 自定义颜色 / 3 自定义图标）。
    // 不能就地复用旧键：旧的 5 档里 暗色=2、亮色=3 与新方案的 2/3 撞号，会互相污染。
    auto f = featuresObj();
    if (f.HasKey(L"trayIconStyle2")) {
        const int v = static_cast<int>(f.GetNamedNumber(L"trayIconStyle2"));
        return (v < 0 || v > 3) ? 1 : v;
    }
    // 首次读取：从旧键一次性迁移（旧：0 跟随系统 / 1 主题 / 2 暗色 / 3 亮色 / 4 自定义）
    int old = 1;
    if (f.HasKey(L"trayIconStyle")) old = static_cast<int>(f.GetNamedNumber(L"trayIconStyle"));
    int nv = 1;
    if (old == 4) nv = 3;                   // 旧「自定义」→ 自定义图标
    else if (old == 2 || old == 3) nv = 1;  // 旧 暗色/亮色 → 主题
    else nv = (old == 0) ? 0 : 1;           // 0 跟随系统 / 其余默认主题
    setFeatInt(L"trayIconStyle2", nv);      // 迁移结果落到新键，之后只读新键
    return nv;
}
void Setting::setTrayIconStyle(int v)
{
    setFeatInt(L"trayIconStyle2", std::max(0, std::min(3, v)));
}
UINT32 Setting::getTrayCustomColor()
{
    auto f = featuresObj();
    if (!f.HasKey(L"trayCustomColor")) return 0x34C759FF;   // 默认主题绿
    return static_cast<UINT32>(f.GetNamedNumber(L"trayCustomColor"));
}
void Setting::setTrayCustomColor(UINT32 rgba)
{
    featuresObj().SetNamedValue(L"trayCustomColor", JsonValue::CreateNumberValue(static_cast<double>(rgba)));
    save();
}
std::wstring Setting::getTrayCustomIconPath() { return featStr(L"trayCustomIconPath", L""); }
void Setting::setTrayCustomIconPath(const std::wstring& path) { setFeatStr(L"trayCustomIconPath", path); }

// ================ 新增：浮动截图按钮 ================
bool Setting::getFloatingCaptureButtonShow() { return featBool(L"floatingCaptureButtonShow", true); }
void Setting::setFloatingCaptureButtonShow(bool on) { setFeatBool(L"floatingCaptureButtonShow", on); }
int Setting::getFloatingCaptureButtonClick() { return featInt(L"floatingCaptureButtonClick", 0); }
void Setting::setFloatingCaptureButtonClick(int v) { setFeatInt(L"floatingCaptureButtonClick", v); }
int Setting::getFloatingLongPressMs() { return featInt(L"floatingLongPressMs", 500); }
void Setting::setFloatingLongPressMs(int ms) { setFeatInt(L"floatingLongPressMs", ms); }

// ================ 新增：窗口黑名单 ================
std::vector<std::wstring> Setting::getWindowBlacklist()
{
    std::vector<std::wstring> list;
    for (auto const& v : featArray(L"windowBlacklist")) {
        if (v.ValueType() == JsonValueType::String)
            list.push_back(v.GetString().c_str());
    }
    return list;
}
void Setting::setWindowBlacklist(const std::vector<std::wstring>& titles)
{
    JsonArray arr;
    for (auto& t : titles) arr.Append(JsonValue::CreateStringValue(t));
    setFeatArray(L"windowBlacklist", arr);
}

// ================ 新增：History 目录 / 条目 / 过期清理 ================
static JsonObject historyStore(JsonObject& features)
{
    if (!features.HasKey(L"history") || features.GetNamedValue(L"history").ValueType() != JsonValueType::Object)
        features.SetNamedValue(L"history", JsonObject{});
    return features.GetNamedObject(L"history");
}

std::filesystem::path Setting::getHistoryDir()
{
    auto dir = dataPath;
    dir.append(L"History");
    if (!std::filesystem::exists(dir)) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
    }
    return dir;
}

static JsonObject historyItemToJson(const Setting::HistoryItem& it)
{
    JsonObject o;
    o.SetNamedValue(L"id", JsonValue::CreateStringValue(it.id));
    o.SetNamedValue(L"filePath", JsonValue::CreateStringValue(it.filePath));
    o.SetNamedValue(L"source", JsonValue::CreateStringValue(it.source));
    o.SetNamedValue(L"width", JsonValue::CreateNumberValue(it.width));
    o.SetNamedValue(L"height", JsonValue::CreateNumberValue(it.height));
    o.SetNamedValue(L"createdAt", JsonValue::CreateNumberValue(static_cast<double>(it.createdAt)));
    return o;
}
static Setting::HistoryItem historyItemFromJson(const JsonObject& o)
{
    Setting::HistoryItem it;
    it.id = o.GetNamedString(L"id", L"").c_str();
    it.filePath = o.GetNamedString(L"filePath", L"").c_str();
    it.source = o.GetNamedString(L"source", L"capture").c_str();
    it.width = static_cast<int>(o.GetNamedNumber(L"width", 0));
    it.height = static_cast<int>(o.GetNamedNumber(L"height", 0));
    it.createdAt = static_cast<long long>(o.GetNamedNumber(L"createdAt", 0));
    return it;
}

std::vector<Setting::HistoryItem> Setting::getHistoryItems()
{
    auto features = featuresObj();
    auto store = historyStore(features);
    JsonArray arr{};
    if (store.HasKey(L"items") && store.GetNamedValue(L"items").ValueType() == JsonValueType::Array)
        arr = store.GetNamedArray(L"items");
    std::vector<HistoryItem> list;
    for (auto const& v : arr) {
        if (v.ValueType() != JsonValueType::Object) continue;
        list.push_back(historyItemFromJson(v.GetObjectW()));
    }
    // 按创建时间倒序 (新的在前)
    std::sort(list.begin(), list.end(), [](const HistoryItem& a, const HistoryItem& b) {
        return a.createdAt > b.createdAt;
    });
    return list;
}

// 记一条截图历史：把图像编码成 PNG 写进历史目录（History/<id>.png），再登记到
// features.history.items。历史自带一份文件，删条目/清空历史只动历史目录里的副本。
std::wstring Setting::addHistoryItem(const std::vector<BYTE>& bgra, int width, int height, const std::wstring& source)
{
    if (bgra.empty() || width <= 0 || height <= 0) return L"";
    HistoryItem it;
    // 时间戳（毫秒）+ 递增序号，避免同一毫秒内连续写入时 id 撞车
    static UINT64 seq = 0;
    const auto now = std::chrono::system_clock::now();
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    it.id = std::format(L"h{}-{}", nowMs, ++seq);
    it.source = source.empty() ? L"capture" : source;
    it.width = width;
    it.height = height;
    it.createdAt = (long long)nowMs;

    auto file = getHistoryDir() / (it.id + L".png");
    if (!Util::saveToFile(file.wstring(), width, height, const_cast<BYTE*>(bgra.data()))) return L"";
    it.filePath = file.wstring();

    JsonArray arr;
    arr.Append(historyItemToJson(it));   // 新的排在前面（与 getHistoryItems 的排序一致）
    for (auto& old : getHistoryItems()) arr.Append(historyItemToJson(old));

    auto features = featuresObj();
    auto store = historyStore(features);
    store.SetNamedValue(L"items", arr);
    features.SetNamedValue(L"history", store);
    save();
    return it.filePath;
}

void Setting::pruneExpiredHistory()
{
    int days = getHistoryRetentionDays();
    if (days < 0) return; // -1 代表永久保留
    // 当前 ms 时间戳
    auto now = std::chrono::system_clock::now();
    auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    long long cutoff = nowMs - static_cast<long long>(days) * 24LL * 3600LL * 1000LL;

    auto list = getHistoryItems();
    std::vector<HistoryItem> keep;
    for (auto& it : list) {
        if (it.createdAt >= cutoff) {
            keep.push_back(it);
        } else {
            // 删除过期文件
            std::error_code ec;
            if (!it.filePath.empty()) std::filesystem::remove(it.filePath, ec);
        }
    }
    // 写回
    JsonArray arr;
    for (auto& it : keep) arr.Append(historyItemToJson(it));
    auto features = featuresObj();
    auto store = historyStore(features);
    store.SetNamedValue(L"items", arr);
    features.SetNamedValue(L"history", store);
    save();
}

void Setting::removeHistoryItem(const std::wstring& id)
{
    auto list = getHistoryItems();
    std::vector<HistoryItem> keep;
    for (auto& it : list) {
        if (it.id == id) {
            std::error_code ec;
            if (!it.filePath.empty()) std::filesystem::remove(it.filePath, ec);
        } else {
            keep.push_back(it);
        }
    }
    JsonArray arr;
    for (auto& it : keep) arr.Append(historyItemToJson(it));
    auto features = featuresObj();
    auto store = historyStore(features);
    store.SetNamedValue(L"items", arr);
    features.SetNamedValue(L"history", store);
    save();
}

void Setting::clearAllHistory()
{
    auto list = getHistoryItems();
    for (auto& it : list) {
        std::error_code ec;
        if (!it.filePath.empty()) std::filesystem::remove(it.filePath, ec);
    }
    auto features = featuresObj();
    auto store = historyStore(features);
    store.SetNamedValue(L"items", JsonArray{});
    features.SetNamedValue(L"history", store);
    save();
}
