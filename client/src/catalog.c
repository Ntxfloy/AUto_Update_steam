// catalog.c - built-in catalog of free-to-play Steam apps
//
// size_hint is a rough download size in GB, used by the UI to warn before a
// fresh install and to mark small titles that are handy for testing.
#include <string.h>
#include "catalog.h"

static const F2PGame g_catalog[] = {
    // ---- small F2P titles, good for a first end-to-end test (<= ~2 GB) ----
    { "1782210", "Crab Game",              "Crab Game"                       },
    { "291550",  "Brawlhalla",             "Brawlhalla"                      },
    { "265630",  "Fistful of Frags",       "FistfulOfFrags"                  },
    { "700330",  "SCP: Secret Laboratory", "SCP Secret Laboratory"           },
    { "304930",  "Unturned",               "Unturned"                        },

    // ---- main club titles ----
    { "730",     "Counter-Strike 2",       "Counter-Strike Global Offensive" },
    { "578080",  "PUBG: BATTLEGROUNDS",    "PUBG"                            },
    { "1172470", "Apex Legends",           "Apex Legends"                    },
    { "570",     "Dota 2",                 "dota 2 beta"                     },
    { "440",     "Team Fortress 2",        "Team Fortress 2"                 },
    { "1085660", "Destiny 2",              "Destiny 2"                       },
    { "2073850", "THE FINALS",             "THE FINALS"                      },
    { "1938090", "Call of Duty (Warzone)", "Call of Duty HQ"                 },
    { "1203220", "NARAKA: BLADEPOINT",     "NARAKA BLADEPOINT"               },
    { "230410",  "Warframe",               "Warframe"                        },
    { "238960",  "Path of Exile",          "Path of Exile"                   },
    { "2694490", "Path of Exile 2",        "Path of Exile 2"                 },
    { "1599340", "Lost Ark",               "Lost Ark"                        },
    { "236390",  "War Thunder",            "War Thunder"                     },
    { "444090",  "Paladins",               "Paladins"                        },
    { "218230",  "PlanetSide 2",           "PlanetSide 2"                    },
    { "1097150", "Fall Guys",              "Fall Guys"                       },
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
