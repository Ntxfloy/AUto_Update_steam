// catalog.c - built-in catalog of free-to-play Steam apps
//
// size_gb is a rough install size in GB. It is deliberately approximate: the
// worker uses it only to make sure the target disk can hold the title (plus
// 10% and 5 GB of air) and to warn before a fresh install. Exact numbers come
// from the appmanifest once the game is on disk.
#include <string.h>
#include "catalog.h"

static const F2PGame g_catalog[] = {
    // ---- small F2P titles, good for a first end-to-end test (<= ~4 GB) ----
    { "1782210", "Crab Game",              "Crab Game",                        1 },
    { "291550",  "Brawlhalla",             "Brawlhalla",                       2 },
    { "265630",  "Fistful of Frags",       "FistfulOfFrags",                   2 },
    { "700330",  "SCP: Secret Laboratory", "SCP Secret Laboratory",            2 },
    { "304930",  "Unturned",               "Unturned",                         4 },

    // ---- main club titles ----
    { "730",     "Counter-Strike 2",       "Counter-Strike Global Offensive", 40 },
    { "578080",  "PUBG: BATTLEGROUNDS",    "PUBG",                            50 },
    { "1172470", "Apex Legends",           "Apex Legends",                    80 },
    { "570",     "Dota 2",                 "dota 2 beta",                     75 },
    { "440",     "Team Fortress 2",        "Team Fortress 2",                 25 },
    { "1085660", "Destiny 2",              "Destiny 2",                      105 },
    { "2073850", "THE FINALS",             "THE FINALS",                      35 },
    { "1938090", "Call of Duty (Warzone)", "Call of Duty HQ",                120 },
    { "1203220", "NARAKA: BLADEPOINT",     "NARAKA BLADEPOINT",               40 },
    { "230410",  "Warframe",               "Warframe",                        60 },
    { "238960",  "Path of Exile",          "Path of Exile",                   40 },
    { "2694490", "Path of Exile 2",        "Path of Exile 2",                 60 },
    { "1599340", "Lost Ark",               "Lost Ark",                       100 },
    { "236390",  "War Thunder",            "War Thunder",                    100 },
    { "444090",  "Paladins",               "Paladins",                        30 },
    { "218230",  "PlanetSide 2",           "PlanetSide 2",                    20 },
    { "1097150", "Fall Guys",              "Fall Guys",                       15 },
};

// App ids that are small enough to use as a smoke test (< ~4 GB).
static const char *g_small_ids[] = {
    "1782210", "291550", "265630", "700330", "304930",
};

const F2PGame *catalog_all(int *count) {
    if (count) *count = (int)(sizeof(g_catalog) / sizeof(g_catalog[0]));
    return g_catalog;
}

const F2PGame *catalog_find(const char *app_id) {
    if (!app_id || !app_id[0]) return NULL;
    int n = (int)(sizeof(g_catalog) / sizeof(g_catalog[0]));
    for (int i = 0; i < n; i++) {
        if (strcmp(g_catalog[i].appid, app_id) == 0) return &g_catalog[i];
    }
    return NULL;
}

int catalog_is_f2p(const char *app_id) {
    return catalog_find(app_id) != NULL;
}

int catalog_is_small(const char *app_id) {
    if (!app_id || !app_id[0]) return 0;
    int n = (int)(sizeof(g_small_ids) / sizeof(g_small_ids[0]));
    for (int i = 0; i < n; i++) {
        if (strcmp(g_small_ids[i], app_id) == 0) return 1;
    }
    return 0;
}

int catalog_size_gb(const char *app_id) {
    const F2PGame *g = catalog_find(app_id);
    return g ? g->size_gb : 0;
}

int catalog_is_huge(const char *app_id) {
    const F2PGame *g = catalog_find(app_id);
    return (g && g->size_gb >= CATALOG_HUGE_GB) ? 1 : 0;
}
