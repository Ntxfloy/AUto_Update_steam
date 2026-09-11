// acf.c - Steam .acf manifest parser, library scanner, backup/restore
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "acf.h"

// A sane upper bound for a manifest / libraryfolders.vdf. Anything bigger is
// not a VDF file and must not be pulled into memory.
#define ACF_MAX_FILE_BYTES  (16 * 1024 * 1024)

// Never treat a disk smaller than this (total size) as an install target.
#define ACF_MIN_DISK_BYTES  (500ULL * 1000ULL * 1000ULL * 1000ULL)

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
//
// Three things were wrong before and all three cost us games in the list:
//  * FILE_SHARE_READ only. The Steam client keeps appmanifest_*.acf open for
//    writing while it is running, so the open failed with a sharing violation
//    and the game silently disappeared from the UI. We now allow the writers
//    and retry a few times if the file is briefly locked anyway.
//  * GetFileSize has no error handling: INVALID_FILE_SIZE (0xFFFFFFFF) turned
//    into a 4 GB HeapAlloc.
//  * ReadFile result was ignored, so a partial read produced a truncated
//    buffer that parsed as "no fields".
// ---------------------------------------------------------------------------
static char *read_file_alloc(const char *path) {
    HANDLE h = INVALID_HANDLE_VALUE;

    for (int attempt = 0; attempt < 4; attempt++) {
        h = CreateFileA(path, GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) break;
        DWORD err = GetLastError();
        if (err != ERROR_SHARING_VIOLATION && err != ERROR_LOCK_VIOLATION &&
            err != ERROR_ACCESS_DENIED)
            return NULL;
        Sleep(120);
    }
    if (h == INVALID_HANDLE_VALUE) return NULL;

    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) ||
        size.QuadPart <= 0 || size.QuadPart > ACF_MAX_FILE_BYTES) {
        CloseHandle(h);
        return NULL;
    }

    DWORD total = (DWORD)size.QuadPart;
    char *buf = (char *)HeapAlloc(GetProcessHeap(), 0, total + 1);
    if (!buf) { CloseHandle(h); return NULL; }

    DWORD done = 0;
    while (done < total) {
        DWORD chunk = 0;
        if (!ReadFile(h, buf + done, total - done, &chunk, NULL)) {
            HeapFree(GetProcessHeap(), 0, buf);
            CloseHandle(h);
            return NULL;
        }
        if (chunk == 0) break;              // shrunk while we were reading
        done += chunk;
    }
    buf[done] = '\0';
    CloseHandle(h);

    if (done == 0) { HeapFree(GetProcessHeap(), 0, buf); return NULL; }
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
// Is this path a sane install target?
//
// Rules (same ones worker.c enforces, kept in sync on purpose):
//  * the volume must be a real fixed disk - no USB sticks, no card readers,
//    no network shares, no DVD drives, no RAM disks;
//  * not the Windows drive;
//  * at least 500 GB of total capacity.
// out_total / out_free are filled whenever the volume could be queried at all.
// ---------------------------------------------------------------------------
int acf_disk_check(const char *path, ULONGLONG *out_total, ULONGLONG *out_free) {
    if (out_total) *out_total = 0;
    if (out_free)  *out_free  = 0;
    if (!path || !path[0]) return ACF_DISK_UNUSABLE;

    char root[MAX_PATH];
    snprintf(root, MAX_PATH, "%s\\", path);

    // GetDriveTypeA wants a root path ("D:\"), so cut the letter off when we
    // got a full library path. UNC paths (\\server\share) are rejected below
    // as DRIVE_REMOTE.
    char vol[8] = {0};
    if (path[1] == ':') {
        vol[0] = path[0]; vol[1] = ':'; vol[2] = '\\'; vol[3] = '\0';
    }

    UINT dt = GetDriveTypeA(vol[0] ? vol : root);
    if (dt != DRIVE_FIXED) return ACF_DISK_NOT_FIXED;

    char sysdir[MAX_PATH] = {0};
    GetWindowsDirectoryA(sysdir, MAX_PATH);
    if (path[1] == ':' && sysdir[0] &&
        (path[0] | 0x20) == (sysdir[0] | 0x20))
        return ACF_DISK_SYSTEM;

    ULARGE_INTEGER avail, total, total_free;
    if (!GetDiskFreeSpaceExA(vol[0] ? vol : root, &avail, &total, &total_free))
        return ACF_DISK_UNUSABLE;

    if (out_total) *out_total = total.QuadPart;
    if (out_free)  *out_free  = avail.QuadPart;

    if (total.QuadPart < ACF_MIN_DISK_BYTES) return ACF_DISK_TOO_SMALL;
    return ACF_DISK_OK;
}

