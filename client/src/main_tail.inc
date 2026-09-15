// main_tail.inc - queue, worker polling, on-screen log and the settings dialog.
//
// This file is #included at the very end of main.c. It exists only so the UI
// layer (design tokens, painting, layout) and the behaviour layer can be read
// separately without touching the build: the compiler still sees one
// translation unit, and the gcc/Makefile file list is unchanged.
//
// Nothing here changed behaviourally. The block below maps the old palette and
// font names onto the new semantic tokens defined in main.c, so this code keeps
// compiling unmodified while the visual language comes from one place.
#ifndef CLR_PANEL
#define CLR_PANEL          CLR_SURFACE
#define CLR_PANEL_ALT      CLR_SURFACE_ALT
#define CLR_ACCENT         CLR_ACTION
#define CLR_ACCENT_HOVER   CLR_ACTION_HOVER
#define CLR_ACCENT_DOWN    CLR_ACTION_ACTIVE
#define g_br_panel         g_br_surface
#define g_br_panel_alt     g_br_surface_a
#define g_font_bold        g_font_display
#endif

static int StartRow(int row_idx) {
    EnterCriticalSection(&g_wstatus.lock);
    g_wstatus.state = WORKER_IDLE;
    g_wstatus.progress = 0.0;
    g_wstatus.state_desc[0]     = '\0';
    g_wstatus.last_log_line[0]  = '\0';
    g_wstatus.error_msg[0]      = '\0';
    g_wstatus.build_id_before[0]= '\0';
    g_wstatus.build_id_after[0] = '\0';
    LeaveCriticalSection(&g_wstatus.lock);

    WorkerConfig cfg = {0};
    strncpy(cfg.server_url,    g_cfg.server_url,    sizeof(cfg.server_url)-1);
    strncpy(cfg.api_key,       g_cfg.api_key,       sizeof(cfg.api_key)-1);
    strncpy(cfg.pc_id,         g_cfg.pc_id,         sizeof(cfg.pc_id)-1);
    strncpy(cfg.steamcmd_path, g_cfg.steamcmd_path, sizeof(cfg.steamcmd_path)-1);
    strncpy(cfg.app_id,        g_rows[row_idx].appid, sizeof(cfg.app_id)-1);
    cfg.jitter_disabled = 1;   // GUI-triggered: the admin is watching

    char buf[320];
    snprintf(buf, sizeof(buf), ">>> %s %s (App %s)...",
             g_rows[row_idx].installed ? "Updating" : "Installing",
             g_rows[row_idx].name, g_rows[row_idx].appid);
    AppendLog(buf);
    LOG_INFO("ui", "starting row %d: app %s (%s), installed=%d, local build %s",
             row_idx, g_rows[row_idx].appid, g_rows[row_idx].name,
             g_rows[row_idx].installed,
             g_rows[row_idx].buildid[0] ? g_rows[row_idx].buildid : "-");

    strncpy(g_rows[row_idx].status, g_rows[row_idx].installed ? "Updating..." : "Installing...",
            sizeof(g_rows[row_idx].status)-1);
    g_rows[row_idx].result = 3;
    ListView_SetItemText(g_list, row_idx, 1, g_rows[row_idx].status);
    ListView_EnsureVisible(g_list, row_idx, FALSE);

    g_current_row = row_idx;
    ProgressReset();
    DrawProgress(0.0, g_rows[row_idx].installed ? "starting update" : "starting install", 0, 0);

    g_worker = worker_start(&cfg, &g_wstatus);
    if (!g_worker) {
        AppendLog("ERROR: failed to start the worker thread.");
        LOG_ERROR("ui", "CreateThread for the worker failed (app %s)", g_rows[row_idx].appid);
        g_fail_count++;
        strncpy(g_rows[row_idx].status, "FAILED: thread", sizeof(g_rows[row_idx].status)-1);
        g_rows[row_idx].result = 2;
        ListView_SetItemText(g_list, row_idx, 1, g_rows[row_idx].status);
        return 0;
    }
    return 1;
}

