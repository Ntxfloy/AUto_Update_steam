// steamcmd.c - SteamCMD process manager implementation
// Uses the runscript file approach (reliable on Windows, unlike stdin piping).
//
// IMPORTANT ordering rule: force_install_dir must be issued BEFORE login.
// If it comes after login SteamCMD silently ignores it and installs into its
// own default library - which is exactly what caused full re-downloads.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "steamcmd.h"
#include "log.h"

// ---------------------------------------------------------------------------
// Internal: generate a temp runscript file path
// ---------------------------------------------------------------------------
static void make_runscript_path(char *out, int out_size) {
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    GUID g;
    CoCreateGuid(&g);
    snprintf(out, out_size, "%ssteamcmd_run_%08lX%04X.txt",
             tmp, g.Data1, g.Data2);
}

static void wipe_runscript(const char *path) {
    HANDLE hz = CreateFileA(path, GENERIC_WRITE, 0, NULL,
                            TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hz != INVALID_HANDLE_VALUE) CloseHandle(hz);
}

// ---------------------------------------------------------------------------
// Internal: write the runscript file
// ---------------------------------------------------------------------------
static int write_runscript(const char *path, const SteamCmdJob *job) {
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        LOG_ERROR("steamcmd", "cannot create runscript %s (winerr %lu)",
                  path, GetLastError());
        return 0;
    }

    char line[MAX_PATH + 256];
    DWORD written;

    WriteFile(h, "@ShutdownOnFailedCommand 1\r\n",
              (DWORD)strlen("@ShutdownOnFailedCommand 1\r\n"), &written, NULL);
    WriteFile(h, "@NoPromptForPassword 1\r\n",
              (DWORD)strlen("@NoPromptForPassword 1\r\n"), &written, NULL);
    WriteFile(h, "@sSteamCmdForcePlatformType windows\r\n",
              (DWORD)strlen("@sSteamCmdForcePlatformType windows\r\n"), &written, NULL);

    // force_install_dir BEFORE login - this is the whole point of the fix.
    if (job->install_dir[0]) {
        snprintf(line, sizeof(line), "force_install_dir \"%s\"\r\n", job->install_dir);
        WriteFile(h, line, (DWORD)strlen(line), &written, NULL);
    }

    // Login
    snprintf(line, sizeof(line), "login %s %s\r\n", job->login, job->password);
    WriteFile(h, line, (DWORD)strlen(line), &written, NULL);
    SecureZeroMemory(line, sizeof(line));

    // F2P license request (must come after login)
    if (job->is_f2p) {
        snprintf(line, sizeof(line), "app_license_request %s\r\n", job->app_id);
        WriteFile(h, line, (DWORD)strlen(line), &written, NULL);
    }

    // Plain app_update downloads only the changed chunks of the manifest.
    // "validate" additionally reads and hashes EVERY installed file, which is
    // minutes of disk I/O per game - only do it when the caller asks.
    snprintf(line, sizeof(line), "app_update %s%s\r\n",
             job->app_id, job->validate ? " validate" : "");
    WriteFile(h, line, (DWORD)strlen(line), &written, NULL);

    WriteFile(h, "quit\r\n", 6, &written, NULL);
    CloseHandle(h);

    LOG_INFO("steamcmd", "runscript: force_install_dir=\"%s\" login=%s f2p=%d "
                         "app_update %s%s",
             job->install_dir[0] ? job->install_dir : "(default)",
             job->login, job->is_f2p, job->app_id,
             job->validate ? " validate" : "");
    return 1;
}

