// main.c - Steam Auto-Updater client, Win32 GUI entry point
// Native Win32 UI: no external frameworks, single .exe
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "acf.h"
#include "worker.h"
#include "lease.h"
#include "catalog.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "/subsystem:windows")

// ---------------------------------------------------------------------------
// Control IDs
// ---------------------------------------------------------------------------
#define ID_LIST_GAMES      101
#define ID_BTN_UPDATE      102
#define ID_BTN_SETTINGS    103
#define ID_BTN_ABORT       104
#define ID_PROGRESS        105
#define ID_STATUS_TEXT     106
#define ID_LOG_TEXT        107
#define ID_BTN_UPDATE_ALL  108
#define ID_BTN_REFRESH     109
#define ID_QUEUE_TEXT      110
#define ID_TIMER_REFRESH   1001

#define MAX_ROWS 128

// ---------------------------------------------------------------------------
// UI model
// ---------------------------------------------------------------------------
typedef struct {
    char appid[32];
    char name[256];
    char size[32];
    char buildid[32];
    char status[64];
    int  installed;
    int  is_f2p;
} GameRow;

static HWND          g_hwnd           = NULL;
static HWND          g_list           = NULL;
static HWND          g_btn_update     = NULL;
static HWND          g_btn_update_all = NULL;
static HWND          g_btn_refresh    = NULL;
static HWND          g_btn_abort      = NULL;
static HWND          g_progress       = NULL;
static HWND          g_status_txt     = NULL;
static HWND          g_queue_txt      = NULL;
static HWND          g_log_txt        = NULL;

static Config        g_cfg                 = {0};
static char          g_cfg_path[MAX_PATH]  = {0};
static GameRow       g_rows[MAX_ROWS]      = {0};
static int           g_row_count           = 0;

static HANDLE        g_worker      = NULL;
static WorkerStatus  g_wstatus     = {0};

// Sequential queue of row indices
static int           g_queue[MAX_ROWS] = {0};
static int           g_queue_len       = 0;
static int           g_queue_pos       = 0;
static int           g_current_row     = -1;
static int           g_ok_count        = 0;
static int           g_fail_count      = 0;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
static void      RefreshGameList(void);
static void      StartQueue(void);
static int       StartRow(int row_idx);
static void      UpdateUIFromWorker(void);
static void      ShowSettingsDialog(HWND parent);
static void      AppendLog(const char *line);
static void      SetQueueText(void);

static const char *state_name(WorkerState s) {
    switch (s) {
        case WORKER_IDLE:          return "Idle";
        case WORKER_ACQUIRING:     return "Acquiring account...";
        case WORKER_KILLING_STEAM: return "Closing Steam...";
        case WORKER_RUNNING:       return "Working...";
        case WORKER_RELEASING:     return "Releasing account...";
        case WORKER_DONE_OK:       return "Done!";
        case WORKER_DONE_FAIL:     return "Failed";
        default:                   return "Unknown";
    }
}

