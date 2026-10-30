/*
 * SnowAir plugin ABI
 * ------------------
 * This header is the contract between SnowAir.exe (the host) and a plugin DLL.
 * Keep a synced copy in the host repository:
 *   Native/Src/Plugin/SnowAirPlugin.h
 *
 * Rules:
 *   - Pure C types only. Never pass STL containers or C++ objects across the
 *     module boundary.
 *   - `api_version` is checked in both directions. A mismatch means "refuse to
 *     load", not "try anyway".
 *   - Evolution is additive: new fields are appended at the end of a struct.
 *     Bump SNOWAIR_PLUGIN_API for breaking changes.
 *   - Every callback is invoked on the host UI thread.
 */
#ifndef SNOWAIR_PLUGIN_H
#define SNOWAIR_PLUGIN_H

#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif
#include <wchar.h>

#define SNOWAIR_PLUGIN_API 3

/*
 * The plugin is loaded with GetProcAddress("SnowAirPluginEntry"), so it does not
 * need an import library. Define SNOWAIR_PLUGIN_EXPORTS while building the DLL
 * to export the entry point; the host can simply not define it.
 */
#ifndef SNOWAIR_PLUGIN_EXPORT
#  if defined(SNOWAIR_PLUGIN_EXPORTS)
#    define SNOWAIR_PLUGIN_EXPORT __declspec(dllexport)
#  else
#    define SNOWAIR_PLUGIN_EXPORT
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Theme roles. Colours are 0xRRGGBBAA. */
enum SnowAirThemeRole {
    SA_THEME_FG = 0,          /* primary text */
    SA_THEME_FG_SECONDARY,
    SA_THEME_FG_TERTIARY,
    SA_THEME_BG,              /* card / panel background */
    SA_THEME_CANVAS,          /* window background */
    SA_THEME_BORDER,
    SA_THEME_ACCENT,          /* hover / selection */
    SA_THEME_ACCENT_FG,
    SA_THEME_SUCCESS,
    SA_THEME_POPOVER,         /* filter bubble / popover surface */
    SA_THEME_INPUT,           /* input / control background */
    SA_THEME_SECONDARY,       /* segmented track */
    SA_THEME_PRIMARY,         /* selected segment */
    SA_THEME_PRIMARY_FG,
    SA_THEME_CONTENT_BORDER,  /* settings card outline */
    SA_THEME_COUNT
};

typedef void (*SnowAirMenuSelect)(void* plugin_user, int id);

typedef struct SnowAirMenuItem {
    const wchar_t* label;
    int id;
    int flags;
    const struct SnowAirMenuItem* submenu;
    int submenu_count;
} SnowAirMenuItem;

enum {
    SNOWAIR_MENU_SEPARATOR = 1,
    SNOWAIR_MENU_CHECKED = 2,
    SNOWAIR_MENU_DISABLED = 4
};

/* Unified entries for the envelope panel: SnowAir's own history (screenshots,
 * screen recordings and user files found in the history folders) merged with
 * the clipboard history, newest first. */
enum SnowAirEntryKind {
    SNOWAIR_ENTRY_TEXT = 0,   /* clipboard text */
    SNOWAIR_ENTRY_IMAGE = 1,  /* bitmap or image file */
    SNOWAIR_ENTRY_FILES = 2,  /* files / folders (videos included) */
    SNOWAIR_ENTRY_MEDIA = 3   /* screen recording (video, has a cover thumb) */
};

enum SnowAirEntrySource {
    SNOWAIR_SOURCE_CLIPBOARD = 0,
    SNOWAIR_SOURCE_CAPTURE = 1,  /* produced by SnowAir's screenshot flow */
    SNOWAIR_SOURCE_RECORD = 2,   /* produced by SnowAir's recorder */
    SNOWAIR_SOURCE_OTHER = 3     /* user files already in the history folders */
};

#define SNOWAIR_ENTRY_LABEL_MAX 512
#define SNOWAIR_ENTRY_PATH_MAX 1024
#define SNOWAIR_ENTRY_TAGS_MAX 768

typedef struct SnowAirEntry {
    int kind;                 /* SnowAirEntryKind */
    int source;               /* SnowAirEntrySource */
    int width;
    int height;
    int duration_sec;         /* recordings only, 0 otherwise */
    long long created_at;     /* Unix epoch milliseconds */
    wchar_t label[SNOWAIR_ENTRY_LABEL_MAX];  /* title: text preview / file name */
    wchar_t path[SNOWAIR_ENTRY_PATH_MAX];    /* file path, may be empty for text */
    wchar_t thumb[SNOWAIR_ENTRY_PATH_MAX];   /* preferred thumbnail, may be empty */
    wchar_t tags[SNOWAIR_ENTRY_TAGS_MAX];    /* custom tags, '\n' separated */
} SnowAirEntry;