// ---------------------------------------------------------------------------
// Internal: parse a SteamCMD stdout line for progress
// Example: " Update state (0x61) downloading, progress: 7.36 (371405745 / 5047000673)"
// ---------------------------------------------------------------------------
static void parse_progress_line(const char *line,
                                SteamCmdProgressCb cb, void *userdata) {
    if (!cb) return;

    const char *p = strstr(line, "progress: ");
    if (!p) {
        // Stages without a percentage still deserve a UI update
        if (strstr(line, "Logging in user") || strstr(line, "Connecting anonymously"))
            cb(-1.0, "logging in", userdata);
        else if (strstr(line, "Waiting for client config"))
            cb(-1.0, "connecting", userdata);
        else if (strstr(line, "Waiting for user info"))
            cb(-1.0, "connecting", userdata);
        else if (strstr(line, "Update state") && strstr(line, "preallocating"))
            cb(-1.0, "preallocating", userdata);
        return;
    }
    p += 10;

    double pct = 0.0;
    if (sscanf(p, "%lf", &pct) != 1) return;

    const char *state = "unknown";
    if      (strstr(line, "downloading"))    state = "downloading";
    else if (strstr(line, "verifying"))      state = "verifying";
    else if (strstr(line, "validating"))     state = "validating";
    else if (strstr(line, "committing"))     state = "committing";
    else if (strstr(line, "preallocating"))  state = "preallocating";
    else if (strstr(line, "reconfiguring"))  state = "reconfiguring";
    else if (strstr(line, "0x61"))           state = "downloading";
    else if (strstr(line, "0x81"))           state = "verifying";
    else if (strstr(line, "0x101"))          state = "committing";

    cb(pct, state, userdata);
}

// ---------------------------------------------------------------------------
// Internal: success detection
// ---------------------------------------------------------------------------
static int is_success_line(const char *line, const char *app_id) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "App '%s'", app_id);
    if (!strstr(line, pattern)) return 0;

    if (strstr(line, "Success!") ||
        strstr(line, "\xD3\xF1\xEF\xE5\xF5!") ||                      // "Uspeh!" CP1251
        strstr(line, "\xD0\xA3\xD1\x81\xD0\xBF\xD0\xB5\xD1\x85!"))   // "Uspeh!" UTF-8
        return 1;

    if (strstr(line, "fully installed") || strstr(line, "already up to date")) return 1;

    return 0;
}

// ---------------------------------------------------------------------------
// Internal: classify error from the accumulated log
//
// "No subscription" is a LICENSING problem, not a network one. Reporting it as
// a CDN error hid the real cause (the account simply does not own the app),
// so the server never learned to hand out a different account.
// ---------------------------------------------------------------------------
static SteamCmdResult classify_error(const char *log, int exit_code) {
    if (strstr(log, "No subscription"))          return STEAMCMD_ERROR_NO_LICENSE;
    if (strstr(log, "Invalid Platform"))         return STEAMCMD_ERROR_NO_LICENSE;
    if (strstr(log, "Login Failure") ||
        strstr(log, "Invalid Password") ||
        strstr(log, "Account Logon Denied") ||
        strstr(log, "RateLimitExceeded"))        return STEAMCMD_ERROR_AUTH;
    if (strstr(log, "Steam Guard") ||
        strstr(log, "two-factor") ||
        strstr(log, "confirmation code"))        return STEAMCMD_ERROR_AUTH;
    if (strstr(log, "Disk write failure") ||
        strstr(log, "Not enough disk space") ||
        strstr(log, "Disk Full"))                return STEAMCMD_ERROR_DISK;
    if (strstr(log, "Timeout") ||
        strstr(log, "rate limit") ||
        strstr(log, "RateLimit") ||
        strstr(log, "Missing file privileges") ||
        strstr(log, "Update Required"))          return STEAMCMD_ERROR_NETWORK;

    if (exit_code == 0 || exit_code == 7) {
        return STEAMCMD_SUCCESS;
    }

    return STEAMCMD_ERROR_UNKNOWN;
}

const char *steamcmd_result_name(SteamCmdResult r) {
    switch (r) {
        case STEAMCMD_SUCCESS:          return "Success";
        case STEAMCMD_ERROR_NETWORK:    return "Network/CDN error";
        case STEAMCMD_ERROR_AUTH:       return "Authentication failure";
        case STEAMCMD_ERROR_PROCESS:    return "Could not start SteamCMD";
        case STEAMCMD_ERROR_TIMEOUT:    return "SteamCMD timeout";
        case STEAMCMD_ERROR_NO_LICENSE: return "Account has no license for this app";
        case STEAMCMD_ERROR_DISK:       return "Disk write failure / not enough space";
        default:                        return "Unknown SteamCMD error";
    }
}

