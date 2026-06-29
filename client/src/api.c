// api.c - VPS REST API client using WinHTTP
// No libcurl, no external JSON libs - pure Win32 + WinHTTP
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <objbase.h>
#include <stdio.h>
#include <string.h>
#include "api.h"

#pragma comment(lib, "winhttp.lib")

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------
static char g_server_url[512] = {0};
static char g_api_key[256]    = {0};
static char g_host[256]       = {0};
static INTERNET_PORT g_port   = INTERNET_DEFAULT_HTTPS_PORT;
static int  g_use_https       = 1;

// ---------------------------------------------------------------------------
// Minimal JSON builder helpers
// ---------------------------------------------------------------------------
static void json_str(char *buf, int buf_size, const char *key, const char *val,
                     int last) {
    char tmp[512];
    int pos = (int)strlen(buf);
    // Escape backslashes and quotes in val
    char escaped[512] = {0};
    int ei = 0;
    for (int i = 0; val && val[i] && ei < 500; i++) {
        if (val[i] == '"' || val[i] == '\\') escaped[ei++] = '\\';
        escaped[ei++] = val[i];
    }
    snprintf(tmp, sizeof(tmp), "\"%s\":\"%s\"%s", key, escaped, last ? "" : ",");
    strncat(buf + pos, tmp, buf_size - pos - 1);
}

static void json_int(char *buf, int buf_size, const char *key, int val, int last) {
    char tmp[128];
    int pos = (int)strlen(buf);
    snprintf(tmp, sizeof(tmp), "\"%s\":%d%s", key, val, last ? "" : ",");
    strncat(buf + pos, tmp, buf_size - pos - 1);
}

static void json_null(char *buf, int buf_size, const char *key, int last) {
    char tmp[128];
    int pos = (int)strlen(buf);
    snprintf(tmp, sizeof(tmp), "\"%s\":null%s", key, last ? "" : ",");
    strncat(buf + pos, tmp, buf_size - pos - 1);
}

// ---------------------------------------------------------------------------
// Minimal JSON extractor: find "key":"value" -> returns 1 on success
// ---------------------------------------------------------------------------
static int json_get_str(const char *json, const char *key, char *out, int out_size) {
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == ' ') p++;
    if (*p == '"') {
        p++;
        const char *start = p;
        while (*p && *p != '"') p++;
        int len = (int)(p - start);
        if (len >= out_size) len = out_size - 1;
        memcpy(out, start, len);
        out[len] = '\0';
        return 1;
    }
    return 0;
}

static int json_get_int(const char *json, const char *key, int *out) {
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == ' ') p++;
    *out = atoi(p);
    return 1;
}

// ---------------------------------------------------------------------------
// Parse server URL into host + port + https flag
// ---------------------------------------------------------------------------
static void parse_url(const char *url) {
    strncpy(g_server_url, url, sizeof(g_server_url) - 1);
    if (strncmp(url, "https://", 8) == 0) {
        g_use_https = 1;
        g_port = INTERNET_DEFAULT_HTTPS_PORT;
        strncpy(g_host, url + 8, sizeof(g_host) - 1);
    } else if (strncmp(url, "http://", 7) == 0) {
        g_use_https = 0;
        g_port = INTERNET_DEFAULT_HTTP_PORT;
        strncpy(g_host, url + 7, sizeof(g_host) - 1);
    } else {
        strncpy(g_host, url, sizeof(g_host) - 1);
        g_use_https = 1;
    }
    // Strip trailing slash and port if present
    char *colon = strchr(g_host, ':');
    if (colon) { g_port = (INTERNET_PORT)atoi(colon + 1); *colon = '\0'; }
    char *slash = strchr(g_host, '/');
    if (slash) *slash = '\0';
}

// ---------------------------------------------------------------------------
// Core HTTP request: POST with JSON body, returns response body
// out_buf must be freed by caller (HeapFree)
// ---------------------------------------------------------------------------
static char *http_post(const char *path, const char *json_body, int *status_out) {
    HINTERNET session = WinHttpOpen(L"SteamAutoUpdater/1.0",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return NULL;

    // Convert host to wchar
    wchar_t whost[256] = {0};
    MultiByteToWideChar(CP_UTF8, 0, g_host, -1, whost, 256);

    HINTERNET connect = WinHttpConnect(session, whost, g_port, 0);
    if (!connect) { WinHttpCloseHandle(session); return NULL; }

    // Convert path to wchar
    wchar_t wpath[512] = {0};
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 512);

    DWORD flags = g_use_https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connect, L"POST", wpath,
                                           NULL, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return NULL;
    }

    // Set Authorization header
    wchar_t auth_header[512] = {0};
    swprintf(auth_header, 512, L"Authorization: Bearer %hs\r\nContent-Type: application/json",
             g_api_key);
    WinHttpAddRequestHeaders(request, auth_header, (DWORD)-1,
                             WINHTTP_ADDREQ_FLAG_ADD);

    // Send request with body
    DWORD body_len = (DWORD)strlen(json_body);
    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            (LPVOID)json_body, body_len, body_len, 0) ||
        !WinHttpReceiveResponse(request, NULL)) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return NULL;
    }

    // Get status code
    if (status_out) {
        DWORD status = 0, status_sz = sizeof(status);
        WinHttpQueryHeaders(request,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            NULL, &status, &status_sz, NULL);
        *status_out = (int)status;
    }

    // Read response body
    char *body = NULL;
    DWORD total = 0;
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(request, &avail) && avail > 0) {
        char *tmp = body
            ? (char *)HeapReAlloc(GetProcessHeap(), 0, body, total + avail + 1)
            : (char *)HeapAlloc(GetProcessHeap(), 0, total + avail + 1);
        if (!tmp) break;
        body = tmp;
        DWORD read = 0;
        WinHttpReadData(request, body + total, avail, &read);
        total += read;
        body[total] = '\0';
    }
    if (!body) {
        body = (char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 2);
        if (body) body[0] = '\0';
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return body;
}