// ---------------------------------------------------------------------------
// WinMain
// ---------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
    (void)hPrev; (void)lpCmd;

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);

    GetModuleFileNameA(NULL, g_cfg_path, MAX_PATH);
    char *last_slash = strrchr(g_cfg_path, '\\');
    if (last_slash) *(last_slash + 1) = '\0';
    strncat(g_cfg_path, "updater.ini", MAX_PATH - strlen(g_cfg_path) - 1);
    int cfg_ok = config_load(g_cfg_path, &g_cfg);

    InitializeCriticalSection(&g_wstatus.lock);

    if (cfg_ok && g_cfg.server_url[0] && g_cfg.api_key[0]) {
        WorkerConfig wcfg = {0};
        strncpy(wcfg.server_url, g_cfg.server_url, sizeof(wcfg.server_url)-1);
        strncpy(wcfg.api_key,    g_cfg.api_key,    sizeof(wcfg.api_key)-1);
        if (worker_check_stale_lease(&wcfg)) {
            MessageBoxA(NULL,
                "Previous session crash detected.\nStale account lease released automatically.",
                "Crash Recovery", MB_OK | MB_ICONWARNING);
        }
    }

    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = "SteamAutoUpdater";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    g_hwnd = CreateWindowExA(0, "SteamAutoUpdater",
                              "Steam Auto-Updater v2.0 (F2P)",
                              WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, 820, 620,
                              NULL, NULL, hInst, NULL);
    ShowWindow(g_hwnd, nShow);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    DeleteCriticalSection(&g_wstatus.lock);
    return (int)msg.wParam;
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {

    case WM_CREATE: {
        HINSTANCE hi = GetModuleHandle(NULL);

        g_list = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEWA, "",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            8, 8, 560, 230, hwnd, (HMENU)ID_LIST_GAMES, hi, NULL);
        ListView_SetExtendedListViewStyle(g_list,
            LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

        LVCOLUMNA col = {0};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx = 70;  col.pszText = (LPSTR)"AppID";   ListView_InsertColumn(g_list, 0, &col);
        col.cx = 210; col.pszText = (LPSTR)"Game";    ListView_InsertColumn(g_list, 1, &col);
        col.cx = 80;  col.pszText = (LPSTR)"Size";    ListView_InsertColumn(g_list, 2, &col);
        col.cx = 90;  col.pszText = (LPSTR)"BuildID"; ListView_InsertColumn(g_list, 3, &col);
        col.cx = 150; col.pszText = (LPSTR)"Status";  ListView_InsertColumn(g_list, 4, &col);

        g_btn_update = CreateWindowA("BUTTON", "Update selected",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            578, 8, 210, 30, hwnd, (HMENU)ID_BTN_UPDATE, hi, NULL);

        g_btn_update_all = CreateWindowA("BUTTON", "Update ALL checked",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_DEFPUSHBUTTON,
            578, 46, 210, 34, hwnd, (HMENU)ID_BTN_UPDATE_ALL, hi, NULL);

        g_btn_refresh = CreateWindowA("BUTTON", "Refresh list",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            578, 88, 210, 30, hwnd, (HMENU)ID_BTN_REFRESH, hi, NULL);

        CreateWindowA("BUTTON", "Settings",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            578, 126, 210, 30, hwnd, (HMENU)ID_BTN_SETTINGS, hi, NULL);

        g_btn_abort = CreateWindowA("BUTTON", "Abort",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
            578, 164, 210, 30, hwnd, (HMENU)ID_BTN_ABORT, hi, NULL);

        g_progress = CreateWindowExA(0, PROGRESS_CLASSA, "",
            WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
            8, 246, 780, 22, hwnd, (HMENU)ID_PROGRESS, hi, NULL);
        SendMessage(g_progress, PBM_SETRANGE, 0, MAKELPARAM(0, 1000));

        g_status_txt = CreateWindowA("STATIC", "Idle",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            8, 272, 780, 20, hwnd, (HMENU)ID_STATUS_TEXT, hi, NULL);

        g_queue_txt = CreateWindowA("STATIC", "Queue: empty",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            8, 294, 780, 20, hwnd, (HMENU)ID_QUEUE_TEXT, hi, NULL);

        g_log_txt = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            8, 318, 780, 240, hwnd, (HMENU)ID_LOG_TEXT, hi, NULL);
        SendMessage(g_log_txt, EM_SETLIMITTEXT, 65536, 0);

        HFONT hfont = CreateFontA(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
        SendMessage(g_log_txt, WM_SETFONT, (WPARAM)hfont, TRUE);

        RefreshGameList();
        SetTimer(hwnd, ID_TIMER_REFRESH, 200, NULL);
        break;
    }

    case WM_TIMER:
        if (wp == ID_TIMER_REFRESH && g_worker) {
            UpdateUIFromWorker();
        }
        break;

    case WM_COMMAND: {
        int ctrl_id = LOWORD(wp);

        if (ctrl_id == ID_BTN_UPDATE || ctrl_id == ID_BTN_UPDATE_ALL) {
            if (GetFileAttributesA(g_cfg.steamcmd_path) == INVALID_FILE_ATTRIBUTES) {
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
            g_fail_count = 0;

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
            }
            StartQueue();
        }
        else if (ctrl_id == ID_BTN_ABORT) {
            worker_request_abort();
            g_queue_len = g_queue_pos;   // stop after the current game
            EnableWindow(g_btn_abort, FALSE);
            SetWindowTextA(g_status_txt, "Aborting...");
        }
        else if (ctrl_id == ID_BTN_SETTINGS) {
            ShowSettingsDialog(hwnd);
        }
        else if (ctrl_id == ID_BTN_REFRESH) {
            RefreshGameList();
        }
        break;
    }

    case WM_SIZE: {
        int w = LOWORD(lp), h = HIWORD(lp);
        if (w > 0 && h > 0) {
            MoveWindow(g_list,           8,     8, w-240, 230, TRUE);
            MoveWindow(g_btn_update,     w-225, 8,   210, 30, TRUE);
            MoveWindow(g_btn_update_all, w-225, 46,  210, 34, TRUE);
            MoveWindow(g_btn_refresh,    w-225, 88,  210, 30, TRUE);
            MoveWindow(g_btn_abort,      w-225, 164, 210, 30, TRUE);
            MoveWindow(g_progress,   8, 246, w-16, 22, TRUE);
            MoveWindow(g_status_txt, 8, 272, w-16, 20, TRUE);
            MoveWindow(g_queue_txt,  8, 294, w-16, 20, TRUE);
            MoveWindow(g_log_txt,    8, 318, w-16, h-326, TRUE);
        }
        break;
    }

    case WM_DESTROY:
        if (g_worker) {
            g_queue_len = g_queue_pos;
            worker_request_abort();
            WaitForSingleObject(g_worker, 15000);
            CloseHandle(g_worker);
            g_worker = NULL;
        }
        PostQuitMessage(0);
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
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
        strncpy(r->status, "Not installed", sizeof(r->status)-1);
        strncpy(r->size,   "-",            sizeof(r->size)-1);

        for (int j = 0; j < n_inst; j++) {
            if (strcmp(installed[j].appid, cat[i].appid) != 0) continue;
            r->installed = 1;
            strncpy(r->buildid, installed[j].buildid, sizeof(r->buildid)-1);
            double gb = atof(installed[j].size_on_disk) / (1024.0*1024.0*1024.0);
            snprintf(r->size, sizeof(r->size), "%.2f GB", gb);
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

    ListView_DeleteAllItems(g_list);
    for (int i = 0; i < g_row_count; i++) {
        LVITEMA item = {0};
        item.mask    = LVIF_TEXT;
        item.iItem   = i;
        item.pszText = g_rows[i].appid;
        ListView_InsertItem(g_list, &item);
        ListView_SetItemText(g_list, i, 1, g_rows[i].name);
        ListView_SetItemText(g_list, i, 2, g_rows[i].size);
        ListView_SetItemText(g_list, i, 3, g_rows[i].buildid);
        ListView_SetItemText(g_list, i, 4, g_rows[i].status);
        // Pre-check everything that is already installed; unchecked F2P rows
        // can be ticked manually to install them from scratch.
        ListView_SetCheckState(g_list, i, g_rows[i].installed ? TRUE : FALSE);
    }

    char buf[128];
    snprintf(buf, sizeof(buf), "List refreshed: %d rows (%d installed on this PC).",
             g_row_count, n_inst);
    AppendLog(buf);

    if (n_inst == 0) {
        AppendLog("WARNING: no Steam library found. Check that the Steam client is installed.");
    }
}

// ---------------------------------------------------------------------------
// Queue handling
// ---------------------------------------------------------------------------
static void SetQueueText(void) {
    char buf[256];
    if (g_queue_len == 0) {
        snprintf(buf, sizeof(buf), "Queue: empty");
    } else {
        int cur = g_queue_pos > 0 ? g_queue_pos : 1;
        snprintf(buf, sizeof(buf), "Queue: %d / %d   |   OK: %d   Failed: %d",
                 cur, g_queue_len, g_ok_count, g_fail_count);
    }
    SetWindowTextA(g_queue_txt, buf);
}

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

    strncpy(g_rows[row_idx].status, g_rows[row_idx].installed ? "Updating..." : "Installing...",
            sizeof(g_rows[row_idx].status)-1);
    ListView_SetItemText(g_list, row_idx, 4, g_rows[row_idx].status);

    g_current_row = row_idx;
    g_worker = worker_start(&cfg, &g_wstatus);
    return g_worker != NULL;
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

    EnableWindow(g_btn_update, FALSE);
    EnableWindow(g_btn_update_all, FALSE);
    EnableWindow(g_btn_refresh, FALSE);
    EnableWindow(g_btn_abort, TRUE);

    g_queue_pos = 0;
    SetQueueText();
    StartRow(g_queue[g_queue_pos++]);
}