static void StartQueue(void) {
    if (g_worker) {
        DWORD exit_code = STILL_ACTIVE;
        GetExitCodeThread(g_worker, &exit_code);
        if (exit_code == STILL_ACTIVE) {
            MessageBoxA(g_hwnd, "An update is already running.", "Busy", MB_OK | MB_ICONWARNING);
            return;
        }
        CloseHandle(g_worker);
        g_worker = NULL;
    }

    SetControlsBusy(1);
    keep_awake(1);

    g_queue_pos = 0;
    SetQueueText();

    // Skip rows that fail to start instead of dead-ending the whole queue.
    while (g_queue_pos < g_queue_len) {
        int row = g_queue[g_queue_pos++];
        if (StartRow(row)) break;
    }
    if (!g_worker) {
        SetControlsBusy(0);
        keep_awake(0);
    }
    SetQueueText();
}

// ---------------------------------------------------------------------------
// UpdateUIFromWorker (called every 100ms from the timer)
// ---------------------------------------------------------------------------
static void UpdateUIFromWorker(void) {
    EnterCriticalSection(&g_wstatus.lock);
    WorkerState state  = g_wstatus.state;
    double      pct    = g_wstatus.progress;
    char        desc[64] = {0}, logline[512] = {0}, errmsg[256] = {0};
    strncpy(desc,    g_wstatus.state_desc,   63);   desc[63] = '\0';
    strncpy(logline, g_wstatus.last_log_line, 511); logline[511] = '\0';
    {
        // Never cut a UTF-8 sequence in half: that used to paint mojibake in the log.
        int n = (int)strlen(logline);
        while (n > 0 && ((unsigned char)logline[n - 1] & 0xC0) == 0x80) n--;
        if (n > 0 && ((unsigned char)logline[n - 1] & 0x80)) n--;
        logline[n] = '\0';
    }
    strncpy(errmsg,  g_wstatus.error_msg,    255);  errmsg[255] = '\0';
    LeaveCriticalSection(&g_wstatus.lock);

    static char      last_log[512] = {0};
    static ULONGLONG last_progress_log = 0;
    if (logline[0] && strcmp(logline, last_log) != 0) {
        // steamcmd repeats "Update state (0x61) downloading, progress: ..."
        // several times per second. The bar already shows that; the log box
        // only needs a sample now and then. The debug log file and the server
        // still receive every line from worker.c / steamcmd.c.
        int is_progress = (strstr(logline, "Update state") != NULL);
        ULONGLONG now = GetTickCount64();
        if (!is_progress || now - last_progress_log >= LOG_PROGRESS_THROTTLE_MS) {
            if (is_progress) last_progress_log = now;
            strncpy(last_log, logline, 511);
            last_log[511] = '\0';
            AppendLog(logline);
        }
    }

    if (state != WORKER_DONE_OK && state != WORKER_DONE_FAIL) {
        ProgressUpdate(state, pct, desc, logline);

        char status_buf[380];
        const char *game = (g_current_row >= 0 && g_current_row < g_row_count)
                         ? g_rows[g_current_row].name : "";
        char el[32];
        fmt_secs((double)((GetTickCount64() - g_tick_job_start) / 1000), el, sizeof(el));
        snprintf(status_buf, sizeof(status_buf), "%s   %s   running for %s%s",
                 game, state_name(state), el, g_aborting ? "   (stopping)" : "");
        char old_status[380] = {0};
        GetWindowTextA(g_status_txt, old_status, sizeof(old_status) - 1);
        if (strcmp(old_status, status_buf) != 0) SetWindowTextA(g_status_txt, status_buf);
        SetQueueText();
        return;
    }

    HANDLE hWorker = g_worker;
    g_worker = NULL;   // prevent re-entrant timer calls

    if (g_current_row >= 0 && g_current_row < g_row_count) {
        if (state == WORKER_DONE_OK) {
            char st[64];
            EnterCriticalSection(&g_wstatus.lock);
            int same_build = (g_wstatus.build_id_before[0] && g_wstatus.build_id_after[0] &&
                              strcmp(g_wstatus.build_id_before, g_wstatus.build_id_after) == 0);
            strncpy(g_rows[g_current_row].buildid, g_wstatus.build_id_after,
                    sizeof(g_rows[g_current_row].buildid)-1);
            LeaveCriticalSection(&g_wstatus.lock);
            // "already latest" now really means nothing was downloaded and no
            // files were hashed: the build id matched before we started.
            if (same_build) g_skip_count++; else g_ok_count++;
            snprintf(st, sizeof(st), same_build ? "OK (already latest)" : "OK (updated)");
            strncpy(g_rows[g_current_row].status, st, sizeof(g_rows[g_current_row].status)-1);
            g_rows[g_current_row].installed = 1;
            g_rows[g_current_row].result    = 1;
            ListView_SetItemText(g_list, g_current_row, 3, g_rows[g_current_row].buildid);
            DrawProgress(100.0, same_build ? "already up to date" : "complete", 0, 0);
            AppendLog(same_build ? ">>> Already up to date, nothing downloaded." : ">>> Done.");
            LOG_INFO("ui", "row done: app %s -> %s (build %s)",
                     g_rows[g_current_row].appid, st, g_rows[g_current_row].buildid);
        } else {
            g_fail_count++;
            snprintf(g_rows[g_current_row].status, sizeof(g_rows[g_current_row].status),
                     "FAILED: %s", errmsg[0] ? errmsg : "unknown");
            g_rows[g_current_row].result = 2;
            DrawProgress(100.0, errmsg[0] ? errmsg : "failed", 1, 0);
            char fail_msg[512];
            snprintf(fail_msg, sizeof(fail_msg), ">>> Failed: %s",
                     errmsg[0] ? errmsg : "Unknown error");
            AppendLog(fail_msg);
            LOG_ERROR("ui", "row failed: app %s: %s",
                      g_rows[g_current_row].appid, errmsg[0] ? errmsg : "unknown");
        }
        ListView_SetItemText(g_list, g_current_row, 1, g_rows[g_current_row].status);
    }

    if (hWorker) CloseHandle(hWorker);
    SetQueueText();

    // Next game in the queue?
    while (g_queue_pos < g_queue_len) {
        int row = g_queue[g_queue_pos++];
        if (StartRow(row)) { SetQueueText(); return; }
    }

    // Queue finished
    g_current_row = -1;
    SetControlsBusy(0);
    keep_awake(0);

    char summary[256];
    snprintf(summary, sizeof(summary),
             "%s Updated: %d, already up to date: %d, failed: %d.",
             g_aborting ? "Stopped." : "Finished.",
             g_ok_count, g_skip_count, g_fail_count);
    SetWindowTextA(g_status_txt, summary);
    DrawProgress(100.0, summary, g_fail_count ? 1 : 0, 0);
    AppendLog(summary);
    LOG_INFO("ui", "%s", summary);
    log_flush_remote(4000);

    // Refresh sizes / build ids so the list matches reality after the run.
    RefreshGameList();

    // Flash taskbar to gently notify operator without ugly white popup
    FlashWindow(g_hwnd, TRUE);
}

