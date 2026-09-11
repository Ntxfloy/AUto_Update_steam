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
        p += strlen(pattern);
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '"') { p++; continue; }
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
// Internal: path helpers
// ---------------------------------------------------------------------------
static void normalize_path(char *p) {
    for (char *c = p; *c; c++) if (*c == '/') *c = '\\';
    int n = (int)strlen(p);
    while (n > 0 && p[n - 1] == '\\') p[--n] = '\0';
}

static int reg_read_str(HKEY root, const char *subkey, const char *value,
                        REGSAM extra, char *out, DWORD out_size) {
    HKEY hk;
    if (RegOpenKeyExA(root, subkey, 0, KEY_READ | extra, &hk) != ERROR_SUCCESS) return 0;
    DWORD sz = out_size - 1, type = 0;
    LONG r = RegQueryValueExA(hk, value, NULL, &type, (BYTE *)out, &sz);
    RegCloseKey(hk);
    if (r != ERROR_SUCCESS) return 0;
    if (type != REG_SZ && type != REG_EXPAND_SZ) return 0;
    if (sz >= out_size) sz = out_size - 1;
    out[sz] = '\0';
    return out[0] != '\0';
}

// ---------------------------------------------------------------------------
// Resolve the Steam client root
//
// The old code read only HKCU\Software\Valve\Steam!SteamPath and silently fell
// back to "C:\Program Files (x86)\Steam". Under SYSTEM / Task Scheduler / a
// different Windows user that hive is a different one and the value is empty,
// which is exactly why paths looked "random". HKLM is machine-wide and is the
// value to trust first.
// ---------------------------------------------------------------------------
int acf_get_steam_root(char *out, int out_size) {
    char buf[MAX_PATH] = {0};
    int ok = 0;

    if (!ok) ok = reg_read_str(HKEY_LOCAL_MACHINE,
                               "SOFTWARE\\WOW6432Node\\Valve\\Steam",
                               "InstallPath", 0, buf, MAX_PATH);
    if (!ok) ok = reg_read_str(HKEY_LOCAL_MACHINE, "SOFTWARE\\Valve\\Steam",
                               "InstallPath", KEY_WOW64_32KEY, buf, MAX_PATH);
    if (!ok) ok = reg_read_str(HKEY_LOCAL_MACHINE, "SOFTWARE\\Valve\\Steam",
                               "InstallPath", KEY_WOW64_64KEY, buf, MAX_PATH);
    if (!ok) ok = reg_read_str(HKEY_CURRENT_USER, "Software\\Valve\\Steam",
                               "SteamPath", 0, buf, MAX_PATH);

    if (ok) {
        normalize_path(buf);
        char probe[MAX_PATH];
        snprintf(probe, MAX_PATH, "%s\\steamapps", buf);
        if (GetFileAttributesA(probe) == INVALID_FILE_ATTRIBUTES) ok = 0;
    }

    if (!ok) {
        static const char *cands[] = {
            "C:\\Program Files (x86)\\Steam", "C:\\Steam", "C:\\Games\\Steam",
            "D:\\Steam", "D:\\Program Files (x86)\\Steam", "D:\\Games\\Steam",
            "E:\\Steam", "E:\\Games\\Steam"
        };
        for (int i = 0; i < (int)(sizeof(cands) / sizeof(cands[0])); i++) {
            char probe[MAX_PATH];
            snprintf(probe, MAX_PATH, "%s\\steamapps", cands[i]);
            if (GetFileAttributesA(probe) != INVALID_FILE_ATTRIBUTES) {
                strncpy(buf, cands[i], MAX_PATH - 1);
                buf[MAX_PATH - 1] = '\0';
                ok = 1;
                break;
            }
        }
    }

    if (!ok) return 0;
    strncpy(out, buf, out_size - 1);
    out[out_size - 1] = '\0';
    return 1;
}

