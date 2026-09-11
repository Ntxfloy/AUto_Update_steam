// log.c - implementation of the verbose client logger.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <objbase.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include "log.h"

#define LOG_MAX_FILE_BYTES   (5 * 1024 * 1024)
#define LOG_KEEP_FILES       3
#define LOG_QUEUE_CAPACITY   4096     // ~2 MB of RAM worst case
#define LOG_BATCH_MAX        150
#define LOG_UPLOAD_PERIOD_MS 3000
#define LOG_TEXT_MAX         480

typedef struct {
    char  ts[32];
    char  module[24];
    char  app_id[16];
    int   level;
    DWORD thread_id;
    char  text[LOG_TEXT_MAX];
} LogEntry;

static CRITICAL_SECTION g_cs;
static int   g_inited        = 0;
static HANDLE g_file         = INVALID_HANDLE_VALUE;
static char  g_path[MAX_PATH] = {0};
static LogLevel g_min_level  = LOG_LEVEL_DEBUG;
static LogLevel g_remote_level = LOG_LEVEL_DEBUG;
static char  g_pc_id[64]     = "unknown";
static char  g_app_id[16]    = {0};
static char  g_session[48]   = {0};

// remote
static int   g_remote_on     = 0;
static char  g_api_key[256]  = {0};
static char  g_host[256]     = {0};
static INTERNET_PORT g_port  = 8000;
static int   g_https         = 0;
static HANDLE g_thread       = NULL;
static HANDLE g_wake         = NULL;
static volatile LONG g_stop  = 0;
static volatile LONG g_flush_done = 0;

// queue (single producer-agnostic ring, guarded by g_cs)
static LogEntry *g_queue     = NULL;
static int   g_q_head        = 0;   // next read
static int   g_q_count       = 0;
static long  g_dropped       = 0;

static const char *level_name(int lvl) {
    switch (lvl) {
        case LOG_LEVEL_DEBUG: return "DEBUG";
        case LOG_LEVEL_INFO:  return "INFO";
        case LOG_LEVEL_WARN:  return "WARN";
        default:              return "ERROR";
    }
}

static void now_stamp(char *out, int size) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(out, size, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
             st.wSecond, st.wMilliseconds);
}

// ---------------------------------------------------------------------------
// File handling with rotation
// ---------------------------------------------------------------------------
static void open_file(void) {
    g_file = CreateFileA(g_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

static void rotate_if_needed(void) {
    if (g_file == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(g_file, &sz)) return;
    if (sz.QuadPart < LOG_MAX_FILE_BYTES) return;

    CloseHandle(g_file);
    g_file = INVALID_HANDLE_VALUE;

    char from[MAX_PATH + 8], to[MAX_PATH + 8];
    snprintf(to, sizeof(to), "%s.%d", g_path, LOG_KEEP_FILES);
    DeleteFileA(to);
    for (int i = LOG_KEEP_FILES - 1; i >= 1; i--) {
        snprintf(from, sizeof(from), "%s.%d", g_path, i);
        snprintf(to,   sizeof(to),   "%s.%d", g_path, i + 1);
        MoveFileA(from, to);
    }
    snprintf(to, sizeof(to), "%s.1", g_path);
    MoveFileA(g_path, to);
    open_file();
}

static void write_file_line(const LogEntry *e) {
    if (g_file == INVALID_HANDLE_VALUE) return;
    char line[LOG_TEXT_MAX + 160];
    int n = snprintf(line, sizeof(line), "%s [%-5s] [%-8s] [tid:%lu]%s%s %s\r\n",
                     e->ts, level_name(e->level), e->module,
                     (unsigned long)e->thread_id,
                     e->app_id[0] ? " app:" : "", e->app_id[0] ? e->app_id : "",
                     e->text);
    if (n < 0) return;
    DWORD written = 0;
    WriteFile(g_file, line, (DWORD)strlen(line), &written, NULL);
    rotate_if_needed();
}

// ---------------------------------------------------------------------------
// Queue
// ---------------------------------------------------------------------------
static void enqueue(const LogEntry *e) {
    if (!g_queue) return;
    if (g_q_count == LOG_QUEUE_CAPACITY) {
        // Server is down or slow: drop the oldest line, never block the worker.
        g_q_head = (g_q_head + 1) % LOG_QUEUE_CAPACITY;
        g_q_count--;
        g_dropped++;
    }
    int idx = (g_q_head + g_q_count) % LOG_QUEUE_CAPACITY;
    g_queue[idx] = *e;
    g_q_count++;
}

int log_pending_count(void) {
    if (!g_inited) return 0;
    EnterCriticalSection(&g_cs);
    int n = g_q_count;
    LeaveCriticalSection(&g_cs);
    return n;
}

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------
static void json_escape(const char *in, char *out, int out_size) {
    int o = 0;
    for (int i = 0; in[i] && o < out_size - 8; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20)         { out[o++] = ' '; }
        else                        { out[o++] = (char)c; }
    }
    out[o] = '\0';
}

// ---------------------------------------------------------------------------
// HTTP POST (own copy so the logger never depends on api.c state)
// ---------------------------------------------------------------------------
static int http_post_logs(const char *body) {
    if (!g_host[0]) return 0;

    HINTERNET session = WinHttpOpen(L"SteamAutoUpdater-Log/1.0",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return 0;
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 10000);

    wchar_t whost[256] = {0};
    MultiByteToWideChar(CP_UTF8, 0, g_host, -1, whost, 256);

    HINTERNET connect = WinHttpConnect(session, whost, g_port, 0);
    if (!connect) { WinHttpCloseHandle(session); return 0; }

    DWORD flags = g_https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connect, L"POST", L"/logs/ingest",
                                           NULL, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return 0;
    }

    wchar_t hdr[512] = {0};
    swprintf(hdr, 512,
             L"Authorization: Bearer %hs\r\nContent-Type: application/json; charset=utf-8",
             g_api_key);
    WinHttpAddRequestHeaders(request, hdr, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

    DWORD len = (DWORD)strlen(body);
    int ok = 0;
    if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                           (LPVOID)body, len, len, 0) &&
        WinHttpReceiveResponse(request, NULL)) {
        DWORD status = 0, ssz = sizeof(status);
        WinHttpQueryHeaders(request,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            NULL, &status, &ssz, NULL);
        ok = (status >= 200 && status < 300);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return ok;
}

