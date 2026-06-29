// config.c - INI config file reader/writer (uses Windows GetPrivateProfileString)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "config.h"

#define SECTION "AutoUpdater"

void config_detect_pc_id(char *out, int out_size) {
    DWORD sz = (DWORD)out_size;
    if (!GetComputerNameA(out, &sz)) {
        strncpy(out, "PC-UNKNOWN", out_size - 1);
    }
}

int config_load(const char *path, Config *out) {
    memset(out, 0, sizeof(Config));

    // GetPrivateProfileString returns default if key not found
    GetPrivateProfileStringA(SECTION, "server_url",    "",
                             out->server_url,    sizeof(out->server_url),    path);
    GetPrivateProfileStringA(SECTION, "api_key",       "",
                             out->api_key,       sizeof(out->api_key),       path);
    GetPrivateProfileStringA(SECTION, "steamcmd_path", "",
                             out->steamcmd_path, sizeof(out->steamcmd_path), path);
    GetPrivateProfileStringA(SECTION, "pc_id",         "",
                             out->pc_id,         sizeof(out->pc_id),         path);

    // Auto-detect pc_id if empty
    if (!out->pc_id[0]) {
        config_detect_pc_id(out->pc_id, sizeof(out->pc_id));
    }

    // Create default config if file doesn't exist
    if (!out->server_url[0]) {
        strncpy(out->server_url,    "https://your-vps.com", sizeof(out->server_url)-1);
        strncpy(out->api_key,       "CHANGE_ME",            sizeof(out->api_key)-1);
        strncpy(out->steamcmd_path, "C:\\steamcmd\\steamcmd.exe", sizeof(out->steamcmd_path)-1);
        config_save(path, out);
        return 0; // indicates defaults were written
    }

    return 1;
}

int config_save(const char *path, const Config *cfg) {
    WritePrivateProfileStringA(SECTION, "server_url",    cfg->server_url,    path);
    WritePrivateProfileStringA(SECTION, "api_key",       cfg->api_key,       path);
    WritePrivateProfileStringA(SECTION, "steamcmd_path", cfg->steamcmd_path, path);
    WritePrivateProfileStringA(SECTION, "pc_id",         cfg->pc_id,         path);
    return 1;
}