typedef struct SnowAirHost {
    uint32_t api_version;                 /* == SNOWAIR_PLUGIN_API */
    void*    user;                        /* host-side context, passed back verbatim */

    /* Theme. Returns 0xRRGGBBAA. */
    uint32_t (*theme_color)(void* user, int role);
    int      (*is_dark)(void* user);

    /*
     * Config. Values live in SnowAir's config.json under features.<key>.
     * Keys use the "rack." prefix, e.g. "rack.enabled".
     */
    bool (*config_get_bool)(void* user, const wchar_t* key, bool def);
    void (*config_set_bool)(void* user, const wchar_t* key, bool value);
    int  (*config_get_int)(void* user, const wchar_t* key, int def);
    void (*config_set_int)(void* user, const wchar_t* key, int value);

    /* Open SnowAir settings at Appearance -> Other appearance. */
    void (*open_settings)(void* user);

    /* Optional debug log. The host owns the destination. */
    void (*log)(void* user, const wchar_t* message);

    /* Host counts/actions for the rack accessories. */
    int  (*history_count)(void* user);
    int  (*clipboard_count)(void* user);
    void (*open_history)(void* user);

    /* True when Appearance -> Other -> hover preview is enabled. */
    int  (*hover_preview)(void* user);

    /* Temporary theme preview (-1 = use saved rack.theme). */
    int  (*theme_override)(void* user);

    /* True while the appearance page is previewing the rack live. */
    int  (*rack_preview_pinned)(void* user);

    /* Show SnowAir's native popup menu (same PinMenu used by pin windows).
       Nested submenus are supported; `on_select` is called with plugin_user. */
    void (*show_menu)(void* user, const SnowAirMenuItem* items, int count,
                      int screen_x, int screen_y, void* plugin_user,
                      SnowAirMenuSelect on_select);

    /* True while a SnowAir popup menu (including one opened for the rack) is up. */
    int (*menu_open)(void* user);

    /* Close any host popup menu (and remove its low-level mouse hook). */
    void (*close_menu)(void* user);

    /* Screen rectangle of the last screenshot region, if known. */
    int (*capture_rect)(void* user, int* x, int* y, int* w, int* h);

    /* Open SnowAir's standalone clipboard-history popup at a screen point. */
    void (*open_clipboard_panel)(void* user, int screen_x, int screen_y);

    /* Clipboard history exposed to the rack's envelope bubble.
       type: 0 = text, 1 = image, 2 = files. */
    int (*clip_count)(void* user);
    int (*clip_item)(void* user, int index, wchar_t* label, int capacity, int* type);
    void (*clip_copy)(void* user, int index);
    void (*clip_delete)(void* user, int index);
    int (*clip_hang)(void* user, int index);   /* 1 = handled */
    int (*clip_path)(void* user, int index, wchar_t* path, int capacity);
    int (*clip_meta)(void* user, int index, wchar_t* meta, int capacity);

    /* Envelope panel entries (history + clipboard, newest first). The host
       owns the snapshot; call entry_refresh after changing anything. */
    int  (*entry_count)(void* user);
    void (*entry_refresh)(void* user);
    int  (*entry_get)(void* user, int index, SnowAirEntry* entry);
    void (*entry_copy)(void* user, int index);
    int  (*entry_hang)(void* user, int index);      /* 1 = hung on the rack */
    void (*entry_delete)(void* user, int index);    /* deletes history record too */
    void (*entry_reveal)(void* user, int index);    /* show the file in Explorer */
    void (*entry_tags_set)(void* user, int index, const wchar_t* tags);
    int  (*entry_all_tags)(void* user, wchar_t* out, int capacity);
} SnowAirHost;

/* Settings item kind. */
enum SnowAirSettingType {
    SA_SET_BOOL = 0,
    SA_SET_INT,
    SA_SET_CHOICE,        /* value is still int; label comes from choice_labels */
    SA_SET_COLOR
};

typedef struct SnowAirSettingDesc {
    const wchar_t* key;               /* "rack.hover_delay_ms" */
    const wchar_t* label;             /* "Hover delay" */
    const wchar_t* desc;              /* optional second line */
    int   type;
    int   default_bool;               /* or default_int */
    int   min_value;
    int   max_value;
    const wchar_t* const* choice_labels;
    int   choice_count;
} SnowAirSettingDesc;

typedef struct SnowAirPlugin {
    uint32_t api_version;
    const wchar_t* name;               /* "Rack" */
    const wchar_t* version;            /* "0.1.0" */
    const wchar_t* id;                 /* "rack" -- config namespace + identity */

    bool (*init)(const SnowAirHost* host);
    void (*shutdown)(void);

    /* Called after SnowAir has written a capture to disk.
     * `png_path` is only valid for the duration of this call. */
    void (*on_capture)(const wchar_t* png_path);

    /* Manual "quick pin to rack" path; ignores autoCapture. */
    void (*on_quick_hang)(const wchar_t* png_path);

    /* Settings declarations, rendered by the host. */
    int  (*settings_count)(void);
    bool (*settings_get)(int index, SnowAirSettingDesc* out);

    /* The host calls this after changing a value, so the plugin can apply it
     * immediately. `key` is the same string declared above. */
    void (*on_setting_changed)(const wchar_t* key);
} SnowAirPlugin;

SNOWAIR_PLUGIN_EXPORT const SnowAirPlugin* SnowAirPluginEntry(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SNOWAIR_PLUGIN_H */
