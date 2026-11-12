#include "pch.h"
#include "PluginHost.h"

#include "../Setting.h"
#include "../Util.h"
#include "../Ocr/PluginPaths.h"
#include "../Win/SettingTheme.h"
#include "../Win/WinSetting.h"
#include "../Win/PinMenu.h"
#include "../Win/WinPin.h"
#include "../Win/ClipPin.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>

namespace {

void showMenu(void*, const SnowAirMenuItem* items, int count, int x, int y,
              void* pluginUser, SnowAirMenuSelect onSelect)
{
    if (!items || count <= 0) {
        return;
    }
    std::function<PinMenu::Item(const SnowAirMenuItem&)> build =
        [&](const SnowAirMenuItem& source) -> PinMenu::Item {
        PinMenu::Item item;
        item.text = source.label ? source.label : L"";
        item.separator = (source.flags & SNOWAIR_MENU_SEPARATOR) != 0;
        item.checked = (source.flags & SNOWAIR_MENU_CHECKED) != 0;
        item.enabled = (source.flags & SNOWAIR_MENU_DISABLED) == 0;
        if (source.submenu && source.submenu_count > 0) {
            item.submenu.reserve(static_cast<size_t>(source.submenu_count));
            for (int i = 0; i < source.submenu_count; ++i) {
                item.submenu.push_back(build(source.submenu[i]));
            }
        }
        item.action = [pluginUser, id = source.id, onSelect]() {
            if (onSelect) onSelect(pluginUser, id);
        };
        return item;
    };

    std::vector<PinMenu::Item> menu;
    menu.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        menu.push_back(build(items[i]));
    }
    PinMenu::popup(std::move(menu), POINT{x, y});
}

int menuOpen(void*)
{
    return PinMenu::isOpen() ? 1 : 0;
}

void closeMenu(void*)
{
    PinMenu::closeMenu();
}

std::wstring clipLabel(const Setting::ClipboardItem& item);
void copyClipboardItem(const Setting::ClipboardItem& item);

int clipCount(void*)
{
    auto* s = Setting::get();
    return s ? static_cast<int>(s->getClipboardItems().size()) : 0;
}

int clipItem(void*, int index, wchar_t* label, int capacity, int* type)
{
    auto* s = Setting::get();
    if (!s || index < 0 || !label || capacity <= 0) return 0;
    auto items = s->getClipboardItems();
    if (index >= static_cast<int>(items.size())) return 0;
    const auto& item = items[index];
    std::wstring text = clipLabel(item);
    wcsncpy_s(label, static_cast<size_t>(capacity), text.c_str(), _TRUNCATE);
    if (type) {
        *type = item.type == L"image" ? 1 : (item.type == L"files" ? 2 : 0);
    }
    return 1;
}

void clipDelete(void*, int index)
{
    auto* s = Setting::get();
    if (!s) return;
    auto items = s->getClipboardItems();
    if (index >= 0 && index < static_cast<int>(items.size())) {
        s->removeClipboardItem(items[index].id);
    }
}

int clipMeta(void*, int index, wchar_t* meta, int capacity)
{
    auto* s = Setting::get();
    if (!s || !meta || capacity <= 0) return 0;
    auto items = s->getClipboardItems();
    if (index < 0 || index >= static_cast<int>(items.size())) return 0;
    const auto& item = items[index];
    wchar_t date[64]{};
    if (item.createdAt > 0) {
        const time_t tt = static_cast<time_t>(item.createdAt / 1000);
        struct tm local{};
        localtime_s(&local, &tt);
        time_t nowTime = time(nullptr);
        struct tm today{};
        localtime_s(&today, &nowTime);
        const bool isToday = local.tm_year == today.tm_year &&
                             local.tm_yday == today.tm_yday;
        wcsftime(date, 64, isToday ? L"%H:%M" : L"%Y-%m-%d", &local);
    }
    if (date[0] == L'\0') return 0;
    std::wstring text = date;
    if (item.type == L"image" && item.width > 0 && item.height > 0) {
        text += L"  ·  ";
        text += std::to_wstring(item.width) + L"×" + std::to_wstring(item.height);
    }
    wcsncpy_s(meta, static_cast<size_t>(capacity), text.c_str(), _TRUNCATE);
    return 1;
}

int clipPath(void*, int index, wchar_t* path, int capacity)
{
    auto* s = Setting::get();
    if (!s || !path || capacity <= 0) return 0;
    auto items = s->getClipboardItems();
    if (index < 0 || index >= static_cast<int>(items.size())) return 0;
    const auto& item = items[index];
    std::wstring source = item.filePath;
    if (source.empty() && !item.files.empty()) source = item.files.front();
    if (source.empty()) return 0;
    wcsncpy_s(path, static_cast<size_t>(capacity), source.c_str(), _TRUNCATE);
    return 1;
}

/* ---- Envelope panel: SnowAir history + clipboard in one list ---- */

std::vector<Setting::HistoryItem> g_entries;
std::vector<std::wstring> g_entryTags;
bool g_entriesDirty = true;

bool hasExt(const std::wstring& path, std::initializer_list<const wchar_t*> exts)
{
    const auto dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = path.substr(dot);
    for (auto& ch : ext) ch = static_cast<wchar_t>(towlower(ch));
    for (const auto* e : exts) {
        if (ext == e) return true;
    }
    return false;
}

bool isImagePath(const std::wstring& path)
{
    return hasExt(path, {L".png", L".jpg", L".jpeg", L".webp", L".bmp", L".gif", L".svg"});
}

bool allImages(const std::vector<std::wstring>& files)
{
    if (files.empty()) return false;
    for (const auto& f : files) {
        if (!isImagePath(f)) return false;
    }
    return true;
}

std::wstring trimPreview(std::wstring text, size_t limit)
{
    for (auto& ch : text) {
        if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
    }
    while (!text.empty() && text.back() == L' ') text.pop_back();
    while (!text.empty() && text.front() == L' ') text.erase(text.begin());
    if (text.size() > limit) text = text.substr(0, limit) + L"…";
    return text;
}