// ---------------------------------------------------------------------------
// AppendLog: on-screen log + updater.log next to the exe + debug log/server.
// ---------------------------------------------------------------------------
static void AppendLog(const char *line) {
    if (!line || !line[0]) return;

    // Everything the operator sees also goes into the verbose log and to the
    // server, so a remote session can be reconstructed line by line.
    log_raw("ui", line);

    if (g_logfile[0]) {
        FILE *f = fopen(g_logfile, "a");
        if (f) {
            time_t t = time(NULL);
            struct tm *lt = localtime(&t);
            if (lt)
                fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] %s\n",
                        lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday,
                        lt->tm_hour, lt->tm_min, lt->tm_sec, line);
            else
                fprintf(f, "%s\n", line);
            fclose(f);
        }
    }

    if (!g_log_txt) return;

    // Keep the control from growing forever during a long night run.
    int len = GetWindowTextLengthW(g_log_txt);
    if (len > 200000) {
        SendMessageW(g_log_txt, EM_SETSEL, 0, len / 2);
        SendMessageW(g_log_txt, EM_REPLACESEL, FALSE, (LPARAM)L"[...older lines trimmed...]\r\n");
        len = GetWindowTextLengthW(g_log_txt);
    }

    // steamcmd speaks UTF-8 most of the time, but a localized Windows build
    // can emit CP1251/OEM bytes. A failed UTF-8 conversion used to produce the
    // garbage characters in the log, so fall back to the ANSI code page.
    int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, line, -1, NULL, 0);
    UINT cp = CP_UTF8;
    if (wlen <= 0) {
        cp   = CP_ACP;
        wlen = MultiByteToWideChar(CP_ACP, 0, line, -1, NULL, 0);
    }
    if (wlen <= 0) return;

    WCHAR  stackbuf[1024];
    WCHAR *wline = stackbuf;
    if (wlen > (int)(sizeof(stackbuf) / sizeof(stackbuf[0]))) {
        wline = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (size_t)wlen * sizeof(WCHAR));
        if (!wline) return;
    }
    MultiByteToWideChar(cp, 0, line, -1, wline, wlen);

    // Strip control characters: steamcmd draws its own progress with \r and
    // backspaces, and those printed straight on top of the previous line.
    for (int i = 0; wline[i]; i++)
        if (wline[i] == L'\r' || wline[i] == L'\n' || wline[i] == L'\b' || wline[i] == L'\t')
            wline[i] = L' ';

    SendMessageW(g_log_txt, EM_SETSEL, len, len);
    SendMessageW(g_log_txt, EM_REPLACESEL, FALSE, (LPARAM)wline);
    SendMessageW(g_log_txt, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
    SendMessageW(g_log_txt, EM_SCROLLCARET, 0, 0);

    if (wline != stackbuf) HeapFree(GetProcessHeap(), 0, wline);
}

