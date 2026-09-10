// catalog.h - built-in catalog of free-to-play Steam apps
// The club only needs F2P titles, and any pool account can update those
// (steamcmd sends app_license_request before app_update).
// installdir must match the folder name under steamapps\common. It is the
// folder we create ourselves on a fresh install, and it is written back into
// the imported appmanifest, so client and disk always agree.
#pragma once

typedef struct {
    const char *appid;
    const char *name;
    const char *installdir;
} F2PGame;

// Full built-in list. Returns pointer to a static array, fills *count.
const F2PGame *catalog_all(int *count);

// Look up one app id. Returns NULL if the app is not in the catalog.
const F2PGame *catalog_find(const char *app_id);

// 1 if the app id is a known free-to-play title.
int catalog_is_f2p(const char *app_id);
