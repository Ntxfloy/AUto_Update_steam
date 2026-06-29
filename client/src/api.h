// api.h - VPS server REST API client (WinHTTP, no external deps)
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Valid result values for api_release()
#define RELEASE_SUCCESS     "success"
#define RELEASE_FAILED      "failed"
#define RELEASE_INTERRUPTED "interrupted"

// Response from /accounts/acquire
typedef struct {
    int    account_id;
    char   login[128];
    char   password[256];   // plaintext, zero after use
    char   lease_token[64];
} ApiAcquireResult;

// Initialize the API client (call once at startup)
// server_url: e.g. "https://my-vps.com"
// api_key: Bearer token
void api_init(const char *server_url, const char *api_key);

// Acquire a Steam account from the VPS pool
// steam_id64: NULL for F2P (any account), non-null for paid game (specific owner)
// Returns 1 on success, 0 on failure
int  api_acquire(const char *pc_id, const char *app_id,
                 const char *steam_id64,
                 ApiAcquireResult *out);

// Send a heartbeat to keep the lease alive (call every 30s during update)
int  api_heartbeat(const char *lease_token, const char *pc_id);

// Release the account after update completes or fails
// result: use RELEASE_SUCCESS / RELEASE_FAILED / RELEASE_INTERRUPTED constants
int  api_release(const char *lease_token, const char *pc_id,
                 const char *result,       // "success"|"failed"|"interrupted"
                 const char *app_id,
                 const char *error_msg,
                 const char *build_id_before,
                 const char *build_id_after);
