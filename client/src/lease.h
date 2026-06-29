// lease.h - Persistent lease file for crash recovery
// On acquire: write lease.json to disk
// On clean exit: delete it
// On startup: if found -> send /release to server (stale session cleanup)
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef struct {
    char lease_token[64];
    char pc_id[64];
    char account_id[16];
    char app_id[32];
    char server_url[512];
    char api_key[256];
} LeaseFile;

// Write lease to disk (call after acquiring account)
// path: full path to lease.json (e.g. same dir as .exe)
int  lease_write(const char *path, const LeaseFile *lf);

// Read lease from disk. Returns 1 if found and parsed.
int  lease_read(const char *path, LeaseFile *out);

// Delete lease file (call on clean exit)
void lease_delete(const char *path);

// On startup: if lease file exists, send /release to server and delete
// Returns 1 if a stale lease was found and released
int  lease_recover_on_startup(const char *path);