// ---------------------------------------------------------------------------
// Kill all steamcmd.exe processes
// ---------------------------------------------------------------------------
void steamcmd_kill_all(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32 pe = { sizeof(pe) };
    int killed = 0;
    if (Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, "steamcmd.exe") == 0) {
                HANDLE hp = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                if (hp) { TerminateProcess(hp, 1); CloseHandle(hp); killed++; }
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    if (killed) LOG_WARN("steamcmd", "killed %d leftover steamcmd.exe process(es)", killed);
}

// ---------------------------------------------------------------------------
// Kill / check steam.exe client
// ---------------------------------------------------------------------------
int steam_client_is_running(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe = { sizeof(pe) };
    int found = 0;
    if (Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, "steam.exe") == 0) { found = 1; break; }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

int steam_client_kill(void) {
    if (!steam_client_is_running()) return 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe = { sizeof(pe) };
    if (Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, "steam.exe") == 0) {
                HANDLE hp = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                if (hp) { TerminateProcess(hp, 0); CloseHandle(hp); }
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    LOG_INFO("steamcmd", "steam.exe was running and has been terminated");
    Sleep(1000);
    return 1;
}

// ---------------------------------------------------------------------------
// Shared plumbing: launch SteamCMD with a runscript and stream stdout lines
// to a callback. Returns the process exit code in *exit_code_out.
// ---------------------------------------------------------------------------
typedef void (*LineSink)(const char *line, void *ud);