std::wstring fileNameOf(const std::wstring& path)
{
    return std::filesystem::path(path).filename().wstring();
}

// 截图完成时会顺手复制到剪贴板：同一张图在「截图历史」和「剪贴板历史」各有一条
// （历史是真正的截图文件，剪贴板是缓存目录里的同名内容 PNG）。
// 信封按内容把它们合并成一张卡，所以删除也要两条一起删；这里用字节比对确认是同一张图。
bool sameFileBytes(const std::wstring& a, const std::wstring& b)
{
    if (a.empty() || b.empty()) return false;
    std::ifstream fa(a, std::ios::binary);
    std::ifstream fb(b, std::ios::binary);
    if (!fa || !fb) return false;
    constexpr std::streamsize kChunk = 64 * 1024;
    std::vector<char> ba(kChunk), bb(kChunk);
    while (fa && fb) {
        fa.read(ba.data(), kChunk);
        fb.read(bb.data(), kChunk);
        const std::streamsize na = fa.gcount();
        if (na != fb.gcount()) return false;
        if (na == 0) break;
        if (std::memcmp(ba.data(), bb.data(), static_cast<size_t>(na)) != 0) return false;
    }
    return true;
}

int entryKindOf(const Setting::HistoryItem& it)
{
    if (it.source == L"clipboard") {
        if (!it.files.empty()) {
            return allImages(it.files) ? SNOWAIR_ENTRY_IMAGE : SNOWAIR_ENTRY_FILES;
        }
        if (!it.filePath.empty()) return SNOWAIR_ENTRY_IMAGE;
        return SNOWAIR_ENTRY_TEXT;
    }
    // 目录里扫出来的用户文件没有索引 id（id 就是路径）
    if (it.id == it.filePath) {
        return isImagePath(it.filePath) ? SNOWAIR_ENTRY_IMAGE : SNOWAIR_ENTRY_FILES;
    }
    if (it.source == L"record") return SNOWAIR_ENTRY_MEDIA;
    return SNOWAIR_ENTRY_IMAGE;
}

int entrySourceOf(const Setting::HistoryItem& it)
{
    if (it.source == L"clipboard") return SNOWAIR_SOURCE_CLIPBOARD;
    if (it.source == L"record") return SNOWAIR_SOURCE_RECORD;
    // 目录扫描出来、文件名不是本软件保存模板的，才算「其他」用户文件
    if (it.id == it.filePath) {
        auto* s = Setting::get();
        if (!s || !s->isOwnHistoryFile(it.filePath)) return SNOWAIR_SOURCE_OTHER;
    }
    if (it.source == L"focused") return SNOWAIR_SOURCE_FOCUSED;
    if (it.source == L"fullscreen") return SNOWAIR_SOURCE_FULLSCREEN;
    if (it.source == L"long") return SNOWAIR_SOURCE_LONG;
    if (it.source == L"autosave") return SNOWAIR_SOURCE_AUTOSAVE;
    return SNOWAIR_SOURCE_CAPTURE;
}

std::wstring entryLabelOf(const Setting::HistoryItem& it)
{
    const int kind = entryKindOf(it);
    if (kind == SNOWAIR_ENTRY_TEXT) return trimPreview(it.text, 240);
    std::wstring first = it.filePath;
    if (first.empty() && !it.files.empty()) first = it.files.front();
    std::wstring name = fileNameOf(first);
    if (name.empty()) name = kind == SNOWAIR_ENTRY_IMAGE ? L"图片" : L"文件";
    if (!it.files.empty() && it.files.size() > 1) {
        name += L"  等 " + std::to_wstring(it.files.size()) + L" 个";
    }
    return name;
}

std::wstring entryTagsOf(const Setting::HistoryItem& it)
{
    auto* s = Setting::get();
    if (!s) return {};
    const std::vector<std::wstring> tags = it.source == L"clipboard"
        ? s->getClipboardTags(it.id) : s->getHistoryTags(it.filePath);
    std::wstring joined;
    for (const auto& t : tags) {
        if (t.empty()) continue;
        if (!joined.empty()) joined.push_back(L'\n');
        joined += t;
    }
    return joined;
}

void ensureEntries()
{
    if (!g_entriesDirty) return;
    g_entriesDirty = false;
    g_entries.clear();
    g_entryTags.clear();
    auto* s = Setting::get();
    if (!s) return;
    for (const auto& c : s->getClipboardItems()) {
        Setting::HistoryItem it;
        it.id = c.id;
        it.source = L"clipboard";
        it.filePath = (c.type == L"image") ? c.filePath : L"";
        it.width = c.width;
        it.height = c.height;
        if (it.width <= 0 && c.files.size() == 1 && isImagePath(c.files.front())) {
            const auto wh = Util::imageSize(c.files.front());
            it.width = wh.first;
            it.height = wh.second;
        }
        it.createdAt = c.createdAt;
        it.text = c.text;
        it.files = c.files;
        g_entries.push_back(std::move(it));
    }
    for (auto& h : s->getHistoryItemsForUi(false)) g_entries.push_back(std::move(h));
    std::stable_sort(g_entries.begin(), g_entries.end(),
                     [](const Setting::HistoryItem& a, const Setting::HistoryItem& b) {
                         return a.createdAt > b.createdAt;
                     });
    g_entryTags.reserve(g_entries.size());
    for (const auto& it : g_entries) g_entryTags.push_back(entryTagsOf(it));
}

int entryCount(void*)
{
    ensureEntries();
    return static_cast<int>(g_entries.size());
}

void entryRefresh(void*)
{
    g_entriesDirty = true;
}

