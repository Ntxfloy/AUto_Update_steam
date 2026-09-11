// acf.h - Steam .acf manifest parser and backup/restore
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// ---------------------------------------------------------------------------
// StateFlags bits from appmanifest_<appid>.acf (Valve's AppState flags).
// Only FILES_MISSING / FILES_CORRUPT justify a full validate pass; a plain
// interrupted download (UPDATE_REQUIRED / UPDATE_STARTED / UPDATE_RUNNING)
// is resumed by SteamCMD on its own without re-hashing the whole install.
// ---------------------------------------------------------------------------
#define ACF_STATE_UNINSTALLED      1
#define ACF_STATE_UPDATE_REQUIRED  2
#define ACF_STATE_FULLY_INSTALLED  4
#define ACF_STATE_UPDATE_STARTED   8
#define ACF_STATE_UPDATE_RUNNING   16
#define ACF_STATE_UPDATE_PAUSED    32
#define ACF_STATE_RECONFIGURING    64
#define ACF_STATE_VALIDATING       128
#define ACF_STATE_ADDING_FILES     256
#define ACF_STATE_FILES_MISSING    512
#define ACF_STATE_FILES_CORRUPT    1024

// Result of acf_disk_check()
#define ACF_DISK_OK         0   // fixed disk, not the Windows drive, >= 500 GB
#define ACF_DISK_SYSTEM     1   // this is the Windows drive
#define ACF_DISK_TOO_SMALL  2   // total capacity under 500 GB
#define ACF_DISK_NOT_FIXED  3   // USB stick, card reader, DVD, network share, RAM disk
#define ACF_DISK_UNUSABLE   4   // could not be queried at all

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

// Classify a path as an install target: drive type, system drive, capacity.
// Returns one of ACF_DISK_*; out_total / out_free are filled with the volume
// numbers whenever they could be read (pass NULL if not needed).
int  acf_disk_check(const char *path, ULONGLONG *out_total, ULONGLONG *out_free);

// Pick an eligible library with the most free space - used as install target
// for games that are not installed yet. Applies the same policy as
// acf_disk_check (no system drive, no removable media, >= 500 GB) and returns
// 0 when nothing qualifies. It never falls back to the system disk.
int  acf_pick_install_library(char *out, int out_size);

// Copy the appmanifest SteamCMD generated (<steamcmd dir>\steamapps\
// appmanifest_<appid>.acf) into the Steam client library and rewrite its
// "installdir" field so the client sees the game as installed.
// The swap is atomic: a temp file is written first and only moved over the
// live manifest once it is complete, so a failure can never leave the game
// without a manifest. Returns 1 on success.
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
