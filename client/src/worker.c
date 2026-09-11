// worker.c - Background update worker thread implementation
// v7: build-id pre-check (skip games that are already current), validate only
//     when it is actually needed, verbose logging of every step.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "worker.h"
#include "acf.h"
#include "steamcmd.h"
#include "catalog.h"
#include "api.h"
#include "lease.h"
#include "log.h"

// Never install on a disk smaller than this (bytes). Small system SSDs in the
// club must stay free; only manual, deliberate installs live there.
#define MIN_INSTALL_DISK_BYTES  (500ULL * 1000ULL * 1000ULL * 1000ULL)

// How many different pool accounts to try before giving up on one game.
#define MAX_ACCOUNT_ATTEMPTS 12

// If the server keeps handing out an account we already burned (it failed to
// flip it to status='bad'), give it a few chances before bailing out.
#define MAX_DUPLICATE_HANDOUTS 4

// StateFlags 4 = "fully installed". Anything else means the previous install
// was interrupted, and only then is a validate pass worth its disk I/O.
#define ACF_STATE_FULLY_INSTALLED 4

static volatile int  g_abort_requested = 0;   // stops heartbeat thread
static volatile int  g_user_abort      = 0;   // user pressed Abort
static WorkerStatus *g_status          = NULL;
static char          g_lease_path[MAX_PATH] = {0};

// ---------------------------------------------------------------------------
// Thread-safe status helpers
// ---------------------------------------------------------------------------
static void set_state(WorkerState s) {
    EnterCriticalSection(&g_status->lock);
    g_status->state = s;
    LeaveCriticalSection(&g_status->lock);
}

static void set_progress(double p, const char *desc) {
    EnterCriticalSection(&g_status->lock);
    if (p >= 0.0) g_status->progress = p;
    if (desc) {
        strncpy(g_status->state_desc, desc, 63);
        g_status->state_desc[63] = '\0';
    }
    LeaveCriticalSection(&g_status->lock);
}

// The GUI polls last_log_line 10 times a second and prints it whenever it
// differs from the previous one. Writing the same sentence again (which the
// retry loop did constantly) produced the duplicated, interleaved mess in the
// log box, so drop identical repeats right here at the source.
static void set_log(const char *line) {
    if (!line || !line[0]) return;
    EnterCriticalSection(&g_status->lock);
    if (strncmp(g_status->last_log_line, line, 511) != 0) {
        strncpy(g_status->last_log_line, line, 511);
        g_status->last_log_line[511] = '\0';
    }
    LeaveCriticalSection(&g_status->lock);
}

// Same text in the UI and in the log file / server, one call.
static void say(const char *fmt, ...) {
    char buf[480];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    set_log(buf);
    LOG_INFO("worker", "%s", buf);
}

static void set_error(const char *msg) {
    EnterCriticalSection(&g_status->lock);
    strncpy(g_status->error_msg, msg, 255);
    g_status->error_msg[255] = '\0';
    LeaveCriticalSection(&g_status->lock);
    LOG_ERROR("worker", "%s", msg);
}

static void on_progress(double pct, const char *desc, void *ud) { (void)ud; set_progress(pct, desc); }
static void on_log(const char *line, void *ud)                  { (void)ud; set_log(line); }

// ---------------------------------------------------------------------------
// Heartbeat thread: /accounts/heartbeat every 15s.
// ---------------------------------------------------------------------------
typedef struct { char lease_token[64]; char pc_id[64]; } HbArgs;

static DWORD WINAPI heartbeat_thread(LPVOID arg) {
    HbArgs *a = (HbArgs *)arg;
    int beats = 0;
    while (!g_abort_requested) {
        for (int i = 0; i < 15 && !g_abort_requested; i++) Sleep(1000);
        if (g_abort_requested) break;
        EnterCriticalSection(&g_status->lock);
        WorkerState st = g_status->state;
        LeaveCriticalSection(&g_status->lock);
        if (st != WORKER_RUNNING) break;
        int ok = api_heartbeat(a->lease_token, a->pc_id);
        beats++;
        if (!ok) LOG_WARN("api", "heartbeat #%d failed (server unreachable?)", beats);
        else     LOG_DEBUG("api", "heartbeat #%d ok", beats);
    }
    LOG_DEBUG("api", "heartbeat thread stopped after %d beat(s)", beats);
    HeapFree(GetProcessHeap(), 0, a);
    return 0;
}