int entryGet(void*, int index, SnowAirEntry* out)
{
    if (!out) return 0;
    ensureEntries();
    if (index < 0 || index >= static_cast<int>(g_entries.size())) return 0;
    const auto& it = g_entries[static_cast<size_t>(index)];
    *out = SnowAirEntry{};
    out->kind = entryKindOf(it);
    out->source = entrySourceOf(it);
    out->width = it.width;
    out->height = it.height;
    out->duration_sec = it.durationSec;
    out->created_at = it.createdAt;
    const bool noteTagged =
        index < static_cast<int>(g_entryTags.size()) &&
        g_entryTags[static_cast<size_t>(index)].find(L"便签") != std::wstring::npos;
    std::wstring label = entryLabelOf(it);
    /* SVG 代码在轻雪里是一张图：卡片标题用 SVG，别把代码铺出来。
       带「便签」标签的记录仍按便签处理，哪怕正文刚好是 SVG。 */
    if (!noteTagged && out->kind == SNOWAIR_ENTRY_TEXT &&
        it.source == L"clipboard" && Util::isSvgText(it.text)) {
        label = L"SVG";
    }
    wcsncpy_s(out->label, SNOWAIR_ENTRY_LABEL_MAX, label.c_str(), _TRUNCATE);
    std::wstring path = it.filePath;
    if (path.empty() && !it.files.empty()) path = it.files.front();
    wcsncpy_s(out->path, SNOWAIR_ENTRY_PATH_MAX, path.c_str(), _TRUNCATE);
    std::wstring thumb;
    if (out->kind == SNOWAIR_ENTRY_MEDIA) {
        auto* s = Setting::get();
        if (s) {
            const std::wstring cover = (s->getHistoryThumbDir() / (it.id + L".png")).wstring();
            std::error_code ec;
            if (std::filesystem::exists(cover, ec)) thumb = cover;
        }
    }
    if (thumb.empty() && out->kind == SNOWAIR_ENTRY_IMAGE) thumb = path;
    /* 剪切板里的 SVG 代码：直接用轻雪历史页那份渲染缓存，
       插件拿 thumb 就能当图片画，不再重复实现一套 SVG 渲染。 */
    if (out->kind == SNOWAIR_ENTRY_TEXT && !noteTagged && it.source == L"clipboard" &&
        Util::isSvgText(it.text)) {
        const std::wstring svgThumb = Util::svgPreviewPath(it.id, it.text);
        if (!svgThumb.empty()) {
            thumb = svgThumb;
            const auto [sw, sh] = Util::imageSize(thumb);
            if (sw > 0 && sh > 0) {
                out->width = sw;
                out->height = sh;
            }
        }
    }
    wcsncpy_s(out->thumb, SNOWAIR_ENTRY_PATH_MAX, thumb.c_str(), _TRUNCATE);
    wcsncpy_s(out->tags, SNOWAIR_ENTRY_TAGS_MAX,
              g_entryTags[static_cast<size_t>(index)].c_str(), _TRUNCATE);
    return 1;
}

void entryCopy(void*, int index)
{
    ensureEntries();
    if (index < 0 || index >= static_cast<int>(g_entries.size())) return;
    const auto it = g_entries[static_cast<size_t>(index)];
    auto* s = Setting::get();
    if (!s) return;
    if (it.source == L"clipboard") {
        for (const auto& c : s->getClipboardItems()) {
            if (c.id != it.id) continue;
            copyClipboardItem(c);
            break;
        }
    } else if (entryKindOf(it) == SNOWAIR_ENTRY_IMAGE && !it.filePath.empty()) {
        auto [w, h, px] = Util::loadImagePixels(it.filePath);
        if (w > 0 && h > 0 && !px.empty()) Util::saveToClipboard(w, h, px.data());
    } else {
        std::vector<std::wstring> files = it.files;
        if (files.empty() && !it.filePath.empty()) files.push_back(it.filePath);
        if (!files.empty()) Util::addFilesToClipboard(files);
    }
    // 这次写入是我们自己触发的：不要在剪切板历史里再生一条
    s->markOwnClipboardWrite();
}

int entryHang(void*, int index)
{
    ensureEntries();
    if (index < 0 || index >= static_cast<int>(g_entries.size())) return 0;
    const auto& it = g_entries[static_cast<size_t>(index)];
    if (entryKindOf(it) != SNOWAIR_ENTRY_IMAGE) return 0;
    std::wstring path = it.filePath;
    if (path.empty() && !it.files.empty()) path = it.files.front();
    if (path.empty()) return 0;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return 0;
    PluginHost::instance().onCapture(path);
    return 1;
}

void entryDelete(void*, int index)
{
    ensureEntries();
    if (index < 0 || index >= static_cast<int>(g_entries.size())) return;
    const auto it = g_entries[static_cast<size_t>(index)];
    auto* s = Setting::get();
    if (!s) return;
    std::error_code ec;
    if (it.source == L"clipboard") {
        // 信封里这张卡可能是「剪贴板副本 + 截图历史」合并出来的：
        // 先把内容相同的截图历史找出来，删完剪贴板记录再一起删（文件移入回收站）。
        std::vector<Setting::HistoryItem> twins;
        if (isImagePath(it.filePath)) {
            for (const auto& h : s->getHistoryItemsForUi(true)) {
                if (h.filePath.empty() || !isImagePath(h.filePath)) continue;
                if (sameFileBytes(it.filePath, h.filePath)) twins.push_back(h);
            }
        }
        s->removeClipboardItem(it.id);
        for (const auto& h : twins) s->removeHistoryItem(h.id, true);
    } else {
        const bool exists = !it.filePath.empty() && std::filesystem::exists(it.filePath, ec);
        // 反过来：删截图那条时，把剪贴板里同一张图的记录也删掉（不动截图文件之外的任何东西）
        std::vector<std::wstring> twinClips;
        if (exists && isImagePath(it.filePath)) {
            for (const auto& c : s->getClipboardItems()) {
                if (c.type != L"image" || c.filePath.empty()) continue;
                if (sameFileBytes(it.filePath, c.filePath)) twinClips.push_back(c.id);
            }
        }
        s->removeHistoryItem(it.id, exists);   // recycle when the file still exists
        for (const auto& id : twinClips) s->removeClipboardItem(id);
    }
    g_entriesDirty = true;
}