// ---------------------------------------------------------------------------
// Public: api_init
// ---------------------------------------------------------------------------
void api_init(const char *server_url, const char *api_key) {
    parse_url(server_url);
    strncpy(g_api_key, api_key, sizeof(g_api_key) - 1);
}

// ---------------------------------------------------------------------------
// Public: api_acquire
// ---------------------------------------------------------------------------
int api_acquire(const char *pc_id, const char *app_id,
                const char *steam_id64, ApiAcquireResult *out) {
    // Build JSON body
    char body[1024] = "{";
    json_str(body, sizeof(body), "pc_id",  pc_id,  0);
    json_str(body, sizeof(body), "app_id", app_id, 0);

    // Generate idempotency key (simple GUID)
    GUID g; CoCreateGuid(&g);
    char ikey[64];
    snprintf(ikey, sizeof(ikey), "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
             g.Data1, g.Data2, g.Data3,
             g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
             g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    json_str(body, sizeof(body), "idempotency_key", ikey, 0);

    if (steam_id64 && steam_id64[0]) {
        json_str(body, sizeof(body), "steam_id64", steam_id64, 1);
    } else {
        json_null(body, sizeof(body), "steam_id64", 1);
    }
    strncat(body, "}", sizeof(body) - strlen(body) - 1);

    int status = 0;
    char *resp = http_post("/accounts/acquire", body, &status);
    if (!resp || status != 200) {
        if (resp) HeapFree(GetProcessHeap(), 0, resp);
        return 0;
    }

    memset(out, 0, sizeof(*out));
    json_get_int(resp, "account_id", &out->account_id);
    json_get_str(resp, "login",       out->login,       sizeof(out->login));
    json_get_str(resp, "password",    out->password,    sizeof(out->password));
    json_get_str(resp, "lease_token", out->lease_token, sizeof(out->lease_token));

    HeapFree(GetProcessHeap(), 0, resp);
    return 1;
}

// ---------------------------------------------------------------------------
// Public: api_heartbeat
// ---------------------------------------------------------------------------
int api_heartbeat(const char *lease_token, const char *pc_id) {
    char body[512] = "{";
    json_str(body, sizeof(body), "lease_token", lease_token, 0);
    json_str(body, sizeof(body), "pc_id", pc_id, 1);
    strncat(body, "}", sizeof(body) - strlen(body) - 1);

    int status = 0;
    char *resp = http_post("/accounts/heartbeat", body, &status);
    if (resp) HeapFree(GetProcessHeap(), 0, resp);
    return (status == 200) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Public: api_release
// ---------------------------------------------------------------------------
int api_release(const char *lease_token, const char *pc_id,
                const char *result,
                const char *app_id,
                const char *error_msg,
                const char *build_id_before,
                const char *build_id_after) {
    char body[1024] = "{";
    json_str(body, sizeof(body), "lease_token",    lease_token,    0);
    json_str(body, sizeof(body), "pc_id",          pc_id,          0);
    json_str(body, sizeof(body), "result",         result,         0);
    json_str(body, sizeof(body), "app_id",         app_id,         0);

    if (error_msg && error_msg[0])
        json_str(body, sizeof(body), "error_msg", error_msg, 0);
    else
        json_null(body, sizeof(body), "error_msg", 0);

    if (build_id_before && build_id_before[0])
        json_str(body, sizeof(body), "build_id_before", build_id_before, 0);
    else
        json_null(body, sizeof(body), "build_id_before", 0);

    if (build_id_after && build_id_after[0])
        json_str(body, sizeof(body), "build_id_after", build_id_after, 1);
    else
        json_null(body, sizeof(body), "build_id_after", 1);

    strncat(body, "}", sizeof(body) - strlen(body) - 1);

    int status = 0;
    char *resp = http_post("/accounts/release", body, &status);
    if (resp) HeapFree(GetProcessHeap(), 0, resp);
    return (status == 200) ? 1 : 0;
}
