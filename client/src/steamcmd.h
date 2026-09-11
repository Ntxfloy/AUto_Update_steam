// steamcmd.h - SteamCMD process manager
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// objbase.h is needed for CoCreateGuid(). Older MinGW pulled it in through
// windows.h, GCC 14+ does not, so the build failed with
// "implicit declaration of function 'CoCreateGuid'".
#include <objbase.h>
#include "acf.h"

// Progress callback: called on every parsed progress line
// progress: 0.0 - 100.0
// state_desc: "downloading" | "verifying" | "committing" | "unknown"
typedef void (*SteamCmdProgressCb)(double progress, const char *state_desc, void *userdata);

// Log callback: called for every stdout line from SteamCMD
typedef void (*SteamCmdLogCb)(const char *line, void *userdata);

// Result of a SteamCMD run
typedef enum {
    STEAMCMD_SUCCESS = 0,       // "Success! App fully installed."
    STEAMCMD_ERROR_NETWORK,     // CDN timeout / rate limit
    STEAMCMD_ERROR_AUTH,        // Login failure / Steam Guard
    STEAMCMD_ERROR_PROCESS,     // Could not start process
    STEAMCMD_ERROR_TIMEOUT,     // No output for too long
    STEAMCMD_ERROR_NO_LICENSE,  // "No subscription" - account does not own the app
    STEAMCMD_ERROR_DISK,        // Disk write failure / not enough space
    STEAMCMD_ERROR_UNKNOWN      // Non-zero exit code, other
} SteamCmdResult;

// Configuration for a single SteamCMD update job
typedef struct {
    char  steamcmd_path[MAX_PATH];  // e.g. C:\steamcmd\steamcmd.exe
    char  login[128];               // Steam account login
    char  password[256];            // plaintext password (zeroed after use)
    char  app_id[32];               // e.g. "730"
    char  library_root[MAX_PATH];   // Steam library root the game belongs to
    char  install_dir[MAX_PATH];    // force_install_dir target = the GAME folder
                                    // (library\steamapps\common\installdir)
    int   is_f2p;                   // 1 = send app_license_request
    // 1 = append "validate" to app_update. This makes SteamCMD read every
    // installed file from disk and hash it, which takes minutes on a big game.
    // Only worth it for a broken/interrupted install or after a failure;
    // a normal delta update does not need it.
    int   validate;
    DWORD timeout_ms;               // max ms without output before abort (0 = no limit)

    SteamCmdProgressCb on_progress;
    SteamCmdLogCb      on_log;
    void              *userdata;
    HANDLE             job_object;  // optional: Job Object to assign SteamCMD process to
    volatile int      *abort_flag;  // optional: set to non-zero to abort the run
} SteamCmdJob;

// One entry for the build-id query below.
typedef struct {
    char app_id[32];     // in
    char buildid[32];    // out, empty if not reported
} SteamCmdAppBuild;

// Ask Valve for the CURRENT public-branch build id of one or more apps.
//
// Runs a single SteamCMD session:
//   login <acc> -> app_info_update 1 -> app_info_print <id> ... -> quit
// and parses "branches" { "public" { "buildid" "..." } } out of the output.
// This is the same data the Steam client itself uses, so it is authoritative.
//
// One login covers the whole list, so checking 20 games costs one session
// (~10-20 s) instead of 20 downloads-with-validate.
//
// Returns STEAMCMD_SUCCESS if the session ran and at least one build id was
// parsed; the caller must still check each entry for an empty buildid.
SteamCmdResult steamcmd_query_buildids(const char *steamcmd_path,
                                       const char *login,
                                       const char *password,
                                       SteamCmdAppBuild *apps, int app_count,
                                       DWORD timeout_ms,
                                       SteamCmdLogCb on_log, void *userdata,
                                       HANDLE job_object,
                                       volatile int *abort_flag);

// Run SteamCMD for one update job.
// Blocks until SteamCMD exits or timeout.
SteamCmdResult steamcmd_run(const SteamCmdJob *job);

// Human-readable name for a result code
const char *steamcmd_result_name(SteamCmdResult r);

// Kill all running steamcmd.exe processes (cleanup before starting)
void steamcmd_kill_all(void);

// Kill the Steam Client process (steam.exe) if running
// Returns 1 if it was running and killed, 0 if already closed
int  steam_client_kill(void);

// Check if steam.exe is running
int  steam_client_is_running(void);
