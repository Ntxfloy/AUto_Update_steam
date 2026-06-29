// worker.h - Background update worker thread
// Orchestrates the full update flow:
//   1. Kill steam.exe
//   2. Backup .acf
//   3. Acquire account from VPS
//   4. Run SteamCMD (with heartbeat thread)
//   5. Release account
//   6. Restore .acf on failure
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Worker state (read from UI thread, written by worker thread)
typedef enum {
    WORKER_IDLE = 0,
    WORKER_ACQUIRING,       // Getting account from VPS
    WORKER_KILLING_STEAM,   // Closing steam.exe
    WORKER_RUNNING,         // SteamCMD is active
    WORKER_RELEASING,       // Releasing account on VPS
    WORKER_DONE_OK,         // Finished successfully
    WORKER_DONE_FAIL        // Finished with error
} WorkerState;

// Worker configuration (set before starting)
typedef struct {
    char server_url[512];
    char api_key[256];
    char pc_id[64];
    char app_id[32];
    char steamcmd_path[MAX_PATH];
    int  jitter_disabled;   // 1 = skip Thundering Herd delay (for single-PC testing)
} WorkerConfig;

// Worker status (read from UI thread)
typedef struct {
    WorkerState state;
    double      progress;           // 0.0 - 100.0
    char        state_desc[64];     // "downloading" etc
    char        last_log_line[512];
    char        error_msg[256];     // set on DONE_FAIL
    char        build_id_before[32];
    char        build_id_after[32];
    CRITICAL_SECTION lock;          // protect all fields above
} WorkerStatus;

// Start a background update job for one app
// Returns thread handle (CloseHandle when done)
HANDLE worker_start(const WorkerConfig *cfg, WorkerStatus *status);

// Request graceful abort (will try to finish heartbeat + release)
void worker_request_abort(void);

// Call on startup BEFORE showing UI:
// if lease.json exists from a previous crash, sends /release and deletes it
// Returns 1 if stale lease was found and released
int  worker_check_stale_lease(const WorkerConfig *cfg);
