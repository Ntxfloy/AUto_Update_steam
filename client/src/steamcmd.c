// steamcmd.c - SteamCMD process manager implementation
// Uses the runscript file approach (reliable on Windows, unlike stdin piping).
//
// IMPORTANT ordering rule: force_install_dir must be issued BEFORE login.
// If it comes after login SteamCMD silently ignores it and installs into its
// own default library - which is exactly what caused full re-downloads.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>
#include "steamcmd.h"

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

// ---------------------------------------------------------------------------
// Internal: write the runscript file
// ---------------------------------------------------------------------------
static int write_runscript(const char *path, const SteamCmdJob *job) {
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;

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

    // Update (validate = compare local chunks, download only the delta)
    snprintf(line, sizeof(line), "app_update %s validate\r\n", job->app_id);
    WriteFile(h, line, (DWORD)strlen(line), &written, NULL);

    WriteFile(h, "quit\r\n", 6, &written, NULL);
    CloseHandle(h);
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
    if (Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, "steamcmd.exe") == 0) {
                HANDLE hp = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                if (hp) { TerminateProcess(hp, 1); CloseHandle(hp); }
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
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
    Sleep(1000);
    return 1;
}

// ---------------------------------------------------------------------------
// Main: run SteamCMD update job
// ---------------------------------------------------------------------------
SteamCmdResult steamcmd_run(const SteamCmdJob *job) {
    char script_path[MAX_PATH];
    make_runscript_path(script_path, MAX_PATH);

    if (!write_runscript(script_path, job)) {
        return STEAMCMD_ERROR_PROCESS;
    }

    char cmdline[MAX_PATH * 3 + 128];
    snprintf(cmdline, sizeof(cmdline),
             "\"%s\" +runscript \"%s\"",
             job->steamcmd_path, script_path);

    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE pipe_read  = NULL;
    HANDLE pipe_write = NULL;
    if (!CreatePipe(&pipe_read, &pipe_write, &sa, 0)) {
        DeleteFileA(script_path);
        return STEAMCMD_ERROR_PROCESS;
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
        CloseHandle(pipe_read);
        CloseHandle(pipe_write);
        if (null_stdin != INVALID_HANDLE_VALUE) CloseHandle(null_stdin);
        DeleteFileA(script_path);
        return STEAMCMD_ERROR_PROCESS;
    }

    CloseHandle(pipe_write);
    if (null_stdin != INVALID_HANDLE_VALUE) CloseHandle(null_stdin);

    if (job->job_object) {
        AssignProcessToJobObject(job->job_object, pi.hProcess);
    }

    // The runscript holds a plaintext password. SteamCMD reads it during
    // startup, so overwrite the contents as soon as the process is running
    // instead of leaving it readable in %TEMP% for the whole download.
    Sleep(1500);
    {
        HANDLE hz = CreateFileA(script_path, GENERIC_WRITE, 0, NULL,
                                TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hz != INVALID_HANDLE_VALUE) CloseHandle(hz);
    }

    char  log_buf[65536] = {0};
    int   log_pos        = 0;
    char  line_buf[4096] = {0};
    int   line_pos       = 0;
    char  read_buf[1024];
    DWORD bytes_read;
    DWORD last_output_tick = GetTickCount();
    SteamCmdResult result  = STEAMCMD_ERROR_UNKNOWN;
    int   success          = 0;

    while (1) {
        if (job->abort_flag && *job->abort_flag) {
            TerminateProcess(pi.hProcess, 1);
            result = STEAMCMD_ERROR_UNKNOWN;
            break;
        }

        if (job->timeout_ms > 0) {
            DWORD elapsed = GetTickCount() - last_output_tick;
            if (elapsed > job->timeout_ms) {
                TerminateProcess(pi.hProcess, 1);
                result = STEAMCMD_ERROR_TIMEOUT;
                break;
            }
        }

        DWORD avail = 0;
        if (!PeekNamedPipe(pipe_read, NULL, 0, NULL, &avail, NULL)) {
            DWORD exit_code = 0;
            GetExitCodeProcess(pi.hProcess, &exit_code);
            if (line_pos > 0) {
                line_buf[line_pos] = '\0';
                if (job->on_log) job->on_log(line_buf, job->userdata);
                parse_progress_line(line_buf, job->on_progress, job->userdata);
                if (is_success_line(line_buf, job->app_id)) success = 1;
                if (log_pos + line_pos + 2 < (int)sizeof(log_buf)) {
                    memcpy(log_buf + log_pos, line_buf, line_pos);
                    log_pos += line_pos;
                    log_buf[log_pos++] = '\n';
                }
                line_pos = 0;
            }
            if (success) result = STEAMCMD_SUCCESS;
            else         result = classify_error(log_buf, (int)exit_code);
            break;
        }

        if (avail == 0) {
            DWORD exit_code;
            if (GetExitCodeProcess(pi.hProcess, &exit_code) &&
                exit_code != STILL_ACTIVE) {
                while (ReadFile(pipe_read, read_buf, sizeof(read_buf)-1,
                                &bytes_read, NULL) && bytes_read > 0) {
                    for (DWORD i = 0; i < bytes_read; i++) {
                        char c = read_buf[i];
                        if (c == '\n' || c == '\r') {
                            if (line_pos > 0) {
                                line_buf[line_pos] = '\0';
                                if (job->on_log) job->on_log(line_buf, job->userdata);
                                parse_progress_line(line_buf, job->on_progress, job->userdata);
                                if (is_success_line(line_buf, job->app_id)) success = 1;
                                if (log_pos + line_pos + 2 < (int)sizeof(log_buf)) {
                                    memcpy(log_buf + log_pos, line_buf, line_pos);
                                    log_pos += line_pos;
                                    log_buf[log_pos++] = '\n';
                                }
                                line_pos = 0;
                            }
                        } else if (line_pos < (int)sizeof(line_buf) - 1) {
                            line_buf[line_pos++] = c;
                        }
                    }
                }
                if (success) result = STEAMCMD_SUCCESS;
                else result = classify_error(log_buf, (int)exit_code);
                break;
            }
            Sleep(50);
            continue;
        }

        if (!ReadFile(pipe_read, read_buf,
                      avail < sizeof(read_buf)-1 ? avail : sizeof(read_buf)-1,
                      &bytes_read, NULL) || bytes_read == 0) break;

        last_output_tick = GetTickCount();

        for (DWORD i = 0; i < bytes_read; i++) {
            char c = read_buf[i];
            if (c == '\n' || c == '\r') {
                if (line_pos > 0) {
                    line_buf[line_pos] = '\0';
                    if (job->on_log) job->on_log(line_buf, job->userdata);
                    parse_progress_line(line_buf, job->on_progress, job->userdata);
                    if (is_success_line(line_buf, job->app_id)) success = 1;
                    if (log_pos + line_pos + 2 < (int)sizeof(log_buf)) {
                        memcpy(log_buf + log_pos, line_buf, line_pos);
                        log_pos += line_pos;
                        log_buf[log_pos++] = '\n';
                    }
                    line_pos = 0;
                }
            } else if (line_pos < (int)sizeof(line_buf) - 1) {
                line_buf[line_pos++] = c;
            }
        }
    }

    CloseHandle(pipe_read);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    DeleteFileA(script_path);

    return result;
}