// ---------------------------------------------------------------------------
// UpdateUIFromWorker (called every 200ms from the timer)
// ---------------------------------------------------------------------------
static void UpdateUIFromWorker(void) {
    EnterCriticalSection(&g_wstatus.lock);
    WorkerState state  = g_wstatus.state;
    double      pct    = g_wstatus.progress;
    char        desc[64] = {0}, logline[512] = {0}, errmsg[256] = {0};
    strncpy(desc,    g_wstatus.state_desc,   63);   desc[63] = '\0';
    strncpy(logline, g_wstatus.last_log_line, 511); logline[511] = '\0';
    {
        int n = (int)strlen(logline);
        while (n > 0 && ((unsigned char)logline[n - 1] & 0xC0) == 0x80) n--;
        if (n > 0 && ((unsigned char)logline[n - 1] & 0x80)) n--;
        logline[n] = '\0';
    }
    strncpy(errmsg,  g_wstatus.error_msg,    255);  errmsg[255] = '\0';
    LeaveCriticalSection(&g_wstatus.lock);

    SendMessage(g_progress, PBM_SETPOS, (WPARAM)(pct * 10.0), 0);

    char status_buf[320];
    const char *game = (g_current_row >= 0 && g_current_row < g_row_count)
                     ? g_rows[g_current_row].name : "";
    if (desc[0])
        snprintf(status_buf, sizeof(status_buf), "%s - %s %.1f%% (%s)",
                 game, state_name(state), pct, desc);
    else
        snprintf(status_buf, sizeof(status_buf), "%s - %s", game, state_name(state));
    SetWindowTextA(g_status_txt, status_buf);

    static char last_log[512] = {0};
    if (logline[0] && strcmp(logline, last_log) != 0) {
        strncpy(last_log, logline, 511);
        last_log[511] = '\0';
        AppendLog(logline);
    }

    if (state != WORKER_DONE_OK && state != WORKER_DONE_FAIL) return;

    HANDLE hWorker = g_worker;
    g_worker = NULL;   // prevent re-entrant timer calls

    if (g_current_row >= 0 && g_current_row < g_row_count) {
        if (state == WORKER_DONE_OK) {
            g_ok_count++;
            strncpy(g_rows[g_current_row].status, "OK", sizeof(g_rows[g_current_row].status)-1);
            g_rows[g_current_row].installed = 1;
            AppendLog(">>> Done.");
        } else {
            g_fail_count++;
            snprintf(g_rows[g_current_row].status, sizeof(g_rows[g_current_row].status),
                     "FAILED: %s", errmsg[0] ? errmsg : "unknown");
            char fail_msg[512];
            snprintf(fail_msg, sizeof(fail_msg), ">>> Failed: %s",
                     errmsg[0] ? errmsg : "Unknown error");
            AppendLog(fail_msg);
        }
        ListView_SetItemText(g_list, g_current_row, 4, g_rows[g_current_row].status);
    }

    if (hWorker) CloseHandle(hWorker);
    SetQueueText();

    // Next game in the queue?
    if (g_queue_pos < g_queue_len) {
        StartRow(g_queue[g_queue_pos++]);
        SetQueueText();
        return;
    }

    // Queue finished
    g_current_row = -1;
    EnableWindow(g_btn_update, TRUE);
    EnableWindow(g_btn_update_all, TRUE);
    EnableWindow(g_btn_refresh, TRUE);
    EnableWindow(g_btn_abort, FALSE);

    char summary[256];
    snprintf(summary, sizeof(summary),
             "Finished. Success: %d, failed: %d.", g_ok_count, g_fail_count);
    SetWindowTextA(g_status_txt, summary);
    AppendLog(summary);
    MessageBoxA(g_hwnd, summary, "Update finished",
                MB_OK | (g_fail_count ? MB_ICONWARNING : MB_ICONINFORMATION));
}