// ---------------------------------------------------------------------------
// List all Steam library roots
// ---------------------------------------------------------------------------
int acf_list_libraries(char roots[][MAX_PATH], int max_roots) {
    if (max_roots <= 0) return 0;

    char steam_root[MAX_PATH] = {0};
    if (!acf_get_steam_root(steam_root, MAX_PATH)) return 0;

    int count = 0;
    strncpy(roots[count], steam_root, MAX_PATH - 1);
    roots[count][MAX_PATH - 1] = '\0';
    count++;

    char vdf_paths[2][MAX_PATH];
    snprintf(vdf_paths[0], MAX_PATH, "%s\\config\\libraryfolders.vdf", steam_root);
    snprintf(vdf_paths[1], MAX_PATH, "%s\\steamapps\\libraryfolders.vdf", steam_root);

    for (int v = 0; v < 2; v++) {
        char *vdf = read_file_alloc(vdf_paths[v]);
        if (!vdf) continue;

        const char *p = vdf;
        while (count < max_roots && (p = strstr(p, "\"path\"")) != NULL) {
            p += 6;
            while (*p == ' ' || *p == '\t') p++;
            if (*p != '"') continue;
            p++;
            const char *start = p;
            while (*p && *p != '"') p++;
            int len = (int)(p - start);
            if (len <= 0) continue;
            if (len >= MAX_PATH) len = MAX_PATH - 1;

            char tmp[MAX_PATH] = {0};
            memcpy(tmp, start, len);

            // "C:\\Games\\Steam" -> "C:\Games\Steam"
            char norm[MAX_PATH] = {0};
            int ni = 0;
            for (int i = 0; tmp[i] && ni < MAX_PATH - 1; i++) {
                if (tmp[i] == '\\' && tmp[i + 1] == '\\') { norm[ni++] = '\\'; i++; }
                else                                       { norm[ni++] = tmp[i]; }
            }
            normalize_path(norm);
            if (!norm[0]) continue;

            int dup = 0;
            for (int i = 0; i < count; i++)
                if (_stricmp(roots[i], norm) == 0) { dup = 1; break; }
            if (dup) continue;

            strncpy(roots[count], norm, MAX_PATH - 1);
            roots[count][MAX_PATH - 1] = '\0';
            count++;
        }
        HeapFree(GetProcessHeap(), 0, vdf);
    }
    return count;
}

// ---------------------------------------------------------------------------
// Pick the library with the most free space (install target for new games)
// ---------------------------------------------------------------------------
int acf_pick_install_library(char *out, int out_size) {
    static char roots[32][MAX_PATH];
    int n = acf_list_libraries(roots, 32);
    if (n <= 0) return 0;

    int best = -1;
    ULARGE_INTEGER best_free;
    best_free.QuadPart = 0;

    for (int i = 0; i < n; i++) {
        if (GetFileAttributesA(roots[i]) == INVALID_FILE_ATTRIBUTES) continue;
        char probe[MAX_PATH];
        snprintf(probe, MAX_PATH, "%s\\", roots[i]);
        ULARGE_INTEGER avail, total, total_free;
        if (!GetDiskFreeSpaceExA(probe, &avail, &total, &total_free)) continue;
        if (best < 0 || avail.QuadPart > best_free.QuadPart) {
            best = i;
            best_free = avail;
        }
    }
    if (best < 0) best = 0;

    strncpy(out, roots[best], out_size - 1);
    out[out_size - 1] = '\0';
    return 1;
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
// Import the manifest SteamCMD wrote into the Steam client library
// ---------------------------------------------------------------------------
int acf_import_manifest(const char *steamcmd_path, const char *app_id,
                        const char *target_library, const char *installdir) {
    char dir[MAX_PATH];
    strncpy(dir, steamcmd_path, MAX_PATH - 1);
    dir[MAX_PATH - 1] = '\0';
    char *slash = strrchr(dir, '\\');
    if (!slash) slash = strrchr(dir, '/');
    if (!slash) return 0;
    *slash = '\0';

    char src[MAX_PATH];
    snprintf(src, MAX_PATH, "%s\\steamapps\\appmanifest_%s.acf", dir, app_id);
    char *buf = read_file_alloc(src);
    if (!buf) return 0;

    char dst_dir[MAX_PATH];
    snprintf(dst_dir, MAX_PATH, "%s\\steamapps", target_library);
    CreateDirectoryA(dst_dir, NULL);

    char dst[MAX_PATH];
    snprintf(dst, MAX_PATH, "%s\\appmanifest_%s.acf", dst_dir, app_id);

    HANDLE h = CreateFileA(dst, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        HeapFree(GetProcessHeap(), 0, buf);
        return 0;
    }

    DWORD written = 0;
    char *p = buf;
    while (*p) {
        char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) + 1 : strlen(p);

        char tmp[1024];
        size_t cl = len < sizeof(tmp) - 1 ? len : sizeof(tmp) - 1;
        memcpy(tmp, p, cl);
        tmp[cl] = '\0';

        if (strstr(tmp, "\"installdir\"")) {
            char out_line[1024];
            snprintf(out_line, sizeof(out_line), "\t\"installdir\"\t\t\"%s\"\r\n", installdir);
            WriteFile(h, out_line, (DWORD)strlen(out_line), &written, NULL);
        } else {
            WriteFile(h, p, (DWORD)len, &written, NULL);
        }
        p += len;
    }

    CloseHandle(h);
    HeapFree(GetProcessHeap(), 0, buf);
    return 1;
}

// ---------------------------------------------------------------------------
// Scan Steam libraries for appmanifest_*.acf
// ---------------------------------------------------------------------------
int acf_scan_libraries(AcfInfo *out, int max_out) {
    static char lib_roots[32][MAX_PATH];
    int lib_count = acf_list_libraries(lib_roots, 32);
    if (lib_count <= 0) return 0;

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
                // Only report games whose content folder really exists on disk.
                if (GetFileAttributesA(out[found].game_path) != INVALID_FILE_ATTRIBUTES) {
                    found++;
                }
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
    static AcfInfo all[256];
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
