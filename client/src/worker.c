// worker.c - Background update worker thread implementation
// v2: + lease.json crash recovery, + Windows Job Object, + Thundering Herd jitter
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "worker.h"
#include "acf.h"
#include "steamcmd.h"
#include "api.h"
#include "lease.h"

// ---------------------------------------------------------------------------
// Globals shared between worker and heartbeat thread
// ---------------------------------------------------------------------------
static volatile int    g_abort_requested = 0;  // stops heartbeat thread
static volatile int    g_user_abort      = 0;  // user pressed Abort
static char            g_lease_token[64] = {0};
static char            g_pc_id[64]       = {0};
static WorkerStatus   *g_status          = NULL;
static char            g_lease_path[MAX_PATH] = {0};  // path to lease.json

// ---------------------------------------------------------------------------
// Internal: safe status update (thread-safe)
// ---------------------------------------------------------------------------
static void set_state(WorkerState s) {
    EnterCriticalSection(&g_status->lock);
    g_status->state = s;
    LeaveCriticalSection(&g_status->lock);
}

static void set_progress(double p, const char *desc) {
    EnterCriticalSection(&g_status->lock);
    g_status->progress = p;
    if (desc) {
        strncpy(g_status->state_desc, desc, 63);
        g_status->state_desc[63] = '\0';
    }
    LeaveCriticalSection(&g_status->lock);
}

static void set_log(const char *line) {
    EnterCriticalSection(&g_status->lock);
    strncpy(g_status->last_log_line, line, 511);
    g_status->last_log_line[511] = '\0';
    LeaveCriticalSection(&g_status->lock);
}

static void set_error(const char *msg) {
    EnterCriticalSection(&g_status->lock);
    strncpy(g_status->error_msg, msg, 255);
    g_status->error_msg[255] = '\0';
    LeaveCriticalSection(&g_status->lock);
}

// ---------------------------------------------------------------------------
// SteamCMD callbacks
// ---------------------------------------------------------------------------
static void on_progress(double pct, const char *desc, void *ud) {
    (void)ud;
    set_progress(pct, desc);
}

static void on_log(const char *line, void *ud) {
    (void)ud;
    set_log(line);
}

