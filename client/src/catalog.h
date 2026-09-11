// catalog.h - built-in catalog of free-to-play Steam apps
// The club only needs F2P titles, and any pool account can update those
// (steamcmd sends app_license_request before app_update).
// installdir must match the folder name under steamapps\common. It is the
// folder we create ourselves on a fresh install, and it is written back into
// the imported appmanifest, so client and disk always agree.
#pragma once

// A title is "huge" when it ships as a handful of giant archives. Those get
// repacked by their publishers, and then even the Steam client re-downloads
// almost everything - so a full-size download on them is not a bug.
#define CATALOG_HUGE_GB 20

typedef struct {
    const char *appid;
    const char *name;
    const char *installdir;
    int         size_gb;     // rough install size, used for disk checks / warnings
} F2PGame;

// Full built-in list. Returns pointer to a static array, fills *count.
const F2PGame *catalog_all(int *count);

// Look up one app id. Returns NULL if the app is not in the catalog.
const F2PGame *catalog_find(const char *app_id);

// 1 if the app id is a known free-to-play title.
int catalog_is_f2p(const char *app_id);

// 1 if the title is small enough (< ~4 GB) to be a sane smoke test.
int catalog_is_small(const char *app_id);

// 1 if the title is >= CATALOG_HUGE_GB. Handy for "do not tick these by
// default" logic in the UI and for pre-flight warnings.
int catalog_is_huge(const char *app_id);

// Rough install size in GB, 0 when the app is not in the catalog.
int catalog_size_gb(const char *app_id);
