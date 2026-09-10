// acf.h - Steam .acf manifest parser and backup/restore
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Parsed fields from an appmanifest_XXXX.acf file
typedef struct {
    char appid[32];
    char name[256];
    char installdir[256];
    char buildid[32];
    char state_flags[16];
    char last_owner[32];    // SteamID64
    char size_on_disk[32];
    char library_root[MAX_PATH];   // e.g. C:\Program Files (x86)\Steam
    char acf_path[MAX_PATH];       // full path to the .acf file
    char game_path[MAX_PATH];      // library_root\steamapps\common\installdir
} AcfInfo;

// Resolve the Steam client install root.
// Order: HKLM\SOFTWARE\WOW6432Node\Valve\Steam!InstallPath (machine-wide, works
// under SYSTEM / Task Scheduler), then the 32/64-bit views of HKLM\SOFTWARE\
// Valve\Steam, then HKCU\Software\Valve\Steam!SteamPath, then common locations
// on disk. Returns 0 if Steam could not be found (never silently guesses).
int  acf_get_steam_root(char *out, int out_size);

// List every Steam library root (client root + libraryfolders.vdf entries).
// Returns the number of roots written into roots[].
int  acf_list_libraries(char roots[][MAX_PATH], int max_roots);

// Pick the library with the most free space - used as install target for games
// that are not installed yet. Returns 0 if no library was found.
int  acf_pick_install_library(char *out, int out_size);

// Copy the appmanifest SteamCMD generated (<steamcmd dir>\steamapps\
// appmanifest_<appid>.acf) into the Steam client library and rewrite its
// "installdir" field so the client sees the game as installed.
// Returns 1 on success.
int  acf_import_manifest(const char *steamcmd_path, const char *app_id,
                         const char *target_library, const char *installdir);

// Find the Steam library root from registry, then scan all libraries
// Returns number of games found, fills out[] (max_out entries)
int  acf_scan_libraries(AcfInfo *out, int max_out);

// Find one specific game by app_id across all libraries
// Returns 1 on success, 0 if not found
int  acf_find_game(const char *app_id, AcfInfo *out);

// Read a single field from a .acf file (VDF format)
// Returns 1 on success
int  acf_read_field(const char *acf_path, const char *field, char *out, int out_size);

// Backup a .acf file to %TEMP%\acf_backup_APPID_TIMESTAMP.acf
// Fills backup_path (MAX_PATH)
// Returns 1 on success
int  acf_backup(const AcfInfo *info, char *backup_path);

// Restore a .acf file from a backup path
int  acf_restore(const char *acf_path, const char *backup_path);

// Parse a .acf file into AcfInfo struct
int  acf_parse(const char *acf_path, const char *library_root, AcfInfo *out);
