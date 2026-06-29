// steamcmd.h - SteamCMD process manager
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
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
    STEAMCMD_ERROR_NETWORK,     // CDN timeout / no subscription
    STEAMCMD_ERROR_AUTH,        // Login failure / Steam Guard
    STEAMCMD_ERROR_PROCESS,     // Could not start process
    STEAMCMD_ERROR_TIMEOUT,     // No output for too long
    STEAMCMD_ERROR_UNKNOWN      // Non-zero exit code, other
} SteamCmdResult;

// Configuration for a single SteamCMD update job
typedef struct {
    char  steamcmd_path[MAX_PATH];  // e.g. C:\steamcmd\steamcmd.exe
    char  login[128];               // Steam account login
    char  password[256];            // plaintext password (zeroed after use)
    char  app_id[32];               // e.g. "427520"
    char  library_root[MAX_PATH];   // force_install_dir target
    int   is_f2p;                   // 1 = send app_license_request
    DWORD timeout_ms;               // max ms without output before abort (0 = no limit)

    SteamCmdProgressCb on_progress;
    SteamCmdLogCb      on_log;
    void              *userdata;
    HANDLE             job_object;  // optional: Job Object to assign SteamCMD process to
    volatile int      *abort_flag;  // optional: set to non-zero to abort the run
} SteamCmdJob;

// Run SteamCMD for one update job.
// Blocks until SteamCMD exits or timeout.
// Returns result code.
SteamCmdResult steamcmd_run(const SteamCmdJob *job);

// Kill all running steamcmd.exe processes (cleanup before starting)
void steamcmd_kill_all(void);

// Kill the Steam Client process (steam.exe) if running
// Returns 1 if it was running and killed, 0 if already closed
int  steam_client_kill(void);

// Check if steam.exe is running
int  steam_client_is_running(void);
