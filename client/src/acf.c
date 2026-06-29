// acf.c - Steam .acf manifest parser, library scanner, backup/restore
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "acf.h"

// ---------------------------------------------------------------------------
// Internal: extract a VDF string field value
// Format: "FieldName"    "Value"
// ---------------------------------------------------------------------------
static int vdf_extract(const char *buf, const char *field, char *out, int out_size) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", field);

    const char *p = buf;
    while ((p = strstr(p, pattern)) != NULL) {
        // skip the field name
        p += strlen(pattern);
        // skip whitespace/tabs
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '"') { p++; continue; }
        p++; // skip opening quote
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

// ---------------------------------------------------------------------------
// Read entire file into a heap buffer (caller must free)
// ---------------------------------------------------------------------------
static char *read_file_alloc(const char *path) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    DWORD size = GetFileSize(h, NULL);
    char *buf = (char *)HeapAlloc(GetProcessHeap(), 0, size + 1);
    if (!buf) { CloseHandle(h); return NULL; }
    DWORD read = 0;
    ReadFile(h, buf, size, &read, NULL);
    buf[read] = '\0';
    CloseHandle(h);
    return buf;
}

// ---------------------------------------------------------------------------
// Parse a single .acf file
// ---------------------------------------------------------------------------
int acf_parse(const char *acf_path, const char *library_root, AcfInfo *out) {
    char *buf = read_file_alloc(acf_path);
    if (!buf) return 0;

    memset(out, 0, sizeof(AcfInfo));
    strncpy(out->acf_path,     acf_path,     MAX_PATH - 1);
    strncpy(out->library_root, library_root, MAX_PATH - 1);

    vdf_extract(buf, "appid",      out->appid,       sizeof(out->appid));
    vdf_extract(buf, "name",       out->name,        sizeof(out->name));
    vdf_extract(buf, "installdir", out->installdir,  sizeof(out->installdir));
    vdf_extract(buf, "buildid",    out->buildid,     sizeof(out->buildid));
    vdf_extract(buf, "StateFlags", out->state_flags, sizeof(out->state_flags));
    vdf_extract(buf, "LastOwner",  out->last_owner,  sizeof(out->last_owner));
    vdf_extract(buf, "SizeOnDisk", out->size_on_disk,sizeof(out->size_on_disk));

    // game_path = library_root\steamapps\common\installdir
    snprintf(out->game_path, MAX_PATH, "%s\\steamapps\\common\\%s",
             library_root, out->installdir);

    HeapFree(GetProcessHeap(), 0, buf);
    return (out->appid[0] != '\0') ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Read a single field from a .acf file
// ---------------------------------------------------------------------------
int acf_read_field(const char *acf_path, const char *field, char *out, int out_size) {
    char *buf = read_file_alloc(acf_path);
    if (!buf) return 0;
    int r = vdf_extract(buf, field, out, out_size);
    HeapFree(GetProcessHeap(), 0, buf);
    return r;
}

// ---------------------------------------------------------------------------
// Scan Steam libraries from registry + libraryfolders.vdf
// ---------------------------------------------------------------------------
int acf_scan_libraries(AcfInfo *out, int max_out) {
    // 1. Get Steam root from registry
    char steam_root[MAX_PATH] = {0};
    HKEY hk;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Valve\\Steam",
                      0, KEY_READ, &hk) == ERROR_SUCCESS) {
        DWORD sz = MAX_PATH;
        RegQueryValueExA(hk, "SteamPath", NULL, NULL, (BYTE*)steam_root, &sz);
        RegCloseKey(hk);
    }
    // Normalize slashes
    for (char *c = steam_root; *c; c++) if (*c == '/') *c = '\\';
    // Trim trailing slash
    int rlen = (int)strlen(steam_root);
    while (rlen > 0 && steam_root[rlen-1] == '\\') steam_root[--rlen] = '\0';

    if (!steam_root[0]) {
        // Fallback
        strncpy(steam_root, "C:\\Program Files (x86)\\Steam", MAX_PATH-1);
    }

    // 2. Build list of library roots
    char lib_roots[32][MAX_PATH];
    int  lib_count = 0;
    strncpy(lib_roots[lib_count++], steam_root, MAX_PATH-1);

    // Parse libraryfolders.vdf
    char vdf_paths[2][MAX_PATH];
    snprintf(vdf_paths[0], MAX_PATH, "%s\\config\\libraryfolders.vdf", steam_root);
    snprintf(vdf_paths[1], MAX_PATH, "%s\\steamapps\\libraryfolders.vdf", steam_root);

    for (int v = 0; v < 2; v++) {
        char *vdf = read_file_alloc(vdf_paths[v]);
        if (!vdf) continue;

        const char *p = vdf;
        while ((p = strstr(p, "\"path\"")) != NULL) {
            p += 6;
            while (*p == ' ' || *p == '\t') p++;
            if (*p != '"') continue;
            p++;
            const char *start = p;
            while (*p && *p != '"') p++;
            int len = (int)(p - start);
            if (len > 0 && lib_count < 32) {
                char tmp[MAX_PATH] = {0};
                if (len >= MAX_PATH) len = MAX_PATH - 1;
                memcpy(tmp, start, len);
                tmp[len] = '\0';
                // Normalize: replace double backslashes with single backslash
                char normalized[MAX_PATH] = {0};
                int ni = 0;
                for (int i = 0; tmp[i] && ni < MAX_PATH-1; i++) {
                    if (tmp[i] == '\\' && tmp[i+1] == '\\') {
                        normalized[ni++] = '\\';
                        i++; // skip second backslash
                    } else {
                        normalized[ni++] = tmp[i];
                    }
                }
                // Check for duplicates
                int dup = 0;
                for (int i = 0; i < lib_count; i++) {
                    if (_stricmp(lib_roots[i], normalized) == 0) { dup = 1; break; }
                }
                if (!dup && normalized[0]) {
                    strncpy(lib_roots[lib_count++], normalized, MAX_PATH-1);
                }
            }
        }
        HeapFree(GetProcessHeap(), 0, vdf);
    }

    // 3. Scan each library for appmanifest_*.acf
    int found = 0;
    for (int li = 0; li < lib_count && found < max_out; li++) {
        char pattern[MAX_PATH];
        snprintf(pattern, MAX_PATH, "%s\\steamapps\\appmanifest_*.acf", lib_roots[li]);

        WIN32_FIND_DATAA fd;
        HANDLE hf = FindFirstFileA(pattern, &fd);
        if (hf == INVALID_HANDLE_VALUE) continue;

        do {
            char acf_full[MAX_PATH];
            snprintf(acf_full, MAX_PATH, "%s\\steamapps\\%s",
                     lib_roots[li], fd.cFileName);
            if (acf_parse(acf_full, lib_roots[li], &out[found])) {
                found++;
            }
        } while (FindNextFileA(hf, &fd) && found < max_out);
        FindClose(hf);
    }
    return found;
}

// ---------------------------------------------------------------------------
// Find one game by app_id
// ---------------------------------------------------------------------------
int acf_find_game(const char *app_id, AcfInfo *out) {
    AcfInfo all[256];
    int n = acf_scan_libraries(all, 256);
    for (int i = 0; i < n; i++) {
        if (strcmp(all[i].appid, app_id) == 0) {
            *out = all[i];
            return 1;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Backup .acf to %TEMP%
// ---------------------------------------------------------------------------
int acf_backup(const AcfInfo *info, char *backup_path) {
    char tmp_dir[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp_dir);

    // timestamp
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(backup_path, MAX_PATH,
             "%sacf_backup_%s_%04d%02d%02d_%02d%02d%02d.acf",
             tmp_dir, info->appid,
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);

    return CopyFileA(info->acf_path, backup_path, FALSE) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Restore .acf from backup
// ---------------------------------------------------------------------------
int acf_restore(const char *acf_path, const char *backup_path) {
    return CopyFileA(backup_path, acf_path, FALSE) ? 1 : 0;
}