static int run_steamcmd_process(const char *steamcmd_path,
                                const char *script_path,
                                DWORD timeout_ms,
                                HANDLE job_object,
                                volatile int *abort_flag,
                                LineSink sink, void *sink_ud,
                                DWORD *exit_code_out,
                                int *timed_out,
                                int *aborted) {
    char cmdline[MAX_PATH * 3 + 128];
    snprintf(cmdline, sizeof(cmdline), "\"%s\" +runscript \"%s\"",
             steamcmd_path, script_path);
    LOG_DEBUG("steamcmd", "exec: %s", cmdline);

    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE pipe_read = NULL, pipe_write = NULL;
    if (!CreatePipe(&pipe_read, &pipe_write, &sa, 0)) {
        LOG_ERROR("steamcmd", "CreatePipe failed (winerr %lu)", GetLastError());
        return 0;
    }
    SetHandleInformation(pipe_read, HANDLE_FLAG_INHERIT, 0);

    HANDLE null_stdin = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    &sa, OPEN_EXISTING, 0, NULL);

    STARTUPINFOA si = { sizeof(si) };
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdOutput = pipe_write;
    si.hStdError  = pipe_write;
    si.hStdInput  = (null_stdin != INVALID_HANDLE_VALUE) ? null_stdin : NULL;

    PROCESS_INFORMATION pi = {0};
    if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        DWORD err = GetLastError();
        LOG_ERROR("steamcmd", "CreateProcess failed for %s (winerr %lu)", steamcmd_path, err);
        CloseHandle(pipe_read);
        CloseHandle(pipe_write);
        if (null_stdin != INVALID_HANDLE_VALUE) CloseHandle(null_stdin);
        return 0;
    }

    CloseHandle(pipe_write);
    if (null_stdin != INVALID_HANDLE_VALUE) CloseHandle(null_stdin);
    if (job_object) AssignProcessToJobObject(job_object, pi.hProcess);

    LOG_INFO("steamcmd", "started steamcmd.exe pid=%lu", (unsigned long)pi.dwProcessId);

    // Script will be deleted at process exit.

    char  line_buf[4096];
    int   line_pos = 0;
    char  read_buf[1024];
    DWORD bytes_read;
    DWORD last_output_tick = GetTickCount();
    DWORD exit_code = 0;

    if (timed_out) *timed_out = 0;
    if (aborted)   *aborted   = 0;

    for (;;) {
        if (abort_flag && *abort_flag) {
            LOG_WARN("steamcmd", "abort requested - terminating pid=%lu",
                     (unsigned long)pi.dwProcessId);
            TerminateProcess(pi.hProcess, 1);
            if (aborted) *aborted = 1;
            break;
        }
        if (timeout_ms > 0 && (GetTickCount() - last_output_tick) > timeout_ms) {
            LOG_ERROR("steamcmd", "no output for %lu ms - terminating pid=%lu",
                      (unsigned long)timeout_ms, (unsigned long)pi.dwProcessId);
            TerminateProcess(pi.hProcess, 1);
            if (timed_out) *timed_out = 1;
            break;
        }

        DWORD avail = 0;
        int pipe_ok = PeekNamedPipe(pipe_read, NULL, 0, NULL, &avail, NULL);
        int proc_done = 0;
        if (!pipe_ok) {
            proc_done = 1;
        } else if (avail == 0) {
            DWORD ec;
            if (GetExitCodeProcess(pi.hProcess, &ec) && ec != STILL_ACTIVE) {
                // Drain whatever is still buffered before giving up.
                while (ReadFile(pipe_read, read_buf, sizeof(read_buf) - 1,
                                &bytes_read, NULL) && bytes_read > 0) {
                    for (DWORD i = 0; i < bytes_read; i++) {
                        char c = read_buf[i];
                        if (c == '\n' || c == '\r') {
                            if (line_pos > 0) {
                                line_buf[line_pos] = '\0';
                                if (sink) sink(line_buf, sink_ud);
                                line_pos = 0;
                            }
                        } else if (line_pos < (int)sizeof(line_buf) - 1) {
                            line_buf[line_pos++] = c;
                        }
                    }
                }
                proc_done = 1;
            } else {
                Sleep(50);
                continue;
            }
        }

        if (proc_done) {
            if (line_pos > 0) {
                line_buf[line_pos] = '\0';
                if (sink) sink(line_buf, sink_ud);
                line_pos = 0;
            }
            break;
        }

        if (!ReadFile(pipe_read, read_buf,
                      avail < sizeof(read_buf) - 1 ? avail : sizeof(read_buf) - 1,
                      &bytes_read, NULL) || bytes_read == 0)
            break;

        last_output_tick = GetTickCount();
        for (DWORD i = 0; i < bytes_read; i++) {
            char c = read_buf[i];
            if (c == '\n' || c == '\r') {
                if (line_pos > 0) {
                    line_buf[line_pos] = '\0';
                    if (sink) sink(line_buf, sink_ud);
                    line_pos = 0;
                }
            } else if (line_pos < (int)sizeof(line_buf) - 1) {
                line_buf[line_pos++] = c;
            }
        }
    }

    WaitForSingleObject(pi.hProcess, 5000);
    GetExitCodeProcess(pi.hProcess, &exit_code);
    if (exit_code == STILL_ACTIVE) {
        TerminateProcess(pi.hProcess, 1);
        exit_code = 1;
    }
    if (exit_code_out) *exit_code_out = exit_code;

    LOG_INFO("steamcmd", "steamcmd.exe pid=%lu exited with code %lu",
             (unsigned long)pi.dwProcessId, (unsigned long)exit_code);

    CloseHandle(pipe_read);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return 1;
}

// ---------------------------------------------------------------------------
// Build id query: login once, print appinfo for every requested app.
// ---------------------------------------------------------------------------
typedef struct {
    SteamCmdAppBuild *apps;
    int               app_count;
    int               cur;          // index of the app block we are inside
    int               in_branches;
    int               in_public;
    int               found;
    SteamCmdLogCb     on_log;
    void             *userdata;
    char              tail[4096];   // last bytes of output, for error classification
} BuildIdParser;

// Extract the Nth double-quoted token of a line.
static int quoted_token(const char *line, int index, char *out, int out_size) {
    int n = 0;
    const char *p = line;
    while (*p) {
        if (*p != '"') { p++; continue; }
        const char *start = ++p;
        while (*p && *p != '"') p++;
        if (n == index) {
            int len = (int)(p - start);
            if (len >= out_size) len = out_size - 1;
            memcpy(out, start, (size_t)len);
            out[len] = '\0';
            return 1;
        }
        n++;
        if (*p) p++;
    }
    return 0;
}