// Build one batch out of the queue head. Returns the number of entries taken
// (they stay in the queue until the POST succeeds).
static int build_batch(char *buf, int buf_size) {
    char esc[LOG_TEXT_MAX * 2 + 8];
    int pos = snprintf(buf, buf_size, "{\"pc_id\":\"%s\",\"session_id\":\"%s\",\"lines\":[",
                       g_pc_id, g_session);
    int taken = 0;

    EnterCriticalSection(&g_cs);
    int n = g_q_count < LOG_BATCH_MAX ? g_q_count : LOG_BATCH_MAX;
    for (int i = 0; i < n; i++) {
        const LogEntry *e = &g_queue[(g_q_head + i) % LOG_QUEUE_CAPACITY];
        json_escape(e->text, esc, sizeof(esc));
        int need = snprintf(NULL, 0,
                            "%s{\"ts\":\"%s\",\"level\":\"%s\",\"module\":\"%s\","
                            "\"app_id\":\"%s\",\"thread\":%lu,\"text\":\"%s\"}",
                            i ? "," : "", e->ts, level_name(e->level), e->module,
                            e->app_id, (unsigned long)e->thread_id, esc);
        if (pos + need + 64 >= buf_size) break;
        pos += snprintf(buf + pos, buf_size - pos,
                        "%s{\"ts\":\"%s\",\"level\":\"%s\",\"module\":\"%s\","
                        "\"app_id\":\"%s\",\"thread\":%lu,\"text\":\"%s\"}",
                        i ? "," : "", e->ts, level_name(e->level), e->module,
                        e->app_id, (unsigned long)e->thread_id, esc);
        taken++;
    }
    LeaveCriticalSection(&g_cs);

    snprintf(buf + pos, buf_size - pos, "]}");
    return taken;
}

static void drop_sent(int count) {
    EnterCriticalSection(&g_cs);
    if (count > g_q_count) count = g_q_count;
    g_q_head = (g_q_head + count) % LOG_QUEUE_CAPACITY;
    g_q_count -= count;
    LeaveCriticalSection(&g_cs);
}