// ---------------------------------------------------------------------------
// Pick an install target for new games.
//
// worker.c owns the policy and reports the reason to the UI; this helper is
// only the "give me something reasonable" shortcut used by tooling. It now
// applies the very same rules so the two can never disagree and drop a game
// onto the system SSD.
// ---------------------------------------------------------------------------
int acf_pick_install_library(char *out, int out_size) {
    static char roots[32][MAX_PATH];
    int n = acf_list_libraries(roots, 32);
    if (n <= 0) return 0;

    int best = -1;
    ULONGLONG best_free = 0;

    for (int i = 0; i < n; i++) {
        if (GetFileAttributesA(roots[i]) == INVALID_FILE_ATTRIBUTES) continue;
        ULONGLONG total = 0, freeb = 0;
        if (acf_disk_check(roots[i], &total, &freeb) != ACF_DISK_OK) continue;
        if (best < 0 || freeb > best_free) { best = i; best_free = freeb; }
    }

    if (best < 0) return 0;      // deliberately no fallback to C:

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
//
// This used to write straight over the live appmanifest and then delete the
// source. Any failure in the middle (no rights, disk full, power loss) left
// the game with no manifest at all - the Steam client then shows it as not
// installed and a 50 GB folder becomes garbage. Now: write a temp file next
// to the destination, flush it, swap it in atomically with MoveFileEx, and
// only remove the source once the swap succeeded.
// ---------------------------------------------------------------------------
int acf_import_manifest(const char *steamcmd_path, const char *app_id,
                        const char *target_library, const char *installdir) {
    char dir[MAX_PATH];
    strncpy(dir, steamcmd_path, MAX_PATH - 1);
    dir[MAX_PATH - 1] = '\0';
    char *slash = strrchr(dir, '\\');
    if (!slash) slash = strrchr(dir, '/');
    if (slash) *slash = '\0';

    char src[MAX_PATH] = {0};
    char cand[MAX_PATH];

    // 1. SteamCMD puts the manifest here when force_install_dir is used:
    // <target_library>\steamapps\common\<installdir>\steamapps\appmanifest_<app_id>.acf
    snprintf(cand, MAX_PATH, "%s\\steamapps\\common\\%s\\steamapps\\appmanifest_%s.acf",
             target_library, installdir, app_id);
    if (GetFileAttributesA(cand) != INVALID_FILE_ATTRIBUTES) {
        strncpy(src, cand, MAX_PATH - 1);
    }

    // 2. Or directly in the game folder:
    if (!src[0]) {
        snprintf(cand, MAX_PATH, "%s\\steamapps\\common\\%s\\appmanifest_%s.acf",
                 target_library, installdir, app_id);
        if (GetFileAttributesA(cand) != INVALID_FILE_ATTRIBUTES) {
            strncpy(src, cand, MAX_PATH - 1);
        }
    }

    // 3. Or in steamcmd directory (if force_install_dir was not used):
    if (!src[0] && dir[0]) {
        snprintf(cand, MAX_PATH, "%s\\steamapps\\appmanifest_%s.acf", dir, app_id);
        if (GetFileAttributesA(cand) != INVALID_FILE_ATTRIBUTES) {
            strncpy(src, cand, MAX_PATH - 1);
        }
    }

    // 4. Or already in the target library:
    if (!src[0]) {
        snprintf(cand, MAX_PATH, "%s\\steamapps\\appmanifest_%s.acf", target_library, app_id);
        if (GetFileAttributesA(cand) != INVALID_FILE_ATTRIBUTES) {
            strncpy(src, cand, MAX_PATH - 1);
        }
    }

    if (!src[0]) return 0;

    char dst_dir[MAX_PATH];
    snprintf(dst_dir, MAX_PATH, "%s\\steamapps", target_library);
    CreateDirectoryA(dst_dir, NULL);

    char dst[MAX_PATH];
    snprintf(dst, MAX_PATH, "%s\\appmanifest_%s.acf", dst_dir, app_id);

    // Already exactly where it has to be: nothing to move, nothing to delete.
    if (_stricmp(src, dst) == 0) return 1;

    char *buf = read_file_alloc(src);
    if (!buf) return 0;

    char tmp_path[MAX_PATH];
    snprintf(tmp_path, MAX_PATH, "%s\\appmanifest_%s.acf.tmp", dst_dir, app_id);

    HANDLE h = CreateFileA(tmp_path, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        HeapFree(GetProcessHeap(), 0, buf);
        return 0;
    }

    int write_ok = 1;
    char *p = buf;
    while (*p && write_ok) {
        char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) + 1 : strlen(p);

        // Rewrite installdir so the Steam client looks into the folder we
        // actually filled. Long lines are copied verbatim instead of being
        // truncated into the 1 KB scratch buffer.
        int is_installdir = 0;
        if (len < 1024) {
            char probe_line[1024];
            memcpy(probe_line, p, len);
            probe_line[len] = '\0';
            if (strstr(probe_line, "\"installdir\"")) is_installdir = 1;
        }

        DWORD written = 0;
        if (is_installdir) {
            char out_line[MAX_PATH + 64];
            int n = snprintf(out_line, sizeof(out_line),
                             "\t\"installdir\"\t\t\"%s\"\r\n", installdir);
            if (n < 0 || !WriteFile(h, out_line, (DWORD)strlen(out_line), &written, NULL) ||
                written != strlen(out_line))
                write_ok = 0;
        } else {
            if (!WriteFile(h, p, (DWORD)len, &written, NULL) || written != (DWORD)len)
                write_ok = 0;
        }
        p += len;
    }

    if (write_ok) FlushFileBuffers(h);
    CloseHandle(h);
    HeapFree(GetProcessHeap(), 0, buf);

    if (!write_ok) {
        DeleteFileA(tmp_path);
        return 0;
    }

    // Atomic swap. The old manifest survives untouched if this fails.
    if (!MoveFileExA(tmp_path, dst,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileA(tmp_path);
        return 0;
    }

    // Only now is it safe to drop SteamCMD's copy.
    DeleteFileA(src);
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