static void buildid_sink(const char *line, void *ud) {
    BuildIdParser *st = (BuildIdParser *)ud;

    // Everything goes to the rolling log file; appinfo output is huge, so the
    // UI callback only receives the interesting lines.
    log_raw("appinfo", line);

    size_t tl = strlen(st->tail), ll = strlen(line);
    if (tl + ll + 2 < sizeof(st->tail)) {
        memcpy(st->tail + tl, line, ll);
        st->tail[tl + ll] = '\n';
        st->tail[tl + ll + 1] = '\0';
    }

    char tok[64];
    // A top-level block header is a line consisting of just "<appid>".
    if (quoted_token(line, 0, tok, sizeof(tok))) {
        for (int i = 0; i < st->app_count; i++) {
            if (strcmp(tok, st->apps[i].app_id) == 0 && !strstr(line, "\"buildid\"")) {
                st->cur         = i;
                st->in_branches = 0;
                st->in_public   = 0;
                break;
            }
        }
    }

    if (strstr(line, "\"branches\"")) { st->in_branches = 1; st->in_public = 0; return; }
    if (st->in_branches && strstr(line, "\"public\"")) { st->in_public = 1; return; }

    if (st->in_public && strstr(line, "\"buildid\"") && st->cur >= 0) {
        if (quoted_token(line, 1, tok, sizeof(tok)) && tok[0]) {
            strncpy(st->apps[st->cur].buildid, tok, sizeof(st->apps[st->cur].buildid) - 1);
            st->apps[st->cur].buildid[sizeof(st->apps[st->cur].buildid) - 1] = '\0';
            st->found++;
            LOG_INFO("appinfo", "app %s: public buildid = %s",
                     st->apps[st->cur].app_id, tok);
            if (st->on_log) {
                char msg[160];
                snprintf(msg, sizeof(msg), "Valve reports build %s for app %s",
                         tok, st->apps[st->cur].app_id);
                st->on_log(msg, st->userdata);
            }
        }
        st->in_public   = 0;
        st->in_branches = 0;
        return;
    }

    // Pass the few meaningful status lines to the UI.
    if (st->on_log &&
        (strstr(line, "Logging in user") || strstr(line, "Login Failure") ||
         strstr(line, "FAILED") || strstr(line, "Steam Guard")))
        st->on_log(line, st->userdata);
}

SteamCmdResult steamcmd_query_buildids(const char *steamcmd_path,
                                       const char *login,
                                       const char *password,
                                       SteamCmdAppBuild *apps, int app_count,
                                       DWORD timeout_ms,
                                       SteamCmdLogCb on_log, void *userdata,
                                       HANDLE job_object,
                                       volatile int *abort_flag) {
    if (!apps || app_count <= 0) return STEAMCMD_ERROR_UNKNOWN;

    char script_path[MAX_PATH];
    make_runscript_path(script_path, MAX_PATH);

    HANDLE h = CreateFileA(script_path, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        LOG_ERROR("appinfo", "cannot create runscript for the build-id query");
        return STEAMCMD_ERROR_PROCESS;
    }

    DWORD written;
    char line[512];
    WriteFile(h, "@ShutdownOnFailedCommand 1\r\n", 28, &written, NULL);
    WriteFile(h, "@NoPromptForPassword 1\r\n", 24, &written, NULL);
    snprintf(line, sizeof(line), "login %s %s\r\n", login, password);
    WriteFile(h, line, (DWORD)strlen(line), &written, NULL);
    SecureZeroMemory(line, sizeof(line));
    // Force a fresh appinfo fetch, otherwise SteamCMD may answer from a cache
    // that is hours old and we would "skip" a game that actually has an update.
    WriteFile(h, "app_info_update 1\r\n", 19, &written, NULL);
    for (int i = 0; i < app_count; i++) {
        apps[i].buildid[0] = '\0';
        snprintf(line, sizeof(line), "app_info_print %s\r\n", apps[i].app_id);
        WriteFile(h, line, (DWORD)strlen(line), &written, NULL);
    }
    WriteFile(h, "quit\r\n", 6, &written, NULL);
    CloseHandle(h);

    LOG_INFO("appinfo", "querying public build ids for %d app(s) with account %s",
             app_count, login);

    BuildIdParser st;
    memset(&st, 0, sizeof(st));
    st.apps      = apps;
    st.app_count = app_count;
    st.cur       = -1;
    st.on_log    = on_log;
    st.userdata  = userdata;

    DWORD exit_code = 0;
    int timed_out = 0, aborted = 0;
    int ok = run_steamcmd_process(steamcmd_path, script_path,
                                  timeout_ms ? timeout_ms : 120000,
                                  job_object, abort_flag,
                                  buildid_sink, &st,
                                  &exit_code, &timed_out, &aborted);
    DeleteFileA(script_path);

    if (!ok)      return STEAMCMD_ERROR_PROCESS;
    if (aborted)  return STEAMCMD_ERROR_UNKNOWN;
    if (timed_out) {
        LOG_ERROR("appinfo", "build-id query timed out");
        return STEAMCMD_ERROR_TIMEOUT;
    }

    if (st.found == 0) {
        SteamCmdResult r = classify_error(st.tail, (int)exit_code);
        LOG_ERROR("appinfo", "build-id query returned nothing (%s)",
                  steamcmd_result_name(r));
        return (r == STEAMCMD_SUCCESS) ? STEAMCMD_ERROR_UNKNOWN : r;
    }

    LOG_INFO("appinfo", "build-id query done: %d of %d app(s) answered",
             st.found, app_count);
    return STEAMCMD_SUCCESS;
}