void entryReveal(void*, int index)
{
    ensureEntries();
    if (index < 0 || index >= static_cast<int>(g_entries.size())) return;
    const auto& it = g_entries[static_cast<size_t>(index)];
    std::wstring path = it.filePath;
    if (path.empty() && !it.files.empty()) path = it.files.front();
    if (path.empty()) return;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return;
    const std::wstring args = L"/select,\"" + path + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

std::vector<std::wstring> splitTagList(const wchar_t* tags)
{
    std::vector<std::wstring> out;
    if (!tags) return out;
    const std::wstring text = tags;
    size_t start = 0;
    while (start <= text.size()) {
        size_t pos = text.find(L'\n', start);
        if (pos == std::wstring::npos) pos = text.size();
        std::wstring part = text.substr(start, pos - start);
        if (!part.empty()) out.push_back(std::move(part));
        if (pos == text.size()) break;
        start = pos + 1;
    }
    return out;
}

void entryTagsSet(void*, int index, const wchar_t* tags)
{
    ensureEntries();
    if (index < 0 || index >= static_cast<int>(g_entries.size())) return;
    auto* s = Setting::get();
    if (!s) return;
    const auto it = g_entries[static_cast<size_t>(index)];
    const std::vector<std::wstring> list = splitTagList(tags);
    if (it.source == L"clipboard") s->setClipboardTags(it.id, list);
    else s->setHistoryTags(it.filePath, list);
    std::wstring joined;
    for (const auto& t : list) {
        if (t.empty()) continue;
        if (!joined.empty()) joined.push_back(L'\n');
        joined += t;
    }
    if (g_entryTags.size() == g_entries.size()) {
        g_entryTags[static_cast<size_t>(index)] = joined;
    }
}

int entryAllTags(void*, wchar_t* out, int capacity)
{
    if (!out || capacity <= 0) return 0;
    ensureEntries();
    std::map<std::wstring, int> counts;
    for (const auto& joined : g_entryTags) {
        size_t start = 0;
        while (start <= joined.size()) {
            size_t pos = joined.find(L'\n', start);
            if (pos == std::wstring::npos) pos = joined.size();
            if (pos > start) counts[joined.substr(start, pos - start)]++;
            if (pos == joined.size()) break;
            start = pos + 1;
        }
    }
    std::vector<std::pair<std::wstring, int>> ranked(counts.begin(), counts.end());
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const std::pair<std::wstring, int>& a,
                        const std::pair<std::wstring, int>& b) {
                         if (a.second != b.second) return a.second > b.second;
                         return a.first < b.first;
                     });
    std::wstring joined;
    for (const auto& entry : ranked) {
        if (!joined.empty()) joined.push_back(L'\n');
        joined += entry.first;
    }
    wcsncpy_s(out, static_cast<size_t>(capacity), joined.c_str(), _TRUNCATE);
    return static_cast<int>(ranked.size());
}

/* ---- v4：文本记录（便签）读写 ---- */

namespace {
std::wstring trimNoteText(const wchar_t* text)
{
    std::wstring out = text ? text : L"";
    while (!out.empty() &&
           (out.back() == L'\0' || out.back() == L'\r' || out.back() == L'\n')) {
        out.pop_back();
    }
    return out;
}
}  // namespace

int entryId(void*, int index, wchar_t* out, int capacity)
{
    if (!out || capacity <= 0) return 0;
    out[0] = 0;
    ensureEntries();
    if (index < 0 || index >= static_cast<int>(g_entries.size())) return 0;
    const auto& it = g_entries[static_cast<size_t>(index)];
    if (it.source != L"clipboard") return 0;
    wcsncpy_s(out, static_cast<size_t>(capacity), it.id.c_str(), _TRUNCATE);
    return 1;
}

int entryText(void*, const wchar_t* id, wchar_t* out, int capacity)
{
    if (!id || !*id) return -1;
    auto* s = Setting::get();
    if (!s) return -1;
    for (const auto& c : s->getClipboardItems()) {
        if (c.id != id) continue;
        if (c.type != L"text") return -1;
        if (out && capacity > 0) {
            wcsncpy_s(out, static_cast<size_t>(capacity), c.text.c_str(), _TRUNCATE);
        }
        return static_cast<int>(c.text.size());
    }
    return -1;
}

int entryCreateText(void*, const wchar_t* text, const wchar_t* tags, wchar_t* idOut,
                    int capacity)
{
    if (idOut && capacity > 0) idOut[0] = 0;
    const std::wstring body = trimNoteText(text);
    if (body.empty()) return 0;
    auto* s = Setting::get();
    if (!s) return 0;
    /* 每条便签都是独立记录：不去重，避免把已有的历史条目改造成便签。 */
    s->addClipboardText(body, false);
    const auto items = s->getClipboardItems();
    if (items.empty() || items.front().type != L"text") return 0;
    const std::wstring id = items.front().id;
    const auto list = splitTagList(tags);
    if (!list.empty()) s->setClipboardTags(id, list);
    if (idOut && capacity > 0) {
        wcsncpy_s(idOut, static_cast<size_t>(capacity), id.c_str(), _TRUNCATE);
    }
    g_entriesDirty = true;
    return 1;
}

int entryUpdateText(void*, const wchar_t* id, const wchar_t* text, const wchar_t* tags)
{
    if (!id || !*id) return 0;
    const std::wstring body = trimNoteText(text);
    if (body.empty()) return 0;
    auto* s = Setting::get();
    if (!s) return 0;
    if (!s->setClipboardText(id, body)) return 0;
    if (tags && *tags) s->setClipboardTags(id, splitTagList(tags));
    g_entriesDirty = true;
    return 1;
}

int entryDeleteId(void*, const wchar_t* id)
{
    if (!id || !*id) return 0;
    auto* s = Setting::get();
    if (!s) return 0;
    s->removeClipboardItem(id);
    g_entriesDirty = true;
    return 1;
}

int clipHang(void*, int index)
{
    auto* s = Setting::get();
    if (!s) return 0;
    auto items = s->getClipboardItems();
    if (index < 0 || index >= static_cast<int>(items.size())) return 0;
    const auto& item = items[index];
    std::wstring path = item.filePath;
    if (item.type == L"files" && !item.files.empty()) path = item.files.front();
    if (path.empty()) return 0;
    std::wstring ext = std::filesystem::path(path).extension().wstring();
    for (auto& ch : ext) ch = static_cast<wchar_t>(towlower(ch));
    if (ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".webp" ||
        ext == L".bmp" || ext == L".gif") {
        PluginHost::instance().onCapture(path);
        return 1;
    }
    return 0;
}