static DWORD WINAPI uploader_thread(LPVOID param) {
    (void)param;
    const int buf_size = 256 * 1024;
    char *buf = (char *)malloc((size_t)buf_size);
    if (!buf) return 0;

    int backoff_ms = 0;

    while (!InterlockedCompareExchange(&g_stop, 0, 0)) {
        WaitForSingleObject(g_wake, backoff_ms > 0 ? backoff_ms : LOG_UPLOAD_PERIOD_MS);

        if (!g_remote_on) { InterlockedExchange(&g_flush_done, 1); continue; }

        for (;;) {
            if (log_pending_count() == 0) break;
            int taken = build_batch(buf, buf_size);
            if (taken <= 0) break;
            if (http_post_logs(buf)) {
                drop_sent(taken);
                backoff_ms = 0;
            } else {
                // Server unreachable: keep the lines, retry later, slow down.
                backoff_ms = backoff_ms ? (backoff_ms < 30000 ? backoff_ms * 2 : 30000)
                                        : 5000;
                break;
            }
        }
        InterlockedExchange(&g_flush_done, 1);
    }

    // Final drain on shutdown (best effort, short).
    if (g_remote_on) {
        for (int i = 0; i < 5 && log_pending_count() > 0; i++) {
            int taken = build_batch(buf, buf_size);
            if (taken <= 0 || !http_post_logs(buf)) break;
            drop_sent(taken);
        }
    }
    free(buf);
    return 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void log_init(const char *file_path, LogLevel min_level) {
    if (g_inited) return;
    InitializeCriticalSection(&g_cs);
    g_inited = 1;
    g_min_level = min_level;
    strncpy(g_path, file_path, sizeof(g_path) - 1);

    GUID g; CoCreateGuid(&g);
    snprintf(g_session, sizeof(g_session), "%08lX%04X%04X", g.Data1, g.Data2, g.Data3);

    g_queue = (LogEntry *)calloc(LOG_QUEUE_CAPACITY, sizeof(LogEntry));
    open_file();

    g_wake   = CreateEvent(NULL, FALSE, FALSE, NULL);
    g_thread = CreateThread(NULL, 0, uploader_thread, NULL, 0, NULL);

    OSVERSIONINFOA os; memset(&os, 0, sizeof(os)); os.dwOSVersionInfoSize = sizeof(os);
    char cwd[MAX_PATH] = {0};
    GetCurrentDirectoryA(MAX_PATH, cwd);
    LOG_INFO("app", "================ new session %s ================", g_session);
    LOG_INFO("app", "log file: %s (level %s, rotation %d MB x %d)",
             g_path, level_name(min_level), LOG_MAX_FILE_BYTES / (1024 * 1024),
             LOG_KEEP_FILES);
    LOG_INFO("app", "working directory: %s", cwd);
}

void log_set_pc_id(const char *pc_id) {
    if (!pc_id) return;
    strncpy(g_pc_id, pc_id, sizeof(g_pc_id) - 1);
    LOG_INFO("app", "pc_id = %s", g_pc_id);
}

void log_set_app(const char *app_id) {
    if (!g_inited) return;
    EnterCriticalSection(&g_cs);
    if (app_id && app_id[0]) strncpy(g_app_id, app_id, sizeof(g_app_id) - 1);
    else g_app_id[0] = '\0';
    LeaveCriticalSection(&g_cs);
}

static void parse_server_url(const char *url) {
    g_https = 0;
    g_port  = 80;
    const char *rest = url;
    if (strncmp(url, "https://", 8) == 0) { g_https = 1; g_port = 443; rest = url + 8; }
    else if (strncmp(url, "http://", 7) == 0) { rest = url + 7; }

    strncpy(g_host, rest, sizeof(g_host) - 1);
    char *slash = strchr(g_host, '/');
    if (slash) *slash = '\0';
    char *colon = strchr(g_host, ':');
    if (colon) { g_port = (INTERNET_PORT)atoi(colon + 1); *colon = '\0'; }
}

void log_set_remote(const char *server_url, const char *api_key, int enabled) {
    if (server_url && server_url[0]) parse_server_url(server_url);
    if (api_key) strncpy(g_api_key, api_key, sizeof(g_api_key) - 1);
    g_remote_on = enabled ? 1 : 0;
    LOG_INFO("app", "log upload %s (host %s:%d, https=%d)",
             g_remote_on ? "enabled" : "disabled", g_host, (int)g_port, g_https);
}

void log_set_remote_level(LogLevel level) { g_remote_level = level; }

const char *log_session_id(void) { return g_session; }
const char *log_file_path(void)  { return g_path; }

static void emit(int level, const char *module, const char *text) {
    LogEntry e;
    memset(&e, 0, sizeof(e));
    now_stamp(e.ts, sizeof(e.ts));
    strncpy(e.module, module ? module : "app", sizeof(e.module) - 1);
    e.level     = level;
    e.thread_id = GetCurrentThreadId();
    strncpy(e.text, text, sizeof(e.text) - 1);

    EnterCriticalSection(&g_cs);
    strncpy(e.app_id, g_app_id, sizeof(e.app_id) - 1);
    write_file_line(&e);
    if (level >= (int)g_remote_level) enqueue(&e);
    LeaveCriticalSection(&g_cs);
}

void log_write(LogLevel level, const char *module, const char *fmt, ...) {
    if (!g_inited || (int)level < (int)g_min_level) return;

    char text[LOG_TEXT_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    emit((int)level, module, text);
}

void log_raw(const char *module, const char *line) {
    if (!g_inited || !line || !line[0]) return;
    if ((int)LOG_LEVEL_DEBUG < (int)g_min_level) return;
    emit((int)LOG_LEVEL_DEBUG, module, line);
}

void log_flush_remote(int timeout_ms) {
    if (!g_inited || !g_remote_on) return;
    InterlockedExchange(&g_flush_done, 0);
    SetEvent(g_wake);
    DWORD start = GetTickCount();
    while ((GetTickCount() - start) < (DWORD)timeout_ms) {
        if (InterlockedCompareExchange(&g_flush_done, 0, 0) && log_pending_count() == 0)
            return;
        Sleep(50);
    }
}

void log_shutdown(void) {
    if (!g_inited) return;
    LOG_INFO("app", "session %s finished (%ld line(s) dropped, %d still queued)",
             g_session, g_dropped, log_pending_count());
    log_flush_remote(4000);
    InterlockedExchange(&g_stop, 1);
    if (g_wake) SetEvent(g_wake);
    if (g_thread) {
        WaitForSingleObject(g_thread, 6000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    if (g_wake) { CloseHandle(g_wake); g_wake = NULL; }
    if (g_file != INVALID_HANDLE_VALUE) { CloseHandle(g_file); g_file = INVALID_HANDLE_VALUE; }
    free(g_queue);
    g_queue = NULL;
    g_inited = 0;
    DeleteCriticalSection(&g_cs);
}