// ---------------------------------------------------------------------------
// Main: run SteamCMD update job
// ---------------------------------------------------------------------------
typedef struct {
    const SteamCmdJob *job;
    char  log_buf[65536];
    int   log_pos;
    int   success;
    int   lines;
} UpdateSink;

static void update_sink(const char *line, void *ud) {
    UpdateSink *st = (UpdateSink *)ud;
    const SteamCmdJob *job = st->job;

    st->lines++;
    // Every single SteamCMD line lands in the rolling log file, verbatim.
    log_raw("steamcmd", line);

    if (job->on_log) job->on_log(line, job->userdata);
    parse_progress_line(line, job->on_progress, job->userdata);
    if (is_success_line(line, job->app_id)) st->success = 1;

    int len = (int)strlen(line);
    if (st->log_pos + len + 2 < (int)sizeof(st->log_buf)) {
        memcpy(st->log_buf + st->log_pos, line, (size_t)len);
        st->log_pos += len;
        st->log_buf[st->log_pos++] = '\n';
        st->log_buf[st->log_pos]   = '\0';
    }
}

SteamCmdResult steamcmd_run(const SteamCmdJob *job) {
    char script_path[MAX_PATH];
    make_runscript_path(script_path, MAX_PATH);

    if (!write_runscript(script_path, job)) {
        return STEAMCMD_ERROR_PROCESS;
    }

    UpdateSink *st = (UpdateSink *)calloc(1, sizeof(UpdateSink));
    if (!st) { DeleteFileA(script_path); return STEAMCMD_ERROR_PROCESS; }
    st->job = job;

    DWORD exit_code = 0;
    int timed_out = 0, aborted = 0;
    DWORD t0 = GetTickCount();
    int ok = run_steamcmd_process(job->steamcmd_path, script_path,
                                  job->timeout_ms, job->job_object, job->abort_flag,
                                  update_sink, st,
                                  &exit_code, &timed_out, &aborted);
    DeleteFileA(script_path);

    SteamCmdResult result;
    if (!ok)             result = STEAMCMD_ERROR_PROCESS;
    else if (aborted)    result = STEAMCMD_ERROR_UNKNOWN;
    else if (timed_out)  result = STEAMCMD_ERROR_TIMEOUT;
    else if (st->success) result = STEAMCMD_SUCCESS;
    else                 result = classify_error(st->log_buf, (int)exit_code);

    LOG_INFO("steamcmd", "app %s finished: %s (exit=%lu, %d output lines, %lu s)",
             job->app_id, steamcmd_result_name(result),
             (unsigned long)exit_code, st->lines,
             (unsigned long)((GetTickCount() - t0) / 1000));

    // On failure keep the tail of the raw output in the log for debugging.
    if (result != STEAMCMD_SUCCESS && st->log_pos > 0) {
        int from = st->log_pos > 2000 ? st->log_pos - 2000 : 0;
        LOG_ERROR("steamcmd", "last output of the failed run:\n%s", st->log_buf + from);
    }

    free(st);
    return result;
}
