// main_ui.inc - included from main.c.
// Progress engine, game list refresh and the status/queue helpers.
// Kept in a separate translation-unit fragment so main.c stays small; the
// build file list is unchanged (this file is #included, not compiled).

// ---------------------------------------------------------------------------
// Progress engine
// ---------------------------------------------------------------------------
static void DrawProgress(double pct, const char *label, int failed, int warn) {
    // Repaint only when something visibly changed: a 100 ms timer that
    // invalidates the bar unconditionally is its own source of flicker.
    char newlabel[192];
    if (label && label[0])
        snprintf(newlabel, sizeof(newlabel), "%.1f%%   %s", pct, label);
    else
        snprintf(newlabel, sizeof(newlabel), "%.1f%%", pct);

    int changed = (failed != g_pct_failed) || (warn != g_pct_warn) ||
                  (strcmp(newlabel, g_pct_label) != 0) ||
                  ((int)(pct * 10) != (int)(g_pct * 10));

    g_pct        = pct;
    g_pct_failed = failed;
    g_pct_warn   = warn;
    strncpy(g_pct_label, newlabel, sizeof(g_pct_label) - 1);
    g_pct_label[sizeof(g_pct_label) - 1] = '\0';

    if (changed && g_progress) InvalidateRect(g_progress, NULL, TRUE);
}

static void ProgressReset(void) {
    g_bytes_done   = 0;
    g_bytes_total  = 0;
    g_speed_bps    = 0.0;
    g_bytes_sample = 0;
    g_tick_bytes   = GetTickCount64();
    g_tick_sample  = g_tick_bytes;
    g_tick_job_start = g_tick_bytes;
    g_stage[0]     = '\0';
    g_stage_floor  = 0.0;
}

// Weighted, monotonic mapping from steamcmd's per-stage percentage to a single
// bar. steamcmd restarts its counter at 0 for every stage (download -> verify
// -> commit), which is exactly why the old bar jumped backwards.
static double stage_map(const char *stage, double raw) {
    if (raw < 0.0)   raw = 0.0;
    if (raw > 100.0) raw = 100.0;
    if (!stage || !stage[0]) return raw * 0.90;

    if (strstr(stage, "checking"))    return 0.5;                 // build-id probe
    if (strstr(stage, "prealloc"))    return 0.0  + raw * 0.02;   //  0 -  2 %
    if (strstr(stage, "download"))    return 2.0  + raw * 0.86;   //  2 - 88 %
    if (strstr(stage, "verif"))       return 88.0 + raw * 0.07;   // 88 - 95 %
    if (strstr(stage, "commit"))      return 95.0 + raw * 0.04;   // 95 - 99 %
    if (strstr(stage, "reconfig"))    return 99.0;
    if (strstr(stage, "validat"))     return 88.0 + raw * 0.07;
    if (strstr(stage, "already"))     return 100.0;
    return raw * 0.90;
}

