// lease.c - Persistent lease file (JSON) for crash recovery
// Format: simple line-based key=value (no external JSON lib needed)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "lease.h"
#include "api.h"

// ---------------------------------------------------------------------------
// Write lease.json
// Format: one "key=value\n" per line (simple, no escaping needed for our values)
// ---------------------------------------------------------------------------
int lease_write(const char *path, const LeaseFile *lf) {
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fprintf(f, "lease_token=%s\n", lf->lease_token);
    fprintf(f, "pc_id=%s\n",       lf->pc_id);
    fprintf(f, "account_id=%s\n",  lf->account_id);
    fprintf(f, "app_id=%s\n",      lf->app_id);
    fprintf(f, "server_url=%s\n",  lf->server_url);
    fprintf(f, "api_key=%s\n",     lf->api_key);
    fclose(f);
    return 1;
}

// ---------------------------------------------------------------------------
// Read lease.json
// ---------------------------------------------------------------------------
static void kv_extract(const char *line, const char *key, char *out, int out_size) {
    int klen = (int)strlen(key);
    if (strncmp(line, key, klen) == 0 && line[klen] == '=') {
        const char *val = line + klen + 1;
        // strip trailing newline
        int vlen = (int)strlen(val);
        while (vlen > 0 && (val[vlen-1] == '\n' || val[vlen-1] == '\r')) vlen--;
        if (vlen >= out_size) vlen = out_size - 1;
        memcpy(out, val, vlen);
        out[vlen] = '\0';
    }
}

int lease_read(const char *path, LeaseFile *out) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    memset(out, 0, sizeof(LeaseFile));
    char line[600];
    while (fgets(line, sizeof(line), f)) {
        kv_extract(line, "lease_token", out->lease_token, sizeof(out->lease_token));
        kv_extract(line, "pc_id",       out->pc_id,       sizeof(out->pc_id));
        kv_extract(line, "account_id",  out->account_id,  sizeof(out->account_id));
        kv_extract(line, "app_id",      out->app_id,      sizeof(out->app_id));
        kv_extract(line, "server_url",  out->server_url,  sizeof(out->server_url));
        kv_extract(line, "api_key",     out->api_key,     sizeof(out->api_key));
    }
    fclose(f);
    return (out->lease_token[0] != '\0') ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Delete lease file
// ---------------------------------------------------------------------------
void lease_delete(const char *path) {
    DeleteFileA(path);
}

// ---------------------------------------------------------------------------
// Startup recovery: if lease.json exists, release it on VPS and delete
// ---------------------------------------------------------------------------
int lease_recover_on_startup(const char *path) {
    LeaseFile lf = {0};
    if (!lease_read(path, &lf)) return 0;

    // Re-init API with credentials from lease file
    api_init(lf.server_url, lf.api_key);

    // Send /release with result=interrupted (we crashed, unknown real result)
    api_release(lf.lease_token, lf.pc_id,
                RELEASE_INTERRUPTED,
                lf.app_id,
                "Client crashed or was killed",
                NULL, NULL);

    // Delete lease file regardless of whether release call succeeded
    // (server will cleanup via heartbeat timeout anyway)
    lease_delete(path);

    return 1;
}