void clipCopy(void*, int index)
{
    auto* s = Setting::get();
    if (!s) return;
    auto items = s->getClipboardItems();
    if (index < 0 || index >= static_cast<int>(items.size())) return;
    copyClipboardItem(items[index]);
}

std::wstring clipLabel(const Setting::ClipboardItem& item)
{
    auto clean = [](std::wstring text, size_t limit) {
        for (auto& ch : text) {
            if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
        }
        while (!text.empty() && text.back() == L' ') text.pop_back();
        if (text.size() > limit) text = text.substr(0, limit) + L"…";
        return text;
    };
    if (item.type == L"image") {
        if (!item.filePath.empty()) {
            return std::filesystem::path(item.filePath).filename().wstring();
        }
        return L"图片";
    }
    if (item.type == L"files") {
        if (item.files.empty()) return L"文件";
        return clean(item.files.front(), 36) +
               (item.files.size() > 1 ? L"  等 " + std::to_wstring(item.files.size()) + L" 个"
                                      : L"");
    }
    return clean(item.text, 240);
}

void copyClipboardItem(const Setting::ClipboardItem& item)
{
    if (item.type == L"text") {
        Ling::Util::setTextToClipboard(item.text);
    } else if (item.type == L"image" && !item.filePath.empty()) {
        auto [w, h, px] = Util::loadImagePixels(item.filePath);
        if (!px.empty() && w > 0 && h > 0) Util::saveToClipboard(w, h, px.data());
    } else if (item.type == L"files" && !item.files.empty()) {
        Util::addFilesToClipboard(item.files);
    }
    if (auto* setting = Setting::get()) setting->touchClipboardItem(item.id);
}

void openClipboardPanel(void*, int x, int y)
{
    auto* setting = Setting::get();
    if (!setting) return;
    const auto all = setting->getClipboardItems();
    auto makeSubmenu = [&](const wchar_t* type) {
        std::vector<PinMenu::Item> out;
        for (const auto& item : all) {
            if (type && item.type != type) continue;
            PinMenu::Item entry;
            entry.text = clipLabel(item);
            entry.action = [item]() { copyClipboardItem(item); };
            out.push_back(std::move(entry));
        }
        if (out.empty()) {
            PinMenu::Item empty;
            empty.text = L"（空）";
            empty.enabled = false;
            out.push_back(std::move(empty));
        }
        return out;
    };

    std::vector<PinMenu::Item> menu;
    PinMenu::Item header;
    header.text = L"剪切板";
    header.enabled = false;
    menu.push_back(std::move(header));
    PinMenu::Item separator;
    separator.separator = true;
    menu.push_back(std::move(separator));

    struct Filter { const wchar_t* label; const wchar_t* type; };
    static const Filter filters[] = {
        {L"全部", nullptr}, {L"图片", L"image"}, {L"文本", L"text"}, {L"文件", L"files"}};
    for (const auto& filter : filters) {
        PinMenu::Item entry;
        entry.text = filter.label;
        entry.submenu = makeSubmenu(filter.type);
        menu.push_back(std::move(entry));
    }
    PinMenu::popup(std::move(menu), POINT{x, y});
}

int captureRect(void*, int* x, int* y, int* w, int* h)
{
    auto* setting = Setting::get();
    if (!setting || !x || !y || !w || !h) return 0;
    int rx = 0, ry = 0, rw = 0, rh = 0;
    if (!setting->getLastCaptureRect(rx, ry, rw, rh)) return 0;
    *x = rx;
    *y = ry;
    *w = rw;
    *h = rh;
    return 1;
}

void dbg(const wchar_t* text)
{
    OutputDebugStringW(L"[SnowAirPlugin] ");
    OutputDebugStringW(text);
    OutputDebugStringW(L"\n");
}

uint32_t themeColor(void*, int role)
{
    switch (role) {
    case SA_THEME_FG: return SettingTheme::textPrimary;
    case SA_THEME_FG_SECONDARY: return SettingTheme::textSecondary;
    case SA_THEME_FG_TERTIARY: return SettingTheme::textMuted;
    case SA_THEME_BG: return SettingTheme::cardBg;
    case SA_THEME_CANVAS: return SettingTheme::windowBg;
    case SA_THEME_BORDER: return SettingTheme::border;
    case SA_THEME_ACCENT: return SettingTheme::accent;
    case SA_THEME_ACCENT_FG: return SettingTheme::accentFg;
    case SA_THEME_SUCCESS: return SettingTheme::success;
    case SA_THEME_POPOVER: return SettingTheme::popover;
    case SA_THEME_INPUT: return SettingTheme::input;
    case SA_THEME_SECONDARY: return SettingTheme::secondary;
    case SA_THEME_PRIMARY: return SettingTheme::primary;
    case SA_THEME_PRIMARY_FG: return SettingTheme::primaryForeground;
    case SA_THEME_CONTENT_BORDER: return SettingTheme::contentBorder;
    default: return 0;
    }
}

int themeDark(void*)
{
    return SettingTheme::isDark() ? 1 : 0;
}

bool configBool(void*, const wchar_t* key, bool def)
{
    auto* s = Setting::get();
    return s && key ? s->getRackFlag(key, def) : def;
}

void setConfigBool(void*, const wchar_t* key, bool value)
{
    if (auto* s = Setting::get(); s && key) s->setRackFlag(key, value);
}

int configInt(void*, const wchar_t* key, int def)
{
    auto* s = Setting::get();
    return s && key ? s->getRackInt(key, def) : def;
}

void setConfigInt(void*, const wchar_t* key, int value)
{
    if (auto* s = Setting::get(); s && key) s->setRackInt(key, value);
}