static void ProgressUpdate(WorkerState state, double raw_pct,
                           const char *desc, const char *logline) {
    ULONGLONG now = GetTickCount64();

    // Stages before steamcmd even starts get a small synthetic slice so the bar
    // is never frozen at 0 while we talk to the server.
    if (state == WORKER_ACQUIRING || state == WORKER_KILLING_STEAM) {
        char lbl[160];
        snprintf(lbl, sizeof(lbl), "%s", state_name(state));
        DrawProgress(state == WORKER_ACQUIRING ? 1.0 : 2.0, lbl, 0, 0);
        return;
    }

    // Pull byte counters straight out of the steamcmd line:
    //   Update state (0x61) downloading, progress: 7.36 (371405745 / 5047000673)
    if (logline && logline[0]) {
        const char *par = strchr(logline, '(');
        while (par) {
            unsigned long long a = 0, b = 0;
            if (sscanf(par, "(%llu / %llu)", &a, &b) == 2 && b > 0) {
                // A new app or a new stage can shrink the totals; never let the
                // byte counter run backwards inside one job.
                if (a != g_bytes_done) g_tick_bytes = now;
                if (b < g_bytes_total && a < g_bytes_done) {
                    g_bytes_sample = 0;
                    g_speed_bps    = 0.0;
                }
                g_bytes_done  = a;
                g_bytes_total = b;
                break;
            }
            par = strchr(par + 1, '(');
        }
    }

    // Smoothed speed: EMA over ~1.5 second samples.
    if (now - g_tick_sample >= 1500) {
        double dt = (double)(now - g_tick_sample) / 1000.0;
        if (g_bytes_done >= g_bytes_sample && dt > 0.0) {
            double inst = (double)(g_bytes_done - g_bytes_sample) / dt;
            g_speed_bps = (g_speed_bps <= 0.0) ? inst : (g_speed_bps * 0.7 + inst * 0.3);
        }
        g_bytes_sample = g_bytes_done;
        g_tick_sample  = now;
    }

    if (desc && desc[0]) {
        if (_stricmp(desc, g_stage) != 0) {
            strncpy(g_stage, desc, sizeof(g_stage) - 1);
            g_stage[sizeof(g_stage) - 1] = '\0';
        }
    }

    double mapped = stage_map(g_stage, raw_pct);
    // Monotonic guard: the bar may stand still, but it never walks backwards.
    if (mapped < g_stage_floor) mapped = g_stage_floor;
    else                        g_stage_floor = mapped;

    char label[176];
    char cur[32] = {0}, tot[32] = {0}, spd[32] = {0}, eta[40] = {0};
    int stalled = (g_bytes_total > 0 && (now - g_tick_bytes) > STALL_WARN_MS);

    if (g_bytes_total > 0) {
        fmt_bytes(g_bytes_done,  cur, sizeof(cur));
        fmt_bytes(g_bytes_total, tot, sizeof(tot));
        if (g_speed_bps > 1024.0) {
            snprintf(spd, sizeof(spd), "%.1f MB/s", g_speed_bps / (1024.0*1024.0));
            double left = (double)(g_bytes_total - g_bytes_done) / g_speed_bps;
            char e[32];
            fmt_secs(left, e, sizeof(e));
            snprintf(eta, sizeof(eta), "ETA %s", e);
        }
    }

    if (stalled) {
        snprintf(label, sizeof(label), "%s   no new data for %llu s (network? antivirus?)",
                 g_stage[0] ? g_stage : "working",
                 (unsigned long long)((now - g_tick_bytes) / 1000));
    } else if (g_bytes_total > 0 && spd[0]) {
        snprintf(label, sizeof(label), "%s   %s / %s   %s   %s",
                 g_stage[0] ? g_stage : "working", cur, tot, spd, eta);
    } else if (g_bytes_total > 0) {
        snprintf(label, sizeof(label), "%s   %s / %s",
                 g_stage[0] ? g_stage : "working", cur, tot);
    } else {
        snprintf(label, sizeof(label), "%s", g_stage[0] ? g_stage : state_name(state));
    }

    DrawProgress(mapped, label, 0, stalled);
}

