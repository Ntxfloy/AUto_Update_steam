// config.h - INI config file reader/writer
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef struct {
    char server_url[512];       // https://my-vps.com
    char api_key[256];          // Bearer token
    char steamcmd_path[MAX_PATH]; // C:\steamcmd\steamcmd.exe
    char pc_id[64];             // e.g. "PC-04" (auto-detected from hostname if empty)
} Config;

// Load config from file (creates default if missing)
// Returns 1 on success
int  config_load(const char *path, Config *out);

// Save config to file
int  config_save(const char *path, const Config *cfg);

// Auto-detect pc_id from hostname
void config_detect_pc_id(char *out, int out_size);