// ---------------------------------------------------------------------------
// Thundering Herd jitter: random delay 1-15 seconds before acquire
// ---------------------------------------------------------------------------
static void thundering_herd_jitter(void) {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    srand((unsigned)(li.QuadPart ^ GetCurrentThreadId()));
    DWORD delay_ms = 1000 + (rand() % 14000);
    LOG_INFO("worker", "startup jitter: sleeping %lu ms before acquire",
             (unsigned long)delay_ms);
    set_log("Applying startup jitter to avoid server overload...");
    Sleep(delay_ms);
}

// ---------------------------------------------------------------------------
// Windows Job Object: kill SteamCMD if we die unexpectedly
// ---------------------------------------------------------------------------
static HANDLE create_job_object(void) {
    HANDLE job = CreateJobObjectA(NULL, NULL);
    if (!job) return NULL;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {0};
    jeli.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
        JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
    return job;
}

static void get_lease_path(char *out, int out_size) {
    GetModuleFileNameA(NULL, out, out_size);
    char *slash = strrchr(out, '\\');
    if (slash) *(slash + 1) = '\0';
    strncat(out, "lease.json", out_size - (int)strlen(out) - 1);
}

static void ensure_dir(const char *path) { CreateDirectoryA(path, NULL); }

// ---------------------------------------------------------------------------
// Install-target policy
// ---------------------------------------------------------------------------
static int pick_install_library(char *out, int out_size, char *reason, int reason_size) {
    static char roots[32][MAX_PATH];
    int n = acf_list_libraries(roots, 32);
    LOG_INFO("worker", "found %d Steam library folder(s)", n);
    if (n <= 0) {
        snprintf(reason, reason_size,
                 "No Steam library found. Is the Steam client installed on this PC?");
        return 0;
    }

    char sysdir[MAX_PATH] = {0};
    GetWindowsDirectoryA(sysdir, MAX_PATH);
    char sys_letter = sysdir[0];

    int    best = -1;
    ULONGLONG best_free = 0;
    int    rejected_system = 0, rejected_small = 0;

    for (int i = 0; i < n; i++) {
        if (GetFileAttributesA(roots[i]) == INVALID_FILE_ATTRIBUTES) {
            LOG_WARN("worker", "library %s listed but missing on disk", roots[i]);
            continue;
        }

        if (roots[i][0] && (roots[i][1] == ':') &&
            (roots[i][0] == sys_letter ||
             roots[i][0] == (char)(sys_letter | 0x20) ||
             roots[i][0] == (char)(sys_letter & ~0x20))) {
            rejected_system++;
            LOG_INFO("worker", "library %s skipped: system drive", roots[i]);
            continue;
        }

        char probe[MAX_PATH];
        snprintf(probe, MAX_PATH, "%s\\", roots[i]);
        ULARGE_INTEGER avail, total, total_free;
        if (!GetDiskFreeSpaceExA(probe, &avail, &total, &total_free)) {
            LOG_WARN("worker", "cannot read free space of %s", roots[i]);
            continue;
        }

        LOG_INFO("worker", "library %s: %.0f GB total, %.0f GB free",
                 roots[i], (double)total.QuadPart / 1e9, (double)avail.QuadPart / 1e9);

        if (total.QuadPart < MIN_INSTALL_DISK_BYTES) {
            rejected_small++;
            LOG_INFO("worker", "library %s skipped: disk under 500 GB", roots[i]);
            continue;
        }

        if (best < 0 || avail.QuadPart > best_free) {
            best = i;
            best_free = avail.QuadPart;
        }
    }

    if (best < 0) {
        snprintf(reason, reason_size,
                 "No eligible Steam library: %d on the system disk, %d on disks under 500 GB. "
                 "Create a Steam library on a big data disk (D:/E:) first.",
                 rejected_system, rejected_small);
        return 0;
    }

    strncpy(out, roots[best], out_size - 1);
    out[out_size - 1] = '\0';
    snprintf(reason, reason_size, "Install target: %s (%.0f GB free).",
             out, (double)best_free / (1024.0 * 1024.0 * 1024.0));
    return 1;
}