int localDataGet(void*, const wchar_t* key, wchar_t* out, int capacity)
{
    auto* s = Setting::get();
    if (!s || !key) return 0;
    const std::wstring text = s->getLocalDataText(key);
    if (text.empty()) return 0;
    const int need = static_cast<int>(text.size()) + 1;
    if (out && capacity > 0) {
        const int n = std::min(static_cast<int>(text.size()), capacity - 1);
        memcpy(out, text.c_str(), static_cast<size_t>(n) * sizeof(wchar_t));
        out[n] = 0;
    }
    return need;
}

int localDataSet(void*, const wchar_t* key, const wchar_t* json)
{
    auto* s = Setting::get();
    if (!s || !key || !json) return 0;
    return s->setLocalDataText(key, json) ? 1 : 0;
}

void openSettings(void*)
{
    WinSetting::init(3);
}

void logMessage(void*, const wchar_t* message)
{
    if (message) dbg(message);
}

int historyCount(void* user)
{
    return static_cast<PluginHost*>(user)->historyCountForHost();
}

int clipboardCount(void* user)
{
    return static_cast<PluginHost*>(user)->clipboardCountForHost();
}

void openHistory(void* user)
{
    static_cast<PluginHost*>(user)->openHistoryForHost();
}

int hoverPreview(void*)
{
    auto* s = Setting::get();
    return s && s->getHoverPreview() ? 1 : 0;
}

int themeOverride(void* user)
{
    return static_cast<PluginHost*>(user)->themeOverrideForHost();
}

int rackPreviewPinned(void* user)
{
    return static_cast<PluginHost*>(user)->rackPreviewPinnedForHost() ? 1 : 0;
}

std::filesystem::path exeDir()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path();
}

} /* namespace */

/*
 * 可选导出的多文件接口：晾衣架插件通过 GetProcAddress 取全部路径，
 * 这样不用改 SnowAirEntry 的大小，旧宿主/旧插件都安全。
 */
extern "C" __declspec(dllexport) int SnowAirEntryPaths(void*, int index, wchar_t* out,
                                                       int capacity)
{
    if (!out || capacity <= 0) return 0;
    out[0] = 0;
    ensureEntries();
    if (index < 0 || index >= static_cast<int>(g_entries.size())) return 0;
    const auto& it = g_entries[static_cast<size_t>(index)];
    std::vector<std::wstring> files = it.files;
    if (files.empty() && !it.filePath.empty()) files.push_back(it.filePath);
    std::wstring joined;
    for (const auto& file : files) {
        if (!joined.empty()) joined.push_back(L'\n');
        joined += file;
    }
    wcsncpy_s(out, static_cast<size_t>(capacity), joined.c_str(), _TRUNCATE);
    return static_cast<int>(joined.size());
}

PluginHost& PluginHost::instance()
{
    static PluginHost host;
    return host;
}

int PluginHost::themeOverrideForHost() const
{
    return themeOverride_;
}

void PluginHost::init()
{
    if (plugin_) return;

    // SettingTheme is normally applied when the settings window is created.
    // If the app starts dark and settings never opened, every plugin would see
    // the light palette; apply the saved theme once before exposing theme_color.
    if (auto* setting = Setting::get()) {
        SettingTheme::apply(setting->getAppTheme());
    }

    host_.api_version = SNOWAIR_PLUGIN_API;
    host_.user = this;
    host_.theme_color = themeColor;
    host_.is_dark = themeDark;
    host_.config_get_bool = configBool;
    host_.config_set_bool = setConfigBool;
    host_.config_get_int = configInt;
    host_.config_set_int = setConfigInt;
    host_.local_data_get = localDataGet;
    host_.local_data_set = localDataSet;
    host_.open_settings = openSettings;
    host_.log = logMessage;
    host_.history_count = ::historyCount;
    host_.clipboard_count = ::clipboardCount;
    host_.open_history = ::openHistory;
    host_.hover_preview = hoverPreview;
    host_.theme_override = ::themeOverride;
    host_.rack_preview_pinned = rackPreviewPinned;
    host_.show_menu = ::showMenu;
    host_.menu_open = menuOpen;
    host_.close_menu = closeMenu;
    host_.capture_rect = captureRect;
    host_.open_clipboard_panel = openClipboardPanel;
    host_.clip_count = clipCount;
    host_.clip_item = clipItem;
    host_.clip_copy = clipCopy;
    host_.clip_delete = clipDelete;
    host_.clip_hang = clipHang;
    host_.clip_path = clipPath;
    host_.clip_meta = clipMeta;
    host_.entry_count = entryCount;
    host_.entry_refresh = entryRefresh;
    host_.entry_get = entryGet;
    host_.entry_copy = entryCopy;
    host_.entry_hang = entryHang;
    host_.entry_delete = entryDelete;
    host_.entry_reveal = entryReveal;
    host_.entry_tags_set = entryTagsSet;
    host_.entry_all_tags = entryAllTags;
    host_.entry_id = entryId;
    host_.entry_text = entryText;
    host_.entry_create_text = entryCreateText;
    host_.entry_update_text = entryUpdateText;
    host_.entry_delete_id = entryDeleteId;

    const std::filesystem::path installed =
        PluginPaths::root() / L"rack" / L"rack.dll";
    std::filesystem::path dll;
    std::error_code ec;
    if (std::filesystem::exists(installed, ec)) dll = installed;
#ifdef _DEBUG
    else {
        // 开发期便利：Release 构建不走这个固定路径，避免正式包依赖本地源码目录
        const std::filesystem::path local = L"I:\\SnowAir-Rack\\dist\\rack.dll";
        if (std::filesystem::exists(local, ec)) dll = local;
    }
#endif
    if (dll.empty()) {
        dbg(L"rack.dll not installed");
        return;
    }

    module_ = LoadLibraryW(dll.c_str());
    if (!module_) {
        dbg(L"LoadLibraryW(rack.dll) failed");
        return;
    }
    using Entry = const SnowAirPlugin* (*)();
    auto entry = reinterpret_cast<Entry>(GetProcAddress(module_, "SnowAirPluginEntry"));
    if (!entry) {
        dbg(L"SnowAirPluginEntry missing");
        FreeLibrary(module_);
        module_ = nullptr;
        return;
    }
    plugin_ = entry();
    if (!plugin_ || !plugin_->init || !plugin_->init(&host_)) {
        dbg(L"plugin init failed");
        plugin_ = nullptr;
        FreeLibrary(module_);
        module_ = nullptr;
        return;
    }
    // Peek only for the first run after a fresh install, never on ordinary
    // SnowAir starts. The installer may also drop .show-intro explicitly.
    auto* setting = Setting::get();
    const bool firstInstall =
        setting && !setting->getRackFlag(L"rack.install_seen", false);
    const auto intro = PluginPaths::root() / L"rack" / L".show-intro";
    if (firstInstall || std::filesystem::exists(intro, ec)) {
        if (plugin_->on_setting_changed) plugin_->on_setting_changed(L"rack.intro");
        if (setting) setting->setRackFlag(L"rack.install_seen", true);
        std::filesystem::remove(intro, ec);
    } else {
        dbg(L"rack plugin started silently");
    }
    dbg(L"rack plugin loaded");
}