// ---------------------------------------------------------------------------
// AppendLog
// ---------------------------------------------------------------------------
static void AppendLog(const char *line) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, line, -1, NULL, 0);
    if (wlen <= 0) return;

    WCHAR  stackbuf[1024];
    WCHAR *wline = stackbuf;
    if (wlen > (int)(sizeof(stackbuf) / sizeof(stackbuf[0]))) {
        wline = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (size_t)wlen * sizeof(WCHAR));
        if (!wline) return;
    }
    MultiByteToWideChar(CP_UTF8, 0, line, -1, wline, wlen);

    int len = GetWindowTextLengthW(g_log_txt);
    SendMessageW(g_log_txt, EM_SETSEL, len, len);
    SendMessageW(g_log_txt, EM_REPLACESEL, FALSE, (LPARAM)wline);
    SendMessageW(g_log_txt, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
    SendMessageW(g_log_txt, WM_VSCROLL, SB_BOTTOM, 0);

    if (wline != stackbuf) HeapFree(GetProcessHeap(), 0, wline);
}

// ---------------------------------------------------------------------------
// Settings Dialog
// ---------------------------------------------------------------------------
#define ID_EDIT_URL    201
#define ID_EDIT_KEY    202
#define ID_EDIT_CMD    203
#define ID_EDIT_PCID   204
#define ID_BTN_SAVE    205
#define ID_BTN_BROWSE  206