// ---------------------------------------------------------------------------
// Main worker thread
// ---------------------------------------------------------------------------
typedef struct { WorkerConfig cfg; } WorkerArgs;

static DWORD WINAPI worker_thread(LPVOID arg) {
    WorkerArgs   *wa  = (WorkerArgs *)arg;
    WorkerConfig *cfg = &wa->cfg;

    char    backup_path[MAX_PATH] = {0};
    AcfInfo acf = {0};

    int  installed = 0;
    char library_root[MAX_PATH] = {0};
    char installdir[256]        = {0};
    char game_path[MAX_PATH]    = {0};
    char acf_path[MAX_PATH]     = {0};
    char reason[320]            = {0};
    char local_buildid[32]      = {0};

    log_set_app(cfg->app_id);
    LOG_INFO("worker", "=== job start: app %s, pc %s, steamcmd %s ===",
             cfg->app_id, cfg->pc_id, cfg->steamcmd_path);

    if (!cfg->jitter_disabled) thundering_herd_jitter();

    // ---- 1. Resolve where this game lives (or should live) --------------
    installed = acf_find_game(cfg->app_id, &acf);

    // Validate = read and hash every installed file. That is minutes of disk
    // I/O per game, so it only happens when the install is actually suspect.
    int validate_needed = 0;

    if (installed) {
        strncpy(library_root, acf.library_root, MAX_PATH - 1);
        strncpy(installdir,   acf.installdir,   sizeof(installdir) - 1);
        strncpy(game_path,    acf.game_path,    MAX_PATH - 1);
        strncpy(acf_path,     acf.acf_path,     MAX_PATH - 1);
        strncpy(local_buildid, acf.buildid,     sizeof(local_buildid) - 1);

        EnterCriticalSection(&g_status->lock);
        strncpy(g_status->build_id_before, acf.buildid, 31);
        LeaveCriticalSection(&g_status->lock);

        int flags = atoi(acf.state_flags);
        LOG_INFO("worker", "installed: %s | buildid %s | stateflags %d | %.1f GB",
                 game_path, acf.buildid, flags,
                 atof(acf.size_on_disk) / 1e9);

        if (flags != ACF_STATE_FULLY_INSTALLED) {
            validate_needed = 1;
            say("Previous install was interrupted (StateFlags=%d) - a file check is required.",
                flags);
        }

        if (!acf_backup(&acf, backup_path)) backup_path[0] = '\0';  // non-fatal

        set_progress(0.0, "checking version");
        say("Installed in %s, local build %s.", library_root,
            local_buildid[0] ? local_buildid : "unknown");
    } else {
        const F2PGame *g = catalog_find(cfg->app_id);
        if (!g) {
            set_error("Game is not installed and is not in the built-in F2P catalog.");
            set_state(WORKER_DONE_FAIL);
            log_set_app(NULL);
            HeapFree(GetProcessHeap(), 0, wa);
            return 1;
        }
        if (!pick_install_library(library_root, MAX_PATH, reason, sizeof(reason))) {
            set_error(reason);
            set_state(WORKER_DONE_FAIL);
            log_set_app(NULL);
            HeapFree(GetProcessHeap(), 0, wa);
            return 1;
        }
        say("%s", reason);

        strncpy(installdir, g->installdir, sizeof(installdir) - 1);
        snprintf(game_path, MAX_PATH, "%s\\steamapps\\common\\%s", library_root, installdir);
        snprintf(acf_path,  MAX_PATH, "%s\\steamapps\\appmanifest_%s.acf", library_root, cfg->app_id);

        char sub[MAX_PATH];
        snprintf(sub, MAX_PATH, "%s\\steamapps", library_root);          ensure_dir(sub);
        snprintf(sub, MAX_PATH, "%s\\steamapps\\common", library_root);  ensure_dir(sub);
        ensure_dir(game_path);

        set_progress(0.0, "installing");
        say("Game is not installed - performing a fresh install into %s.", game_path);
    }

    // ---- 2. Close the Steam client + leftover SteamCMD -------------------
    set_state(WORKER_KILLING_STEAM);
    steam_client_kill();
    steamcmd_kill_all();

    api_init(cfg->server_url, cfg->api_key);
    int f2p = catalog_is_f2p(cfg->app_id);
    LOG_INFO("worker", "catalog: f2p=%d, server=%s", f2p, cfg->server_url);

    const char *owner = (!f2p && acf.last_owner[0] && acf.last_owner[0] != '0')
                        ? acf.last_owner : NULL;

    SteamCmdResult result   = STEAMCMD_ERROR_UNKNOWN;
    int            ok       = 0;
    int            aborted  = 0;
    int            skipped  = 0;
    int            tried_ids[MAX_ACCOUNT_ATTEMPTS] = {0};
    int            attempts = 0;
    int            duplicates = 0;
    char           last_err[256] = {0};

    // ---- 3..8. Try accounts until one of them actually works -------------
    while (attempts < MAX_ACCOUNT_ATTEMPTS) {
        ApiAcquireResult account = {0};

        if (g_user_abort) { aborted = 1; break; }

        set_state(WORKER_ACQUIRING);
        LOG_INFO("api", "requesting a pool account (attempt %d)", attempts + 1);
        if (!api_acquire(cfg->pc_id, cfg->app_id, owner, &account)) {
            if (attempts == 0)
                strncpy(last_err, "Failed to acquire a Steam account from the server.", 255);
            else
                snprintf(last_err, sizeof(last_err),
                         "No more usable accounts in the pool (tried %d).", attempts);
            LOG_ERROR("api", "%s", last_err);
            break;
        }

        int repeat = 0;
        for (int i = 0; i < attempts; i++)
            if (tried_ids[i] == account.account_id) repeat = 1;
        if (repeat) {
            api_release(account.lease_token, cfg->pc_id, RELEASE_FAILED,
                        cfg->app_id, "Duplicate account handed out", NULL, NULL);
            SecureZeroMemory(account.password, sizeof(account.password));
            if (++duplicates >= MAX_DUPLICATE_HANDOUTS) {
                strncpy(last_err, "Server keeps returning the same failing account.", 255);
                LOG_ERROR("api", "%s", last_err);
                break;
            }
            say("Server returned account #%d again - asking for another one...",
                account.account_id);
            Sleep(1500);
            continue;
        }
        tried_ids[attempts] = account.account_id;
        attempts++;

        say("Using pool account #%d (%s) - attempt %d of %d.",
            account.account_id, account.login, attempts, MAX_ACCOUNT_ATTEMPTS);

        // lease.json for crash recovery
        LeaseFile lf = {0};
        strncpy(lf.lease_token, account.lease_token, 63);
        strncpy(lf.pc_id,       cfg->pc_id,          63);
        snprintf(lf.account_id, sizeof(lf.account_id), "%d", account.account_id);
        strncpy(lf.app_id,      cfg->app_id,          31);
        strncpy(lf.server_url,  cfg->server_url,      511);
        strncpy(lf.api_key,     cfg->api_key,         255);
        lease_write(g_lease_path, &lf);
        SecureZeroMemory(lf.api_key, sizeof(lf.api_key));

        HANDLE job_obj = create_job_object();

        HbArgs *hb_args = (HbArgs *)HeapAlloc(GetProcessHeap(), 0, sizeof(HbArgs));
        strncpy(hb_args->lease_token, account.lease_token, 63);
        strncpy(hb_args->pc_id, cfg->pc_id, 63);
        HANDLE hb_thread = CreateThread(NULL, 0, heartbeat_thread, hb_args, 0, NULL);

        set_state(WORKER_RUNNING);

        // ---- 3a. Is there anything to download at all? ------------------
        // Ask Valve for the current public build id and compare it with the
        // one sitting in appmanifest_<id>.acf. Equal => the game is current,
        // so we skip SteamCMD entirely instead of hashing 70 GB for nothing.
        int do_update    = 1;
        int query_failed = 0;
        char remote_buildid[32] = {0};

        if (installed && !validate_needed && local_buildid[0]) {
            SteamCmdAppBuild ab;
            memset(&ab, 0, sizeof(ab));
            strncpy(ab.app_id, cfg->app_id, sizeof(ab.app_id) - 1);

            set_progress(-1.0, "checking version");
            say("Asking Valve for the current build of app %s...", cfg->app_id);

            SteamCmdResult qr = steamcmd_query_buildids(cfg->steamcmd_path,
                                                        account.login, account.password,
                                                        &ab, 1, 120000,
                                                        on_log, NULL,
                                                        job_obj, &g_user_abort);
            if (qr == STEAMCMD_SUCCESS && ab.buildid[0]) {
                strncpy(remote_buildid, ab.buildid, sizeof(remote_buildid) - 1);
                if (strcmp(remote_buildid, local_buildid) == 0) {
                    do_update = 0;
                    say("Build %s is already installed - nothing to download.", local_buildid);
                } else {
                    say("New build available: %s -> %s, downloading the delta.",
                        local_buildid, remote_buildid);
                }
            } else if (qr == STEAMCMD_ERROR_AUTH || qr == STEAMCMD_ERROR_NO_LICENSE) {
                // Bad credentials show up here already - cheap failover, we
                // have not downloaded a single byte yet.
                result       = qr;
                query_failed = 1;
                do_update    = 0;
                LOG_WARN("worker", "version check failed with %s - will try another account",
                         steamcmd_result_name(qr));
            } else {
                LOG_WARN("worker", "version check inconclusive (%s) - updating anyway",
                         steamcmd_result_name(qr));
                say("Could not read the remote build id - running a normal update.");
            }
        }

        if (do_update) {
            SteamCmdJob job = {0};
            strncpy(job.steamcmd_path, cfg->steamcmd_path, MAX_PATH - 1);
            strncpy(job.login,         account.login,      127);
            strncpy(job.password,      account.password,   255);
            strncpy(job.app_id,        cfg->app_id,        31);
            strncpy(job.library_root,  library_root,       MAX_PATH - 1);
            strncpy(job.install_dir,   game_path,          MAX_PATH - 1);
            job.is_f2p      = f2p ? 1 : 0;
            job.validate    = validate_needed;
            job.timeout_ms  = 600000;   // 10 min with no output at all = abort
            job.on_progress = on_progress;
            job.on_log      = on_log;
            job.job_object  = job_obj;
            job.abort_flag  = &g_user_abort;

            SecureZeroMemory(account.password, sizeof(account.password));

            set_progress(0.0, installed ? "updating" : "installing");
            result = steamcmd_run(&job);

            SecureZeroMemory(job.password, sizeof(job.password));
        } else {
            SecureZeroMemory(account.password, sizeof(account.password));
        }

        g_abort_requested = 1;
        if (hb_thread) { WaitForSingleObject(hb_thread, 5000); CloseHandle(hb_thread); }
        g_abort_requested = 0;
        if (job_obj) CloseHandle(job_obj);

        if (!do_update && !query_failed) {
            ok      = 1;
            skipped = 1;
            result  = STEAMCMD_SUCCESS;
        } else {
            ok = (result == STEAMCMD_SUCCESS);
        }
        aborted = g_user_abort;

        // Publish the manifest into the client library on success
        if (ok && !skipped) {
            if (!acf_import_manifest(cfg->steamcmd_path, cfg->app_id, library_root, installdir)) {
                say("Warning: could not import appmanifest into the Steam library.");
            }
        }

        char new_buildid[32] = {0};
        acf_read_field(acf_path, "buildid", new_buildid, sizeof(new_buildid));
        EnterCriticalSection(&g_status->lock);
        strncpy(g_status->build_id_after, new_buildid, 31);
        LeaveCriticalSection(&g_status->lock);
        LOG_INFO("worker", "buildid before=%s after=%s (skipped=%d)",
                 local_buildid[0] ? local_buildid : "-",
                 new_buildid[0] ? new_buildid : "-", skipped);

        set_state(WORKER_RELEASING);

        const char *release_result;
        const char *err_msg = NULL;
        if (ok) {
            release_result = RELEASE_SUCCESS;
        } else if (aborted) {
            release_result = RELEASE_INTERRUPTED;
            err_msg = "Aborted by user";
        } else {
            release_result = RELEASE_FAILED;
            err_msg = steamcmd_result_name(result);
        }

        api_release(account.lease_token, cfg->pc_id, release_result,
                    cfg->app_id, err_msg,
                    g_status->build_id_before,
                    new_buildid[0] ? new_buildid : NULL);
        LOG_INFO("api", "released account #%d as %s%s%s",
                 account.account_id, release_result,
                 err_msg ? ": " : "", err_msg ? err_msg : "");

        lease_delete(g_lease_path);

        if (ok || aborted) break;

        if (err_msg) strncpy(last_err, err_msg, 255);

        // Only a bad/limited account is worth retrying with another one.
        if (result != STEAMCMD_ERROR_AUTH && result != STEAMCMD_ERROR_NO_LICENSE) {
            LOG_ERROR("worker", "local failure (%s) - retrying with another account would not help",
                      steamcmd_result_name(result));
            break;
        }

        if (attempts >= MAX_ACCOUNT_ATTEMPTS) {
            snprintf(last_err, sizeof(last_err),
                     "%d pool accounts rejected in a row (last: %s). Check the account pool.",
                     attempts, err_msg ? err_msg : "unknown");
            LOG_ERROR("worker", "%s", last_err);
            break;
        }

        say("Account #%d rejected (%s) - marked bad on the server, taking another one (%d/%d)...",
            account.account_id, err_msg ? err_msg : "unknown",
            attempts, MAX_ACCOUNT_ATTEMPTS);
        set_progress(0.0, "retrying with another account");
        Sleep(1000);
    }

    // ---- 9. Result -------------------------------------------------------
    if (!ok) {
        if (backup_path[0]) acf_restore(acf_path, backup_path);
        set_error(last_err[0] ? last_err : "Update failed");
        set_state(WORKER_DONE_FAIL);
        LOG_ERROR("worker", "=== job FAILED for app %s after %d account(s) ===",
                  cfg->app_id, attempts);
    } else {
        set_progress(100.0, skipped ? "already up to date" : "complete");
        set_state(WORKER_DONE_OK);
        LOG_INFO("worker", "=== job OK for app %s (%s) ===",
                 cfg->app_id, skipped ? "already latest, skipped" : "updated");
    }

    // Make sure this game's lines reach the server before the next one starts.
    log_flush_remote(3000);
    log_set_app(NULL);

    HeapFree(GetProcessHeap(), 0, wa);
    return ok ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
HANDLE worker_start(const WorkerConfig *cfg, WorkerStatus *status) {
    g_status = status;
    g_abort_requested = 0;
    g_user_abort = 0;
    get_lease_path(g_lease_path, MAX_PATH);

    WorkerArgs *wa = (WorkerArgs *)HeapAlloc(GetProcessHeap(), 0, sizeof(WorkerArgs));
    wa->cfg = *cfg;

    return CreateThread(NULL, 0, worker_thread, wa, 0, NULL);
}

void worker_request_abort(void) {
    LOG_WARN("worker", "abort requested by the user");
    g_user_abort = 1;
}

int worker_check_stale_lease(const WorkerConfig *cfg) {
    char path[MAX_PATH];
    get_lease_path(path, MAX_PATH);
    api_init(cfg->server_url, cfg->api_key);
    int n = lease_recover_on_startup(path);
    if (n) LOG_WARN("worker", "recovered %d stale lease(s) from a previous crash", n);
    return n;
}