void PluginHost::shutdown()
{
    if (plugin_) {
        if (plugin_->shutdown) plugin_->shutdown();
        plugin_ = nullptr;
    }
    if (module_) {
        FreeLibrary(module_);
        module_ = nullptr;
    }
}

void PluginHost::toggleRack()
{
    if (plugin_ && plugin_->on_setting_changed) {
        plugin_->on_setting_changed(L"rack.toggle");
    }
}

void PluginHost::onCapture(const std::wstring& path)
{
    if (plugin_ && plugin_->on_capture) {
        plugin_->on_capture(path.c_str());
    }
}

void PluginHost::hangQuickPin(const std::wstring& path)
{
    if (plugin_ && plugin_->on_quick_hang) {
        plugin_->on_quick_hang(path.c_str());
    } else {
        onCapture(path);
    }
}

namespace {
using RackQuickHangFn = int (*)(int kind, const wchar_t* text,
                                 const wchar_t* const* paths, int count);
RackQuickHangFn rackQuickHang(HMODULE module)
{
    return module ? reinterpret_cast<RackQuickHangFn>(
                        GetProcAddress(module, "SnowAirRackQuickHang"))
                  : nullptr;
}
} /* namespace */

bool PluginHost::hangQuickPinText(const std::wstring& text)
{
    const auto fn = rackQuickHang(module_);
    if (!fn) return false;
    return fn(SNOWAIR_ENTRY_TEXT, text.c_str(), nullptr, 0) != 0;
}

bool PluginHost::hangQuickPinPaths(const std::vector<std::wstring>& paths)
{
    const auto fn = rackQuickHang(module_);
    if (!fn || paths.empty()) return false;
    std::vector<const wchar_t*> raw;
    raw.reserve(paths.size());
    for (const auto& path : paths) raw.push_back(path.c_str());
    return fn(SNOWAIR_ENTRY_FILES, nullptr, raw.data(),
              static_cast<int>(raw.size())) != 0;
}

void PluginHost::onSettingChanged(const std::wstring& key)
{
    if (plugin_ && plugin_->on_setting_changed) {
        plugin_->on_setting_changed(key.c_str());
    }
}

int PluginHost::historyCount()
{
    return historyCountForHost();
}

int PluginHost::clipboardCount()
{
    return clipboardCountForHost();
}

void PluginHost::openHistory()
{
    openHistoryForHost();
}

bool PluginHost::rackInstalled() const
{
    std::error_code ec;
    return std::filesystem::exists(PluginPaths::root() / L"rack" / L"rack.dll", ec);
}

std::wstring PluginHost::rackVersion() const
{
    return plugin_ && plugin_->version ? std::wstring{ plugin_->version } : std::wstring{};
}

bool PluginHost::uninstallRack()
{
    shutdown();
    if (auto* s = Setting::get()) {
        s->setRackFlag(L"rack.intro_v2", false);
        s->setRackFlag(L"rack.install_seen", false);
    }
    std::error_code ec;
    std::filesystem::remove_all(PluginPaths::root() / L"rack", ec);
    return !ec;
}

void PluginHost::setRackPreview(bool on)
{
    rackPreview_ = on;
    onSettingChanged(L"rack.preview");
}

void PluginHost::quickHang()
{
    auto* s = Setting::get();
    if (!s || !plugin_) return;
    for (const auto& item : s->getClipboardItems()) {
        if (item.type == L"image" && !item.filePath.empty() &&
            std::filesystem::exists(item.filePath)) {
            onCapture(item.filePath);
            return;
        }
    }
    for (const auto& item : s->getHistoryItemsForUi()) {
        std::error_code ec;
        if (!item.filePath.empty() && std::filesystem::exists(item.filePath, ec)) {
            onCapture(item.filePath);
            return;
        }
    }
}

/* 插件可调用：把图片按 SnowAir 的贴图形态贴到桌面（带原生落下动效）。
   同一张图重复贴图时不再新建，而是让已有贴图抖动提示。
   导出符号给插件用 GetProcAddress 查找，旧宿主没有这个符号时插件自行降级。 */
static std::map<std::wstring, HWND> g_rackPinnedImages;

static std::wstring lowerKey(const std::wstring& text)
{
    std::wstring key = text;
    for (auto& ch : key) ch = static_cast<wchar_t>(towlower(ch));
    return key;
}

