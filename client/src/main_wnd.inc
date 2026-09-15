// main_wnd.inc - included from main.c.
// The window procedure: painting, control creation, owner-drawn buttons and
// progress bar, list-view colours, commands and shutdown handling.

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {

    // Swallow the erase pass; WM_PAINT below fills the background once.
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        FillRect(dc, &ps.rcPaint, g_br_bg);
        paint_card(dc, hwnd, g_list, RADIUS_LG);
        paint_card(dc, hwnd, g_log_txt, RADIUS_LG);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CREATE: {
        HINSTANCE hi = GetModuleHandle(NULL);

        g_title_txt = CreateWindowA("STATIC", "Steam Auto-Updater",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | SS_LEFT | SS_ENDELLIPSIS,
            PAD, 26, 600, 46, hwnd, (HMENU)ID_TITLE_TEXT, hi, NULL);
        SendMessage(g_title_txt, WM_SETFONT, (WPARAM)g_font_display, TRUE);

        g_sub_txt = CreateWindowA("STATIC",
            "Tick the games, then press Update. Fresh installs go to a non-system disk only.",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | SS_LEFT | SS_ENDELLIPSIS,
            PAD, 78, 600, 20, hwnd, (HMENU)ID_SUBTITLE_TEXT, hi, NULL);
        SendMessage(g_sub_txt, WM_SETFONT, (WPARAM)g_font_ui, TRUE);

        g_list = CreateWindowExA(0, WC_LISTVIEWA, "",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP | LVS_REPORT
            | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
            PAD, Y_CONTENT, 660, 320, hwnd, (HMENU)ID_LIST_GAMES, hi, NULL);
        ListView_SetExtendedListViewStyle(g_list,
            LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        SendMessage(g_list, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
        ListView_SetBkColor(g_list, CLR_SURFACE);
        ListView_SetTextBkColor(g_list, CLR_SURFACE);
        ListView_SetTextColor(g_list, CLR_TEXT);

        // 15px text in an 18px row was the cramped look. A 1x26 image list is
        // the supported way to give a report-mode ListView taller rows.
        g_row_il = ImageList_Create(1, 26, ILC_COLOR32, 1, 1);
        if (g_row_il) ListView_SetImageList(g_list, g_row_il, LVSIL_SMALL);

        HWND hdr = ListView_GetHeader(g_list);
        if (hdr) {
            SendMessage(hdr, WM_SETFONT, (WPARAM)g_font_label, TRUE);
            g_hdr_oldproc = (WNDPROC)SetWindowLongPtrA(hdr, GWLP_WNDPROC, (LONG_PTR)HeaderSubProc);
        }

        // Col 0 is the game name, so the checkbox sits next to the title.
        LVCOLUMNA col = {0};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx = 240; col.pszText = (LPSTR)"GAME";   ListView_InsertColumn(g_list, 0, &col);
        col.cx = 150; col.pszText = (LPSTR)"STATUS"; ListView_InsertColumn(g_list, 1, &col);
        col.cx = 80;  col.pszText = (LPSTR)"SIZE";   ListView_InsertColumn(g_list, 2, &col);
        col.cx = 90;  col.pszText = (LPSTR)"BUILD";  ListView_InsertColumn(g_list, 3, &col);
        col.cx = 70;  col.pszText = (LPSTR)"APPID";  ListView_InsertColumn(g_list, 4, &col);

        enable_dark_control(g_list);

        int bx = PAD + 660 + COL_GAP;

        g_lbl_run = make_group_label(hwnd, "RUN", ID_LBL_GROUP_RUN, bx, Y_CONTENT, RIGHT_W);
        g_btn_update_all  = make_button(hwnd, "Update checked games", ID_BTN_UPDATE_ALL,  bx, 0, RIGHT_W, H_BTN_PRIMARY, 0);
        g_btn_update      = make_button(hwnd, "Update selected row",  ID_BTN_UPDATE,      bx, 0, RIGHT_W, H_BTN, 0);
        g_btn_abort       = make_button(hwnd, "Stop",                 ID_BTN_ABORT,       bx, 0, RIGHT_W, H_BTN, 1);

        g_lbl_sel = make_group_label(hwnd, "SELECTION", ID_LBL_GROUP_SEL, bx, 0, RIGHT_W);
        g_btn_check_all   = make_button(hwnd, "Select all",        ID_BTN_CHECK_ALL,   bx, 0, RIGHT_W, H_BTN_SM, 0);
        g_btn_uncheck_all = make_button(hwnd, "Clear selection",   ID_BTN_UNCHECK_ALL, bx, 0, RIGHT_W, H_BTN_SM, 0);
        g_btn_check_f2p   = make_button(hwnd, "Only free-to-play", ID_BTN_CHECK_F2P,   bx, 0, RIGHT_W, H_BTN_SM, 0);

        g_lbl_svc = make_group_label(hwnd, "SERVICE", ID_LBL_GROUP_SVC, bx, 0, RIGHT_W);
        g_btn_refresh     = make_button(hwnd, "Refresh list",   ID_BTN_REFRESH,  bx, 0, RIGHT_W, H_BTN_SM, 0);
        g_btn_settings    = make_button(hwnd, "Settings",       ID_BTN_SETTINGS, bx, 0, RIGHT_W, H_BTN_SM, 0);
        g_btn_open_log    = make_button(hwnd, "Open debug log", ID_BTN_OPEN_LOG, bx, 0, RIGHT_W, H_BTN_SM, 0);

        // Our own progress bar: a static we paint ourselves, so it can be dark
        // and can show percentage, stage, bytes, speed and ETA inside the bar.
        g_progress = CreateWindowA("STATIC", "",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | SS_OWNERDRAW,
            PAD, 0, 660, H_PROGRESS, hwnd, (HMENU)ID_PROGRESS, hi, NULL);
        g_progress_oldproc = (WNDPROC)SetWindowLongPtrA(g_progress, GWLP_WNDPROC, (LONG_PTR)ProgressSubProc);

        g_status_txt = CreateWindowA("STATIC", "Idle. Nothing is running.",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | SS_LEFT | SS_ENDELLIPSIS,
            PAD, 0, 660, H_STATUS, hwnd, (HMENU)ID_STATUS_TEXT, hi, NULL);
        SendMessage(g_status_txt, WM_SETFONT, (WPARAM)g_font_ui, TRUE);

        g_queue_txt = CreateWindowA("STATIC", "Queue: empty",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | SS_LEFT | SS_ENDELLIPSIS,
            PAD, 0, 660, H_QUEUE, hwnd, (HMENU)ID_QUEUE_TEXT, hi, NULL);
        SendMessage(g_queue_txt, WM_SETFONT, (WPARAM)g_font_ui, TRUE);

        g_log_txt = CreateWindowExA(0, "EDIT", "",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_VSCROLL | WS_TABSTOP
            | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            PAD, 0, 660, H_LOG_MIN, hwnd, (HMENU)ID_LOG_TEXT, hi, NULL);
        SendMessage(g_log_txt, EM_SETLIMITTEXT, 262144, 0);
        SendMessage(g_log_txt, WM_SETFONT, (WPARAM)g_font_mono, TRUE);
        enable_dark_control(g_log_txt);

        RECT rc;
        GetClientRect(hwnd, &rc);
        LayoutAll(rc.right - rc.left, rc.bottom - rc.top);

        ProgressReset();
        DrawProgress(0.0, "idle", 0, 0);
        RefreshGameList();

        // One-time config sanity warnings, printed instead of silently failing later
        if (!g_cfg.server_url[0] || !g_cfg.api_key[0])
            AppendLog("WARNING: server URL / API key not set. Open Settings first.");
        else if (_strnicmp(g_cfg.server_url, "http://", 7) == 0)
            AppendLog("WARNING: server_url uses plain http:// - Steam passwords travel unencrypted "
                      "over the club LAN. Switch the server to https.");
        if (g_cfg.steamcmd_path[0] &&
            GetFileAttributesA(g_cfg.steamcmd_path) == INVALID_FILE_ATTRIBUTES)
            AppendLog("WARNING: steamcmd.exe not found at the configured path.");
        if (!g_cfg.pc_id[0])
            AppendLog("WARNING: pc_id is empty. The server log will not show which PC this is.");

        char sess[160];
        snprintf(sess, sizeof(sess),
                 "Debug log: %s   (session %s, mirrored to the server)",
                 g_dbglog, log_session_id());
        AppendLog(sess);

        SetTimer(hwnd, ID_TIMER_REFRESH, 100, NULL);
        break;
    }

    // --- dark colours for the standard controls --------------------------
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        HWND ctl = (HWND)lp;
        if (ctl == g_log_txt) {
            // The log is a read-only EDIT, so it arrives here. It MUST paint
            // opaque text: with TRANSPARENT background mode the control never
            // clears the old glyphs, and every scroll stacked new lines on top
            // of the previous ones - that was the unreadable mess in the log.
            SetBkMode(dc, OPAQUE);
            SetTextColor(dc, CLR_TEXT_DIM);
            SetBkColor(dc, CLR_SURFACE);
            return (LRESULT)g_br_surface;
        }
        SetBkMode(dc, TRANSPARENT);
        if (ctl == g_title_txt)            SetTextColor(dc, CLR_TEXT);
        else if (ctl == g_sub_txt)         SetTextColor(dc, CLR_TEXT_DIM);
        else if (ctl == g_lbl_run || ctl == g_lbl_sel || ctl == g_lbl_svc)
                                           SetTextColor(dc, CLR_TEXT_DIM);
        else if (ctl == g_queue_txt)       SetTextColor(dc, CLR_TEXT_DIM);
        else                               SetTextColor(dc, CLR_TEXT);
        SetBkColor(dc, CLR_BG);
        return (LRESULT)g_br_bg;
    }

    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        HWND ctl = (HWND)lp;
        SetBkMode(dc, OPAQUE);
        if (ctl == g_log_txt) {
            SetTextColor(dc, CLR_TEXT_DIM);
            SetBkColor(dc, CLR_SURFACE);
            return (LRESULT)g_br_surface;
        }
        SetTextColor(dc, CLR_TEXT);
        SetBkColor(dc, CLR_SURFACE_ALT);
        return (LRESULT)g_br_surface_a;
    }

    // --- owner-drawn buttons + progress bar ------------------------------
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *di = (DRAWITEMSTRUCT *)lp;

        if (di->CtlID == ID_PROGRESS) {
            RECT rc;
            GetClientRect(di->hwndItem, &rc);
            FillRect(di->hDC, &rc, g_br_bg);
            fill_round(di->hDC, rc, CLR_SURFACE_ALT, CLR_SEPARATOR, RADIUS_MD);

            double p = g_pct;
            if (p < 0.0)   p = 0.0;
            if (p > 100.0) p = 100.0;
            int track = rc.right - rc.left - 6;
            int w = (int)(track * (p / 100.0));
            if (w > track) w = track;

            RECT fr = { rc.left + 3, rc.top + 3, rc.left + 3 + w, rc.bottom - 3 };
            if (w > 4) {
                COLORREF c = g_pct_failed ? CLR_FAIL_FILL
                           : g_pct_warn   ? CLR_WARN
                           : (p >= 99.999 ? CLR_OK : CLR_ACTION);
                fill_round(di->hDC, fr, c, c, RADIUS_MD - 2);
            }

            // The label crosses the fill edge, so it is drawn twice with a
            // clip: one colour always failed contrast on one of the halves.
            SetBkMode(di->hDC, TRANSPARENT);
            HFONT of = (HFONT)SelectObject(di->hDC, g_font_ui);
            int dark_on_fill = (g_pct_warn || (!g_pct_failed && p >= 99.999));

            if (w > 4) {
                HRGN clip = CreateRectRgn(fr.left, fr.top, fr.right, fr.bottom);
                SelectClipRgn(di->hDC, clip);
                SetTextColor(di->hDC, dark_on_fill ? CLR_BG : CLR_ON_ACTION);
                DrawTextA(di->hDC, g_pct_label, -1, &rc,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                SelectClipRgn(di->hDC, NULL);
                DeleteObject(clip);
            }
            HRGN rest = CreateRectRgn(fr.right, rc.top, rc.right, rc.bottom);
            SelectClipRgn(di->hDC, rest);
            SetTextColor(di->hDC, CLR_TEXT);
            DrawTextA(di->hDC, g_pct_label, -1, &rc,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectClipRgn(di->hDC, NULL);
            DeleteObject(rest);

            SelectObject(di->hDC, of);
            return TRUE;
        }

        if (di->CtlType == ODT_BUTTON) {
            RECT rc;
            GetClientRect(di->hwndItem, &rc);
            FillRect(di->hDC, &rc, g_br_bg);

            int hovered     = (int)GetWindowLongPtrA(di->hwndItem, GWLP_USERDATA);
            int pressed     = (di->itemState & ODS_SELECTED) ? 1 : 0;
            int disabled    = (di->itemState & ODS_DISABLED) ? 1 : 0;
            int focused     = (di->itemState & ODS_FOCUS) ? 1 : 0;
            int primary     = (di->CtlID == ID_BTN_UPDATE_ALL);
            int destructive = (di->CtlID == ID_BTN_ABORT);

            COLORREF fill, border, text;
            if (disabled) {
                // Disabled controls are exempt from the contrast minimum, but
                // they must still read as "not available", not as "empty".
                fill = CLR_SURFACE; border = CLR_SEPARATOR; text = CLR_TEXT_DISABLED;
            } else if (primary) {
                fill   = pressed ? CLR_ACTION_ACTIVE : (hovered ? CLR_ACTION_HOVER : CLR_ACTION);
                border = fill;
                text   = CLR_ON_ACTION;
            } else if (destructive) {
                fill   = pressed ? CLR_SURFACE_ACTIVE : (hovered ? CLR_SURFACE_HOVER : CLR_SURFACE);
                border = (hovered || pressed) ? CLR_FAIL : CLR_BORDER;
                text   = CLR_FAIL;
            } else {
                fill   = pressed ? CLR_SURFACE_ACTIVE : (hovered ? CLR_SURFACE_HOVER : CLR_SURFACE);
                border = CLR_BORDER;
                text   = CLR_TEXT;
            }

            fill_round(di->hDC, rc, fill, border, RADIUS_MD);
            if (focused && !disabled) draw_focus_ring(di->hDC, rc, RADIUS_MD - 2);

            char txt[128] = {0};
            GetWindowTextA(di->hwndItem, txt, sizeof(txt) - 1);
            SetBkMode(di->hDC, TRANSPARENT);
            SetTextColor(di->hDC, text);
            HFONT of = (HFONT)SelectObject(di->hDC, g_font_ui);
            DrawTextA(di->hDC, txt, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(di->hDC, of);
            return TRUE;
        }
        break;
    }

    // --- list view: custom colours, checkbox persistence, dbl-click ------
    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)lp;

        if (nh->idFrom == ID_LIST_GAMES && nh->code == NM_DBLCLK) {
            if (!g_worker) SendMessage(hwnd, WM_COMMAND, ID_BTN_UPDATE, 0);
            return 0;
        }

        // Remember what the operator ticked, so the next launch starts with
        // exactly the same set of games instead of guessing.
        if (nh->idFrom == ID_LIST_GAMES && nh->code == LVN_ITEMCHANGED) {
            NMLISTVIEW *nlv = (NMLISTVIEW *)lp;
            if ((nlv->uChanged & LVIF_STATE) &&
                ((nlv->uOldState ^ nlv->uNewState) & LVIS_STATEIMAGEMASK))
                SelectionSave();
        }

        if (nh->idFrom == ID_LIST_GAMES && nh->code == NM_CUSTOMDRAW) {
            NMLVCUSTOMDRAW *cd = (NMLVCUSTOMDRAW *)lp;
            switch (cd->nmcd.dwDrawStage) {
            case CDDS_PREPAINT:
                return CDRF_NOTIFYITEMDRAW;
            case CDDS_ITEMPREPAINT:
                return CDRF_NOTIFYSUBITEMDRAW;
            case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
                int row = (int)cd->nmcd.dwItemSpec;
                cd->clrTextBk = (row % 2) ? CLR_SURFACE_ALT : CLR_SURFACE;
                if (cd->iSubItem == 1 && row >= 0 && row < g_row_count) {
                    switch (g_rows[row].result) {
                    case 1:  cd->clrText = CLR_OK;   break;
                    case 2:  cd->clrText = CLR_FAIL; break;
                    case 3:  cd->clrText = CLR_WARN; break;
                    default:
                        if (strstr(g_rows[row].status, "Installed"))
                            cd->clrText = CLR_OK;
                        else if (strstr(g_rows[row].status, "Working") || strstr(g_rows[row].status, "Updating"))
                            cd->clrText = CLR_INFO;
                        else
                            cd->clrText = CLR_TEXT_DIM;
                        break;
                    }
                } else if (cd->iSubItem == 0 && row >= 0 && row < g_row_count) {
                    cd->clrText = g_rows[row].installed ? CLR_TEXT : CLR_TEXT_DIM;
                } else {
                    cd->clrText = CLR_TEXT_DIM;
                }
                return CDRF_NEWFONT;
            }
            }
        }
        break;
    }

    case WM_TIMER:
        if (wp == ID_TIMER_REFRESH && g_worker) UpdateUIFromWorker();
        break;

    case WM_COMMAND: {
        int ctrl_id = LOWORD(wp);

        if (ctrl_id == ID_BTN_UPDATE || ctrl_id == ID_BTN_UPDATE_ALL) {
            if (!g_cfg.steamcmd_path[0] ||
                GetFileAttributesA(g_cfg.steamcmd_path) == INVALID_FILE_ATTRIBUTES) {
                MessageBoxA(hwnd, "steamcmd.exe not found. Check Settings.",
                            "Configuration Error", MB_OK | MB_ICONERROR);
                return 0;
            }
            if (!g_cfg.server_url[0] || !g_cfg.api_key[0]) {
                MessageBoxA(hwnd, "Server URL / API key are not configured. Check Settings.",
                            "Configuration Error", MB_OK | MB_ICONERROR);
                return 0;
            }

            g_queue_len = 0;
            g_queue_pos = 0;
            g_ok_count  = 0;
            g_skip_count = 0;
            g_fail_count = 0;
            g_aborting   = 0;

            if (ctrl_id == ID_BTN_UPDATE) {
                int sel = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
                if (sel < 0 || sel >= g_row_count) {
                    MessageBoxA(hwnd, "Select a game in the list first.",
                                "No selection", MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
                g_queue[g_queue_len++] = sel;
            } else {
                for (int i = 0; i < g_row_count; i++) {
                    if (ListView_GetCheckState(g_list, i)) g_queue[g_queue_len++] = i;
                }
                if (g_queue_len == 0) {
                    MessageBoxA(hwnd, "Nothing is checked. Tick the games you want to update.",
                                "Empty queue", MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
                SelectionSave();
                // Paid titles cannot be updated by a pool account: a service
                // account simply does not own them.
                int paid = 0;
                for (int i = 0; i < g_queue_len; i++)
                    if (!g_rows[g_queue[i]].is_f2p) paid++;
                if (paid > 0) {
                    char q[360];
                    snprintf(q, sizeof(q),
                        "%d of the %d checked games are PAID titles.\n"
                        "Pool accounts do not own them, so those rows will fail with "
                        "\"No subscription\".\n\nRun anyway?",
                        paid, g_queue_len);
                    if (MessageBoxA(hwnd, q, "Paid games in the queue",
                                    MB_YESNO | MB_ICONWARNING) != IDYES)
                        return 0;
                }
                // Warn about mass fresh installs: that is tens of GB per PC.
                int fresh = 0;
                for (int i = 0; i < g_queue_len; i++)
                    if (!g_rows[g_queue[i]].installed) fresh++;
                if (fresh >= 3) {
                    char q[320];
                    snprintf(q, sizeof(q),
                        "%d of the %d checked games are NOT installed yet.\n"
                        "That means full downloads (tens of GB) on this PC.\n\nContinue?",
                        fresh, g_queue_len);
                    if (MessageBoxA(hwnd, q, "Confirm fresh installs",
                                    MB_YESNO | MB_ICONQUESTION) != IDYES)
                        return 0;
                }
            }
            LOG_INFO("ui", "queue started: %d game(s)", g_queue_len);
            StartQueue();
        }
        else if (ctrl_id == ID_BTN_CHECK_ALL) {
            g_sel_suppress = 1;
            for (int i = 0; i < g_row_count; i++)
                ListView_SetCheckState(g_list, i, TRUE);
            g_sel_suppress = 0;
            SelectionSave();
            AppendLog("All rows checked.");
        }
        else if (ctrl_id == ID_BTN_UNCHECK_ALL) {
            g_sel_suppress = 1;
            for (int i = 0; i < g_row_count; i++)
                ListView_SetCheckState(g_list, i, FALSE);
            g_sel_suppress = 0;
            SelectionSave();
            AppendLog("All rows unchecked.");
        }
        else if (ctrl_id == ID_BTN_CHECK_F2P) {
            int n = 0;
            g_sel_suppress = 1;
            for (int i = 0; i < g_row_count; i++) {
                int want = g_rows[i].is_f2p ? 1 : 0;
                ListView_SetCheckState(g_list, i, want ? TRUE : FALSE);
                n += want;
            }
            g_sel_suppress = 0;
            SelectionSave();
            char b[96];
            snprintf(b, sizeof(b), "Checked %d free-to-play rows, paid titles skipped.", n);
            AppendLog(b);
        }
        else if (ctrl_id == ID_BTN_OPEN_LOG) {
            OpenLogInNotepad();
        }
        else if (ctrl_id == ID_BTN_ABORT) {
            if (MessageBoxA(hwnd,
                    "Stop after the current game?\n\n"
                    "The running steamcmd will be terminated and the account released. "
                    "A partially downloaded game stays partial and resumes next time.",
                    "Stop", MB_YESNO | MB_ICONQUESTION) != IDYES)
                return 0;
            g_aborting = 1;
            worker_request_abort();
            g_queue_len = g_queue_pos;   // stop after the current game
            EnableWindow(g_btn_abort, FALSE);
            SetWindowTextA(g_status_txt, "Stopping after the current game...");
            AppendLog(">>> Stop requested by the operator.");
        }
        else if (ctrl_id == ID_BTN_SETTINGS) {
            ShowSettingsDialog(hwnd);
        }
        else if (ctrl_id == ID_BTN_REFRESH) {
            RefreshGameList();
        }
        break;
    }

    // --- do not let Windows sleep / reboot in the middle of an update ----
    case WM_QUERYENDSESSION:
        if (g_worker) {
            AppendLog("WARNING: shutdown/logoff requested while updating - blocked.");
            return FALSE;
        }
        return TRUE;

    case WM_POWERBROADCAST:
        if (wp == PBT_APMQUERYSUSPEND && g_worker) return BROADCAST_QUERY_DENY;
        break;

    case WM_SIZE:
        LayoutAll(LOWORD(lp), HIWORD(lp));
        break;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        mmi->ptMinTrackSize.x = 1000;
        mmi->ptMinTrackSize.y = 760;
        break;
    }

    case WM_CLOSE:
        if (g_worker) {
            if (MessageBoxA(hwnd,
                    "An update is still running.\n\n"
                    "Closing now will stop steamcmd and release the account. Close anyway?",
                    "Update in progress", MB_YESNO | MB_ICONWARNING) != IDYES)
                return 0;
        }
        SelectionSave();
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (g_worker) {
            g_queue_len = g_queue_pos;
            worker_request_abort();
            // Give the worker time to release the lease; otherwise the account
            // stays 'busy' on the server for up to 90 seconds.
            if (WaitForSingleObject(g_worker, 20000) == WAIT_TIMEOUT)
                AppendLog("WARNING: worker did not stop in 20s; server will free the lease itself.");
            CloseHandle(g_worker);
            g_worker = NULL;
        }
        keep_awake(0);
        KillTimer(hwnd, ID_TIMER_REFRESH);
        log_flush_remote(4000);
        PostQuitMessage(0);
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}