// ---------------------------------------------------------------------------
// Known F2P App IDs (any pool account can update these)
// ---------------------------------------------------------------------------
static int is_f2p_app(const char *app_id) {
    static const char *f2p_ids[] = { "730", "578080", "252950" };  // CS2, PUBG, Rocket League
    for (int i = 0; i < (int)(sizeof(f2p_ids) / sizeof(f2p_ids[0])); i++) {
        if (strcmp(app_id, f2p_ids[i]) == 0) return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Heartbeat thread: sends /accounts/heartbeat every 30s
// ---------------------------------------------------------------------------
typedef struct { char lease_token[64]; char pc_id[64]; } HbArgs;

static DWORD WINAPI heartbeat_thread(LPVOID arg) {
    HbArgs *a = (HbArgs *)arg;
    while (!g_abort_requested) {
        Sleep(30000);
        if (g_abort_requested) break;
        EnterCriticalSection(&g_status->lock);
        WorkerState st = g_status->state;
        LeaveCriticalSection(&g_status->lock);
        if (st != WORKER_RUNNING) break;
        api_heartbeat(a->lease_token, a->pc_id);
    }
    HeapFree(GetProcessHeap(), 0, a);
    return 0;
}

// ---------------------------------------------------------------------------
// Thundering Herd jitter: random delay 1-15 seconds before acquire
// Prevents 49 PCs from hitting the server simultaneously
// ---------------------------------------------------------------------------
static void thundering_herd_jitter(void) {
    // Seed from performance counter for better randomness per-PC
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    srand((unsigned)(li.QuadPart ^ GetCurrentThreadId()));
    // Random delay: 1000ms to 15000ms
    DWORD delay_ms = 1000 + (rand() % 14000);
    set_log("Applying startup jitter to avoid server overload...");
    Sleep(delay_ms);
}

// ---------------------------------------------------------------------------
// Windows Job Object: wrap SteamCMD process
// Ensures SteamCMD is killed if our process dies unexpectedly
// ---------------------------------------------------------------------------
static HANDLE create_job_object(void) {
    HANDLE job = CreateJobObjectA(NULL, NULL);
    if (!job) return NULL;

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {0};
    jeli.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |    // kill children when job handle closed
        JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION; // don't show crash dialogs
    SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                            &jeli, sizeof(jeli));
    return job;
}

// ---------------------------------------------------------------------------
// Build lease path: same dir as .exe, named "lease.json"
// ---------------------------------------------------------------------------
static void get_lease_path(char *out, int out_size) {
    GetModuleFileNameA(NULL, out, out_size);
    char *slash = strrchr(out, '\\');
    if (slash) *(slash + 1) = '\0';
    strncat(out, "lease.json", out_size - (int)strlen(out) - 1);
}

// ---------------------------------------------------------------------------
// VDF-экранирование: каждый '\' -> '\\' (формат libraryfolders.vdf)
// ---------------------------------------------------------------------------
static void vdf_escape(const char *in, char *out, int out_size) {
    int o = 0;
    for (int i = 0; in[i] && o < out_size - 2; i++) {
        if (in[i] == '\\') { out[o++] = '\\'; out[o++] = '\\'; }
        else               { out[o++] = in[i]; }
    }
    out[o] = '\0';
}

// ---------------------------------------------------------------------------
// Прописать нативную библиотеку Steam-клиента в собственный libraryfolders.vdf
// SteamCMD (<папка steamcmd>\steamapps\libraryfolders.vdf), чтобы app_update
// нашёл уже установленную игру в библиотеке клиента и обновил её на месте.
//   steamcmd_path : полный путь к steamcmd.exe (берём его папку)
//   steam_library : корень библиотеки Steam (напр. "C:\Program Files (x86)\Steam")
// Возвращает 1 при успехе.
// ---------------------------------------------------------------------------
static int write_steamcmd_libraryfolders(const char *steamcmd_path,
                                         const char *steam_library) {
    char dir[MAX_PATH];
    strncpy(dir, steamcmd_path, MAX_PATH - 1);
    dir[MAX_PATH - 1] = '\0';
    char *slash = strrchr(dir, '\\');
    if (!slash) slash = strrchr(dir, '/');
    if (slash) *slash = '\0';
    else       dir[0] = '\0';

    char steamapps[MAX_PATH];
    snprintf(steamapps, sizeof(steamapps), "%s\\steamapps", dir);
    CreateDirectoryA(steamapps, NULL);

    char vdf_path[MAX_PATH];
    snprintf(vdf_path, sizeof(vdf_path), "%s\\libraryfolders.vdf", steamapps);

    char esc_self[MAX_PATH * 2], esc_lib[MAX_PATH * 2];
    vdf_escape(dir,           esc_self, sizeof(esc_self));
    vdf_escape(steam_library, esc_lib,  sizeof(esc_lib));

    char buf[MAX_PATH * 5];
    int n = snprintf(buf, sizeof(buf),
        "\"libraryfolders\"\r\n"
        "{\r\n"
        "\t\"0\"\r\n"
        "\t{\r\n"
        "\t\t\"path\"\t\t\"%s\"\r\n"
        "\t}\r\n"
        "\t\"1\"\r\n"
        "\t{\r\n"
        "\t\t\"path\"\t\t\"%s\"\r\n"
        "\t}\r\n"
        "}\r\n",
        esc_self, esc_lib);

    HANDLE h = CreateFileA(vdf_path, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD written = 0;
    BOOL ok = WriteFile(h, buf, (DWORD)n, &written, NULL);
    CloseHandle(h);
    return (ok && written == (DWORD)n) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Main worker thread
// ---------------------------------------------------------------------------
typedef struct { WorkerConfig cfg; } WorkerArgs;

static DWORD WINAPI worker_thread(LPVOID arg) {
    WorkerArgs   *wa  = (WorkerArgs *)arg;
    WorkerConfig *cfg = &wa->cfg;

    char backup_path[MAX_PATH] = {0};
    AcfInfo acf = {0};
    ApiAcquireResult account = {0};
    HANDLE job_obj = NULL;

    // ---- 0. Thundering Herd jitter ---------------------------------------
    // Skip if disabled (e.g. single-PC testing) or if only 1 PC in the club
    if (!cfg->jitter_disabled) {
        thundering_herd_jitter();
    }

    // ---- 1. Find the game's .acf ----------------------------------------
    if (!acf_find_game(cfg->app_id, &acf)) {
        set_error("Game not found in any Steam library.");
        set_state(WORKER_DONE_FAIL);
        HeapFree(GetProcessHeap(), 0, wa);
        return 1;
    }

    EnterCriticalSection(&g_status->lock);
    strncpy(g_status->build_id_before, acf.buildid, 31);
    LeaveCriticalSection(&g_status->lock);

    // ---- 2. Backup .acf --------------------------------------------------
    if (!acf_backup(&acf, backup_path)) {
        set_error("Failed to backup .acf manifest.");
        set_state(WORKER_DONE_FAIL);
        HeapFree(GetProcessHeap(), 0, wa);
        return 1;
    }

    // ---- 3. Kill Steam Client + leftover SteamCMD -----------------------
    set_state(WORKER_KILLING_STEAM);
    steam_client_kill();
    steamcmd_kill_all();

    // ---- 4. Acquire account from VPS ------------------------------------
    set_state(WORKER_ACQUIRING);
    api_init(cfg->server_url, cfg->api_key);

    int f2p = is_f2p_app(cfg->app_id);
    const char *owner = (!f2p && acf.last_owner[0] && acf.last_owner[0] != '0')
                        ? acf.last_owner : NULL;

    if (!api_acquire(cfg->pc_id, cfg->app_id, owner, &account)) {
        set_error("Failed to acquire Steam account from server.");
        set_state(WORKER_DONE_FAIL);
        acf_restore(acf.acf_path, backup_path);
        HeapFree(GetProcessHeap(), 0, wa);
        return 1;
    }

    strncpy(g_lease_token, account.lease_token, 63);
    strncpy(g_pc_id, cfg->pc_id, 63);

    // ---- 5. Write lease.json to disk (crash recovery) -------------------
    // If we crash after this, next startup will read this and send /release
    LeaseFile lf = {0};
    strncpy(lf.lease_token, account.lease_token, 63);
    strncpy(lf.pc_id,       cfg->pc_id,          63);
    snprintf(lf.account_id, sizeof(lf.account_id), "%d", account.account_id);
    strncpy(lf.app_id,      cfg->app_id,          31);
    strncpy(lf.server_url,  cfg->server_url,      511);
    strncpy(lf.api_key,     cfg->api_key,         255);
    lease_write(g_lease_path, &lf);
    // Zero api_key in lf after writing
    SecureZeroMemory(lf.api_key, sizeof(lf.api_key));

    // ---- 6. Create Job Object for SteamCMD (kill-on-close) -------------
    job_obj = create_job_object();

    // ---- 7. Start heartbeat thread --------------------------------------
    HbArgs *hb_args = (HbArgs *)HeapAlloc(GetProcessHeap(), 0, sizeof(HbArgs));
    strncpy(hb_args->lease_token, account.lease_token, 63);
    strncpy(hb_args->pc_id, cfg->pc_id, 63);
    HANDLE hb_thread = CreateThread(NULL, 0, heartbeat_thread, hb_args, 0, NULL);

    // ---- 8. Run SteamCMD ------------------------------------------------
    set_state(WORKER_RUNNING);

    SteamCmdJob job = {0};
    strncpy(job.steamcmd_path, cfg->steamcmd_path, MAX_PATH - 1);
    strncpy(job.login,         account.login,       127);
    strncpy(job.password,      account.password,    255);
    strncpy(job.app_id,        cfg->app_id,         31);
    // force_install_dir = ПАПКА ИГРЫ, а не корень библиотеки.
    // SteamCMD ставит контент плоско в force_install_dir, поэтому путь должен
    // быть acf.game_path (library\steamapps\common\installdir) — тогда
    // force_install_dir больше не используется (см. steamcmd.c). job.library_root
    // оставлен для совместимости со структурой, но в команду не попадает —
    // SteamCMD находит игру через libraryfolders.vdf (см. ниже).
    strncpy(job.library_root,  acf.library_root,    MAX_PATH - 1);
    job.library_root[MAX_PATH - 1] = '\0';
    job.is_f2p      = f2p ? 1 : 0;
    job.timeout_ms  = 300000; // 5 min without output = abort
    job.on_progress = on_progress;
    job.on_log      = on_log;
    job.job_object  = job_obj;   // pass Job Object to steamcmd runner
    job.abort_flag  = &g_user_abort;

    SecureZeroMemory(account.password, sizeof(account.password));

    // Регистрируем нативную библиотеку Steam-клиента в libraryfolders.vdf самого
    // SteamCMD (<папка steamcmd>\steamapps\libraryfolders.vdf). Тогда app_update
    // найдёт существующий appmanifest игры в библиотеке клиента и обновит её
    // НА МЕСТЕ (validate → только дельта), а не скачает заново в свою папку.
    // acf.library_root = корень библиотеки, напр. "C:\Program Files (x86)\Steam".
    if (!write_steamcmd_libraryfolders(cfg->steamcmd_path, acf.library_root)) {
        set_log("Warning: could not write libraryfolders.vdf; SteamCMD may re-download.");
    }

    SteamCmdResult result = steamcmd_run(&job);

    SecureZeroMemory(job.password, sizeof(job.password));

    // ---- 9. Stop heartbeat thread ----------------------------------------
    g_abort_requested = 1;
    if (hb_thread) { WaitForSingleObject(hb_thread, 5000); CloseHandle(hb_thread); }
    g_abort_requested = 0;

    // Close Job Object (kills any lingering SteamCMD children)
    if (job_obj) CloseHandle(job_obj);

    // ---- 10. Read new buildid from .acf -----------------------------------
    // Без force_install_dir SteamCMD обновляет манифест НА МЕСТЕ, прямо в
    // библиотеке клиента (acf.acf_path). Копировать ничего не нужно — Steam
    // уже видит свежий билд. Просто читаем новый buildid оттуда.
    char new_buildid[32] = {0};
    acf_read_field(acf.acf_path, "buildid", new_buildid, sizeof(new_buildid));
    EnterCriticalSection(&g_status->lock);
    strncpy(g_status->build_id_after, new_buildid, 31);
    LeaveCriticalSection(&g_status->lock);

    // ---- 11. Release account on VPS -------------------------------------
    set_state(WORKER_RELEASING);

    int ok = (result == STEAMCMD_SUCCESS);
    int aborted = g_user_abort;
    const char *release_result;
    const char *err_msg = NULL;

    if (ok) {
        release_result = RELEASE_SUCCESS;
    } else if (aborted) {
        release_result = RELEASE_INTERRUPTED;
        err_msg = "Aborted by user";
    } else {
        release_result = RELEASE_FAILED;
        switch (result) {
            case STEAMCMD_ERROR_NETWORK:  err_msg = "Network/CDN error"; break;
            case STEAMCMD_ERROR_AUTH:     err_msg = "Authentication failure"; break;
            case STEAMCMD_ERROR_TIMEOUT:  err_msg = "SteamCMD timeout"; break;
            case STEAMCMD_ERROR_PROCESS:  err_msg = "Could not start process"; break;
            default:                      err_msg = "Unknown SteamCMD error"; break;
        }
    }

    api_release(account.lease_token, cfg->pc_id,
                release_result,
                cfg->app_id, err_msg,
                g_status->build_id_before,
                new_buildid[0] ? new_buildid : NULL);

    // ---- 12. Delete lease.json (clean exit) -----------------------------
    lease_delete(g_lease_path);

    // ---- 13. On failure: restore .acf -----------------------------------
    if (!ok) {
        acf_restore(acf.acf_path, backup_path);
        if (err_msg) set_error(err_msg);
        set_state(WORKER_DONE_FAIL);
    } else {
        set_progress(100.0, "complete");
        set_state(WORKER_DONE_OK);
    }

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
    g_user_abort = 1;
}

// Called from main() on startup BEFORE showing UI
int worker_check_stale_lease(const WorkerConfig *cfg) {
    char path[MAX_PATH];
    get_lease_path(path, MAX_PATH);
    api_init(cfg->server_url, cfg->api_key);
    return lease_recover_on_startup(path);
}