extern "C" __declspec(dllexport) void SnowAirPinImageFile(const wchar_t* path)
{
    if (!path || !*path) return;
    const std::wstring key = lowerKey(path);
    auto it = g_rackPinnedImages.find(key);
    if (it != g_rackPinnedImages.end()) {
        if (IsWindow(it->second)) {
            RECT rc{};
            GetWindowRect(it->second, &rc);
            for (int i = 0; i < 8; ++i) {
                const int offset = (i % 2 == 0) ? 7 : -7;
                SetWindowPos(it->second, nullptr, rc.left + offset, rc.top, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                Sleep(26);
            }
            SetWindowPos(it->second, nullptr, rc.left, rc.top, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            return;
        }
        g_rackPinnedImages.erase(it);
    }
    auto [w, h, px] = Util::loadImagePixels(path);
    if (w <= 0 || h <= 0 || px.empty()) return;
    RECT rc{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &rc, 0);
    const int areaW = static_cast<int>(rc.right - rc.left);
    const int areaH = static_cast<int>(rc.bottom - rc.top);
    const int x = static_cast<int>(rc.left) + (std::max)(0, (areaW - w) / 2);
    const int y = static_cast<int>(rc.top) + (std::max)(0, (areaH - h) / 2);
    if (WinPin* pin = WinPin::initFromData(x, y, w, h, px)) {
        if (pin->hwnd) g_rackPinnedImages[key] = pin->hwnd;
    }
}

/* 指定锚点（卡片正下方）：centerX 为卡片中心，topY 为卡片底边。
   图片按实际尺寸居中，文字/文件按各自默认宽度居中，并夹在工作区内。 */
extern "C" __declspec(dllexport) void SnowAirPinImageFileAt(const wchar_t* path, int centerX,
                                                           int topY)
{
    if (!path || !*path) return;
    const std::wstring key = lowerKey(path);
    auto it = g_rackPinnedImages.find(key);
    if (it != g_rackPinnedImages.end()) {
        if (IsWindow(it->second)) {
            RECT rc{};
            GetWindowRect(it->second, &rc);
            for (int i = 0; i < 8; ++i) {
                const int offset = (i % 2 == 0) ? 7 : -7;
                SetWindowPos(it->second, nullptr, rc.left + offset, rc.top, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                Sleep(26);
            }
            SetWindowPos(it->second, nullptr, rc.left, rc.top, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            return;
        }
        g_rackPinnedImages.erase(it);
    }
    auto [w, h, px] = Util::loadImagePixels(path);
    if (w <= 0 || h <= 0 || px.empty()) return;
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int minX = static_cast<int>(work.left);
    const int minY = static_cast<int>(work.top);
    const int maxX = (std::max)(minX, static_cast<int>(work.right) - w);
    const int maxY = (std::max)(minY, static_cast<int>(work.bottom) - h);
    const int x = std::clamp(centerX - w / 2, minX, maxX);
    const int y = std::clamp(topY, minY, maxY);
    if (WinPin* pin = WinPin::initFromData(x, y, w, h, px)) {
        if (pin->hwnd) g_rackPinnedImages[key] = pin->hwnd;
    }
}

extern "C" __declspec(dllexport) void SnowAirPinTextAt(const wchar_t* text, int centerX, int topY)
{
    if (!text || !*text) return;
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int minX = static_cast<int>(work.left);
    const int minY = static_cast<int>(work.top);
    const int maxX = (std::max)(minX, static_cast<int>(work.right) - 350);
    const int maxY = (std::max)(minY, static_cast<int>(work.bottom) - 150);
    const int x = std::clamp(centerX - 175, minX, maxX);
    const int y = std::clamp(topY, minY, maxY);
    ClipPin::showTextAt(text, POINT{x, y});
}

extern "C" __declspec(dllexport) void SnowAirPinFilesAt(const wchar_t* const* paths, int count,
                                                        int centerX, int topY)
{
    if (!paths || count <= 0) return;
    std::vector<std::wstring> files;
    files.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        if (paths[i] && *paths[i]) files.emplace_back(paths[i]);
    }
    if (files.empty()) return;
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int minX = static_cast<int>(work.left);
    const int minY = static_cast<int>(work.top);
    const int maxX = (std::max)(minX, static_cast<int>(work.right) - 350);
    const int maxY = (std::max)(minY, static_cast<int>(work.bottom) - 75);
    const int x = std::clamp(centerX - 175, minX, maxX);
    const int y = std::clamp(topY, minY, maxY);
    ClipPin::showFilesAt(files, POINT{x, y});
}

/* 文字 / 文件卡片的普通贴屏。图片走上面的 SnowAirPinImageFile，三者共用
   宿主原来的贴屏落下动效。 */
extern "C" __declspec(dllexport) void SnowAirPinText(const wchar_t* text)
{
    if (!text || !*text) return;
    RECT rc{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &rc, 0);
    const int areaW = static_cast<int>(rc.right - rc.left);
    const int areaH = static_cast<int>(rc.bottom - rc.top);
    ClipPin::showTextAt(text, POINT{static_cast<LONG>(rc.left) + (std::max)(0, (areaW - 350) / 2),
                                    static_cast<LONG>(rc.top) + (std::max)(0, (areaH - 150) / 2)});
}

extern "C" __declspec(dllexport) void SnowAirPinFiles(const wchar_t* const* paths, int count)
{
    if (!paths || count <= 0) return;
    std::vector<std::wstring> files;
    files.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        if (paths[i] && *paths[i]) files.emplace_back(paths[i]);
    }
    if (files.empty()) return;
    RECT rc{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &rc, 0);
    const int areaW = static_cast<int>(rc.right - rc.left);
    const int areaH = static_cast<int>(rc.bottom - rc.top);
    ClipPin::showFilesAt(files, POINT{static_cast<LONG>(rc.left) + (std::max)(0, (areaW - 350) / 2),
                                      static_cast<LONG>(rc.top) + (std::max)(0, (areaH - 75) / 2)});
}

int PluginHost::historyCountForHost()
{
    auto* s = Setting::get();
    return s ? static_cast<int>(s->getHistoryItemsForUi().size()) : 0;
}

int PluginHost::clipboardCountForHost()
{
    auto* s = Setting::get();
    return s ? static_cast<int>(s->getClipboardItems().size()) : 0;
}

void PluginHost::openHistoryForHost()
{
    WinSetting::init(5);
}

void PluginHost::previewRackTheme(int theme)
{
    themeOverride_ = std::clamp(theme, 0, 3);
    onSettingChanged(L"rack.theme");
}

void PluginHost::applyRackTheme(int theme)
{
    themeOverride_ = -1;
    if (auto* s = Setting::get()) {
        s->setRackInt(L"rack.theme", std::clamp(theme, 0, 3));
    }
    onSettingChanged(L"rack.theme");
}

void PluginHost::clearRackThemePreview()
{
    if (themeOverride_ < 0) return;
    themeOverride_ = -1;
    onSettingChanged(L"rack.theme");
}