// ---------------------------------------------------------------------------
// RefreshGameList: built-in F2P catalog first, then any other installed game
// ---------------------------------------------------------------------------
static void RefreshGameList(void) {
    static AcfInfo installed[MAX_ROWS];
    int n_inst = acf_scan_libraries(installed, MAX_ROWS);

    g_row_count = 0;

    int cat_count = 0;
    const F2PGame *cat = catalog_all(&cat_count);

    for (int i = 0; i < cat_count && g_row_count < MAX_ROWS; i++) {
        GameRow *r = &g_rows[g_row_count++];
        memset(r, 0, sizeof(*r));
        strncpy(r->appid, cat[i].appid, sizeof(r->appid)-1);
        strncpy(r->name,  cat[i].name,  sizeof(r->name)-1);
        r->is_f2p = 1;
        // Small titles are flagged so there is an obvious candidate for a
        // first end-to-end test instead of a 60 GB download.
        if (catalog_is_small(cat[i].appid))
            strncpy(r->status, "Not installed (small)", sizeof(r->status)-1);
        else
            strncpy(r->status, "Not installed", sizeof(r->status)-1);
        strncpy(r->size,   "-",            sizeof(r->size)-1);

        for (int j = 0; j < n_inst; j++) {
            if (strcmp(installed[j].appid, cat[i].appid) != 0) continue;
            r->installed = 1;
            strncpy(r->buildid, installed[j].buildid, sizeof(r->buildid)-1);
            double gb = atof(installed[j].size_on_disk) / (1024.0*1024.0*1024.0);
            snprintf(r->size, sizeof(r->size), "%.2f GB", gb);
            // Show which disk it sits on: games on C: are updated in place,
            // fresh installs never land there.
            if (installed[j].library_root[0] && installed[j].library_root[1] == ':')
                snprintf(r->status, sizeof(r->status), "Installed (%c:)",
                         installed[j].library_root[0]);
            else
                strncpy(r->status, "Installed", sizeof(r->status)-1);
            break;
        }
    }

    for (int j = 0; j < n_inst && g_row_count < MAX_ROWS; j++) {
        if (catalog_find(installed[j].appid)) continue;
        GameRow *r = &g_rows[g_row_count++];
        memset(r, 0, sizeof(*r));
        strncpy(r->appid, installed[j].appid, sizeof(r->appid)-1);
        strncpy(r->name,  installed[j].name,  sizeof(r->name)-1);
        strncpy(r->buildid, installed[j].buildid, sizeof(r->buildid)-1);
        double gb = atof(installed[j].size_on_disk) / (1024.0*1024.0*1024.0);
        snprintf(r->size, sizeof(r->size), "%.2f GB", gb);
        r->installed = 1;
        r->is_f2p    = 0;
        strncpy(r->status, "Installed (paid)", sizeof(r->status)-1);
    }

    g_sel_suppress = 1;
    SendMessage(g_list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_list);
    for (int i = 0; i < g_row_count; i++) {
        LVITEMA item = {0};
        item.mask    = LVIF_TEXT;
        item.iItem   = i;
        item.pszText = g_rows[i].name;
        ListView_InsertItem(g_list, &item);
        ListView_SetItemText(g_list, i, 1, g_rows[i].status);
        ListView_SetItemText(g_list, i, 2, g_rows[i].size);
        ListView_SetItemText(g_list, i, 3, g_rows[i].buildid);
        ListView_SetItemText(g_list, i, 4, g_rows[i].appid);
        // Restore the operator's last choice; only fall back to "installed
        // free-to-play games" the very first time this PC runs the updater.
        int checked = g_sel_loaded
                    ? SelectionContains(g_rows[i].appid)
                    : (g_rows[i].installed && g_rows[i].is_f2p);
        ListView_SetCheckState(g_list, i, checked ? TRUE : FALSE);
    }
    SendMessage(g_list, WM_SETREDRAW, TRUE, 0);
    g_sel_suppress = 0;
    InvalidateRect(g_list, NULL, FALSE);

    char buf[200];
    snprintf(buf, sizeof(buf),
             "List refreshed: %d rows (%d installed on this PC)%s.",
             g_row_count, n_inst,
             g_sel_loaded ? ", previous selection restored" : "");
    AppendLog(buf);

    if (n_inst == 0)
        AppendLog("WARNING: no Steam library found. Check that the Steam client is installed.");

    // Warn if there is nowhere legal to install: every fresh install must go to
    // a non-system disk of at least 500 GB.
    char win_dir[MAX_PATH] = {0};
    GetWindowsDirectoryA(win_dir, MAX_PATH);
    int eligible = 0;
    for (int j = 0; j < n_inst; j++) {
        if (!installed[j].library_root[0] || installed[j].library_root[1] != ':') continue;
        if (win_dir[0] && (installed[j].library_root[0] | 32) == (win_dir[0] | 32)) continue;
        char root[4] = { installed[j].library_root[0], ':', '\\', '\0' };
        ULARGE_INTEGER freeb, totalb, avail;
        if (GetDiskFreeSpaceExA(root, &avail, &totalb, &freeb) &&
            totalb.QuadPart >= 500ULL*1000ULL*1000ULL*1000ULL) {
            eligible = 1;
            break;
        }
    }
    if (n_inst > 0 && !eligible)
        AppendLog("WARNING: no Steam library on a non-system disk of 500 GB or more. "
                  "Fresh installs will be refused. Add a library on D:/E: in the Steam client.");
}

// ---------------------------------------------------------------------------
// Status line helpers
// ---------------------------------------------------------------------------
static void SetQueueText(void) {
    char buf[300];
    if (g_queue_len == 0) {
        snprintf(buf, sizeof(buf), "Queue: empty");
    } else {
        int cur = g_queue_pos > 0 ? g_queue_pos : 1;
        char el[32] = {0};
        fmt_secs((double)((GetTickCount64() - g_tick_job_start) / 1000), el, sizeof(el));
        int pending = log_pending_count();
        snprintf(buf, sizeof(buf),
                 "Queue %d of %d   |   updated %d   up to date %d   failed %d   |   "
                 "current game running for %s%s",
                 cur, g_queue_len, g_ok_count, g_skip_count, g_fail_count, el,
                 pending > 50 ? "   |   log upload lagging" : "");
    }
    // Rewriting identical text 10 times per second makes the label flash.
    char old[300] = {0};
    GetWindowTextA(g_queue_txt, old, sizeof(old) - 1);
    if (strcmp(old, buf) != 0) SetWindowTextA(g_queue_txt, buf);
}

// Enable / disable everything that must not be touched mid-run.
static void SetControlsBusy(int busy) {
    EnableWindow(g_btn_update,      busy ? FALSE : TRUE);
    EnableWindow(g_btn_update_all,  busy ? FALSE : TRUE);
    EnableWindow(g_btn_refresh,     busy ? FALSE : TRUE);
    EnableWindow(g_btn_check_all,   busy ? FALSE : TRUE);
    EnableWindow(g_btn_uncheck_all, busy ? FALSE : TRUE);
    EnableWindow(g_btn_check_f2p,   busy ? FALSE : TRUE);
    EnableWindow(g_btn_abort,       busy ? TRUE  : FALSE);
}