LRESULT CALLBACK SettingsDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        Config *cfg = (Config *)((CREATESTRUCTA*)lp)->lpCreateParams;

        CreateWindowA("STATIC", "Server URL:",
            WS_CHILD|WS_VISIBLE, 10, 10, 100, 20, hwnd, NULL, NULL, NULL);
        CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", cfg->server_url,
            WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
            10, 32, 380, 22, hwnd, (HMENU)ID_EDIT_URL, NULL, NULL);

        CreateWindowA("STATIC", "API Key:",
            WS_CHILD|WS_VISIBLE, 10, 62, 100, 20, hwnd, NULL, NULL, NULL);
        CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", cfg->api_key,
            WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL|ES_PASSWORD,
            10, 84, 380, 22, hwnd, (HMENU)ID_EDIT_KEY, NULL, NULL);

        CreateWindowA("STATIC", "steamcmd.exe path:",
            WS_CHILD|WS_VISIBLE, 10, 114, 150, 20, hwnd, NULL, NULL, NULL);
        CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", cfg->steamcmd_path,
            WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
            10, 136, 300, 22, hwnd, (HMENU)ID_EDIT_CMD, NULL, NULL);
        CreateWindowA("BUTTON", "...",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            318, 136, 72, 22, hwnd, (HMENU)ID_BTN_BROWSE, NULL, NULL);

        CreateWindowA("STATIC", "PC ID:",
            WS_CHILD|WS_VISIBLE, 10, 166, 100, 20, hwnd, NULL, NULL, NULL);
        CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", cfg->pc_id,
            WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
            10, 188, 200, 22, hwnd, (HMENU)ID_EDIT_PCID, NULL, NULL);

        CreateWindowA("BUTTON", "Save",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_DEFPUSHBUTTON,
            310, 218, 80, 28, hwnd, (HMENU)ID_BTN_SAVE, NULL, NULL);
        break;
    }

    case WM_COMMAND: {
        if (LOWORD(wp) == ID_BTN_SAVE) {
            GetWindowTextA(GetDlgItem(hwnd, ID_EDIT_URL),  g_cfg.server_url,    sizeof(g_cfg.server_url));
            GetWindowTextA(GetDlgItem(hwnd, ID_EDIT_KEY),  g_cfg.api_key,       sizeof(g_cfg.api_key));
            GetWindowTextA(GetDlgItem(hwnd, ID_EDIT_CMD),  g_cfg.steamcmd_path, sizeof(g_cfg.steamcmd_path));
            GetWindowTextA(GetDlgItem(hwnd, ID_EDIT_PCID), g_cfg.pc_id,         sizeof(g_cfg.pc_id));
            config_save(g_cfg_path, &g_cfg);
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
    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = SettingsDlgProc;
    wc.hInstance     = GetModuleHandle(NULL);
    wc.lpszClassName = "SettingsDlg";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassA(&wc);

    CreateWindowExA(WS_EX_DLGMODALFRAME, "SettingsDlg", "Settings",
                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                    CW_USEDEFAULT, CW_USEDEFAULT, 410, 280,
                    parent, NULL, GetModuleHandle(NULL), &g_cfg);
}