// ---------------------------------------------------------------------------
// Settings dialog
// ---------------------------------------------------------------------------
#define ID_EDIT_URL    201
#define ID_EDIT_KEY    202
#define ID_EDIT_CMD    203
#define ID_EDIT_PCID   204
#define ID_BTN_SAVE    205
#define ID_BTN_BROWSE  206

static HWND make_label(HWND parent, const char *text, int x, int y, int cx) {
    HWND h = CreateWindowA("STATIC", text, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                           x, y, cx, 20, parent, NULL, GetModuleHandle(NULL), NULL);
    SendMessage(h, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
    return h;
}

static HWND make_edit(HWND parent, const char *text, int id, int x, int y, int cx, DWORD extra) {
    HWND h = CreateWindowExA(0, "EDIT", text,
                             WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP
                             | ES_AUTOHSCROLL | extra,
                             x, y, cx, 30, parent, (HMENU)(LONG_PTR)id,
                             GetModuleHandle(NULL), NULL);
    SendMessage(h, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
    enable_dark_control(h);
    return h;
}

LRESULT CALLBACK SettingsDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        FillRect(dc, &ps.rcPaint, g_br_bg);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CREATE: {
        Config *cfg = (Config *)((CREATESTRUCTA*)lp)->lpCreateParams;
        enable_dark_titlebar(hwnd);

        make_label(hwnd, "Server URL", 20, 16, 200);
        make_edit(hwnd, cfg->server_url, ID_EDIT_URL, 20, 38, 390, 0);

        make_label(hwnd, "API key", 20, 78, 200);
        make_edit(hwnd, cfg->api_key, ID_EDIT_KEY, 20, 100, 390, ES_PASSWORD);

        make_label(hwnd, "Path to steamcmd.exe", 20, 140, 260);
        make_edit(hwnd, cfg->steamcmd_path, ID_EDIT_CMD, 20, 162, 302, 0);
        make_button(hwnd, "Browse", ID_BTN_BROWSE, 330, 162, 80, 30, 0);

        make_label(hwnd, "PC ID", 20, 202, 200);
        make_edit(hwnd, cfg->pc_id, ID_EDIT_PCID, 20, 224, 210, 0);

        make_button(hwnd, "Save", ID_BTN_SAVE, 310, 268, 100, 38, 0);
        break;
    }

    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, CLR_TEXT_DIM);
        SetBkColor(dc, CLR_BG);
        return (LRESULT)g_br_bg;
    }

    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, OPAQUE);
        SetTextColor(dc, CLR_TEXT);
        SetBkColor(dc, CLR_SURFACE_ALT);
        return (LRESULT)g_br_surface_a;
    }

    case WM_DRAWITEM:
        return SendMessage(g_hwnd, WM_DRAWITEM, wp, lp);

    case WM_COMMAND: {
        if (LOWORD(wp) == ID_BTN_SAVE) {
            char url[512] = {0}, key[256] = {0}, cmd[MAX_PATH] = {0}, pcid[64] = {0};
            GetWindowTextA(GetDlgItem(hwnd, ID_EDIT_URL),  url,  sizeof(url));
            GetWindowTextA(GetDlgItem(hwnd, ID_EDIT_KEY),  key,  sizeof(key));
            GetWindowTextA(GetDlgItem(hwnd, ID_EDIT_CMD),  cmd,  sizeof(cmd));
            GetWindowTextA(GetDlgItem(hwnd, ID_EDIT_PCID), pcid, sizeof(pcid));

            // Validate before saving: a typo here means 50 machines fail at 5 AM.
            if (_strnicmp(url, "http://", 7) != 0 && _strnicmp(url, "https://", 8) != 0) {
                MessageBoxA(hwnd, "Server URL must start with http:// or https://",
                            "Invalid URL", MB_OK | MB_ICONERROR);
                return 0;
            }
            size_t ulen = strlen(url);
            while (ulen > 0 && url[ulen-1] == '/') url[--ulen] = '\0';  // no double slashes
            if (!key[0]) {
                MessageBoxA(hwnd, "API key cannot be empty.", "Invalid key", MB_OK | MB_ICONERROR);
                return 0;
            }
            if (cmd[0] && GetFileAttributesA(cmd) == INVALID_FILE_ATTRIBUTES) {
                if (MessageBoxA(hwnd, "steamcmd.exe was not found at that path. Save anyway?",
                                "Path not found", MB_YESNO | MB_ICONWARNING) != IDYES)
                    return 0;
            }
            if (!pcid[0]) {
                // Fall back to the machine name so the server log is still usable.
                DWORD n = sizeof(pcid);
                GetComputerNameA(pcid, &n);
            }

            strncpy(g_cfg.server_url,    url,  sizeof(g_cfg.server_url)-1);
            strncpy(g_cfg.api_key,       key,  sizeof(g_cfg.api_key)-1);
            strncpy(g_cfg.steamcmd_path, cmd,  sizeof(g_cfg.steamcmd_path)-1);
            strncpy(g_cfg.pc_id,         pcid, sizeof(g_cfg.pc_id)-1);

            if (!config_save(g_cfg_path, &g_cfg)) {
                MessageBoxA(hwnd,
                    "Could not write updater.ini.\nRun the updater as administrator or move it "
                    "out of Program Files.", "Save failed", MB_OK | MB_ICONERROR);
                return 0;
            }
            // Logging targets can change with the settings: re-point them now.
            log_set_pc_id(g_cfg.pc_id);
            log_set_remote(g_cfg.server_url, g_cfg.api_key, 1);
            LOG_INFO("ui", "settings saved: pc_id=%s server=%s", g_cfg.pc_id, g_cfg.server_url);
            AppendLog("Settings saved.");
            DestroyWindow(hwnd);
        }
        else if (LOWORD(wp) == ID_BTN_BROWSE) {
            OPENFILENAMEA ofn = {0};
            char path[MAX_PATH] = {0};
            GetWindowTextA(GetDlgItem(hwnd, ID_EDIT_CMD), path, MAX_PATH);
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner   = hwnd;
            ofn.lpstrFilter = "steamcmd.exe\0steamcmd.exe\0All Files\0*.*\0";
            ofn.lpstrFile   = path;
            ofn.nMaxFile    = MAX_PATH;
            ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameA(&ofn)) {
                SetWindowTextA(GetDlgItem(hwnd, ID_EDIT_CMD), path);
            }
        }
        break;
    }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void ShowSettingsDialog(HWND parent) {
    static int registered = 0;
    if (!registered) {
        WNDCLASSA wc = {0};
        wc.lpfnWndProc   = SettingsDlgProc;
        wc.hInstance     = GetModuleHandle(NULL);
        wc.lpszClassName = "SettingsDlg";
        wc.hbrBackground = NULL;
        wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
        RegisterClassA(&wc);
        registered = 1;
    }

    CreateWindowExA(WS_EX_DLGMODALFRAME, "SettingsDlg", "Settings",
                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE | WS_CLIPCHILDREN,
                    CW_USEDEFAULT, CW_USEDEFAULT, 460, 360,
                    parent, NULL, GetModuleHandle(NULL), &g_cfg);
}
