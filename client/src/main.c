// main.c - Steam Auto-Updater client, Win32 GUI entry point
// Native Win32 UI: no external frameworks, single .exe
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "acf.h"
#include "worker.h"
#include "lease.h"

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
#define ID_TIMER_REFRESH   1001

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------
static HWND          g_hwnd        = NULL;
static HWND          g_list        = NULL;
static HWND          g_btn_update  = NULL;
static HWND          g_btn_abort   = NULL;
static HWND          g_progress    = NULL;
static HWND          g_status_txt  = NULL;
static HWND          g_log_txt     = NULL;

static Config        g_cfg         = {0};
static char          g_cfg_path[MAX_PATH] = {0};
static AcfInfo       g_games[128]  = {0};
static int           g_game_count  = 0;

static HANDLE        g_worker      = NULL;
static WorkerStatus  g_wstatus     = {0};

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
static void      RefreshGameList(void);
static void      StartUpdate(int game_idx);
static void      UpdateUIFromWorker(void);
static void      ShowSettingsDialog(HWND parent);
static void      AppendLog(const char *line);

// ---------------------------------------------------------------------------
// State name helper
// ---------------------------------------------------------------------------
static const char *state_name(WorkerState s) {
    switch (s) {
        case WORKER_IDLE:          return "Idle";
        case WORKER_ACQUIRING:     return "Acquiring account...";
        case WORKER_KILLING_STEAM: return "Closing Steam...";
        case WORKER_RUNNING:       return "Updating...";
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

    // Init common controls
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);

    // Load config
    GetModuleFileNameA(NULL, g_cfg_path, MAX_PATH);
    char *last_slash = strrchr(g_cfg_path, '\\');
    if (last_slash) *(last_slash + 1) = '\0';
    strncat(g_cfg_path, "updater.ini", MAX_PATH - strlen(g_cfg_path) - 1);
    int cfg_ok = config_load(g_cfg_path, &g_cfg);

    // Init worker status
    InitializeCriticalSection(&g_wstatus.lock);

    // Crash recovery: if lease.json exists from a previous crash, release it
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

    // Register window class
    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = "SteamAutoUpdater";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    // Create main window
    g_hwnd = CreateWindowExA(0, "SteamAutoUpdater",
                              "Steam Auto-Updater v1.0",
                              WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, 780, 560,
                              NULL, NULL, hInst, NULL);
    ShowWindow(g_hwnd, nShow);
    UpdateWindow(g_hwnd);

    // Message loop
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

        // Game list (ListView)
        g_list = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEWA, "",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            8, 8, 540, 200, hwnd, (HMENU)ID_LIST_GAMES, hi, NULL);

        // Columns: AppID | Name | Size | BuildID | Status
        LVCOLUMNA col = {0};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx = 70;  col.pszText = (LPSTR)"AppID";  ListView_InsertColumn(g_list, 0, &col);
        col.cx = 220; col.pszText = (LPSTR)"Game";   ListView_InsertColumn(g_list, 1, &col);
        col.cx = 70;  col.pszText = (LPSTR)"Size";   ListView_InsertColumn(g_list, 2, &col);
        col.cx = 100; col.pszText = (LPSTR)"BuildID"; ListView_InsertColumn(g_list, 3, &col);
        col.cx = 80;  col.pszText = (LPSTR)"Flags";  ListView_InsertColumn(g_list, 4, &col);

        // Buttons
        g_btn_update = CreateWindowA("BUTTON", "Update Selected",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            558, 8, 190, 30, hwnd, (HMENU)ID_BTN_UPDATE, hi, NULL);

        CreateWindowA("BUTTON", "Refresh List",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            558, 46, 190, 30, hwnd, (HMENU)ID_BTN_SETTINGS + 10, hi, NULL);

        CreateWindowA("BUTTON", "Update ALL Games",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            558, 84, 190, 30, hwnd, (HMENU)ID_BTN_UPDATE_ALL, hi, NULL);

        CreateWindowA("BUTTON", "Settings",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            558, 122, 190, 30, hwnd, (HMENU)ID_BTN_SETTINGS, hi, NULL);

        g_btn_abort = CreateWindowA("BUTTON", "Abort",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
            558, 160, 190, 30, hwnd, (HMENU)ID_BTN_ABORT, hi, NULL);

        // Progress bar
        g_progress = CreateWindowExA(0, PROGRESS_CLASSA, "",
            WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
            8, 216, 740, 22, hwnd, (HMENU)ID_PROGRESS, hi, NULL);
        SendMessage(g_progress, PBM_SETRANGE, 0, MAKELPARAM(0, 1000));

        // Status label
        g_status_txt = CreateWindowA("STATIC", "Idle",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            8, 242, 740, 20, hwnd, (HMENU)ID_STATUS_TEXT, hi, NULL);

        // Log output (multiline read-only edit)
        g_log_txt = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            8, 266, 740, 240, hwnd, (HMENU)ID_LOG_TEXT, hi, NULL);
        SendMessage(g_log_txt, EM_SETLIMITTEXT, 65536, 0);

        // Set monospace font for log
        HFONT hfont = CreateFontA(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
        SendMessage(g_log_txt, WM_SETFONT, (WPARAM)hfont, TRUE);

        // Load game list
        RefreshGameList();

        // Timer for UI refresh (every 200ms)
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

        if (ctrl_id == ID_BTN_UPDATE) {
            // Validate steamcmd.exe exists
            if (GetFileAttributesA(g_cfg.steamcmd_path) == INVALID_FILE_ATTRIBUTES) {
                MessageBoxA(hwnd, "steamcmd.exe not found. Check Settings.",
                            "Configuration Error", MB_OK | MB_ICONERROR);
                return 0;
            }
            int sel = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
            if (sel >= 0 && sel < g_game_count) {
                StartUpdate(sel);
            } else {
                MessageBoxA(hwnd, "Please select a game first.", "No selection", MB_OK | MB_ICONINFORMATION);
            }
        }
        else if (ctrl_id == ID_BTN_UPDATE_ALL) {
            MessageBoxA(hwnd,
                "Update All will queue all games sequentially.\nNot yet implemented - coming soon.",
                "Update All", MB_OK | MB_ICONINFORMATION);
        }
        else if (ctrl_id == ID_BTN_ABORT) {
            worker_request_abort();
            EnableWindow(g_btn_abort, FALSE);
            SetWindowTextA(g_status_txt, "Aborting...");
        }
        else if (ctrl_id == ID_BTN_SETTINGS) {
            ShowSettingsDialog(hwnd);
        }
        else if (ctrl_id == ID_BTN_SETTINGS + 10) { // Refresh
            RefreshGameList();
        }
        break;
    }

    case WM_SIZE: {
        int w = LOWORD(lp), h = HIWORD(lp);
        if (w > 0 && h > 0) {
            MoveWindow(g_list,       8,   8,   w-220, 200, TRUE);
            MoveWindow(g_btn_update, w-205, 8,  195, 30, TRUE);
            MoveWindow(g_progress,   8, 216, w-16, 22, TRUE);
            MoveWindow(g_status_txt, 8, 242, w-16, 20, TRUE);
            MoveWindow(g_log_txt,    8, 266, w-16, h-274, TRUE);
        }
        break;
    }

    case WM_DESTROY:
        // Если обновление идёт — прерываем и ждём завершения потока
        if (g_worker) {
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
// RefreshGameList
// ---------------------------------------------------------------------------
static void RefreshGameList(void) {
    g_game_count = acf_scan_libraries(g_games, 128);
    ListView_DeleteAllItems(g_list);

    for (int i = 0; i < g_game_count; i++) {
        LVITEMA item = {0};
        item.mask    = LVIF_TEXT;
        item.iItem   = i;
        item.pszText = g_games[i].appid;
        ListView_InsertItem(g_list, &item);
        ListView_SetItemText(g_list, i, 1, g_games[i].name);

        // Size in GB
        char size_str[32];
        double gb = atof(g_games[i].size_on_disk) / (1024.0*1024.0*1024.0);
        snprintf(size_str, sizeof(size_str), "%.2f GB", gb);
        ListView_SetItemText(g_list, i, 2, size_str);

        ListView_SetItemText(g_list, i, 3, g_games[i].buildid);
        ListView_SetItemText(g_list, i, 4, g_games[i].state_flags);
    }

    char buf[64];
    snprintf(buf, sizeof(buf), "Found %d games.", g_game_count);
    AppendLog(buf);
}

// ---------------------------------------------------------------------------
// StartUpdate
// ---------------------------------------------------------------------------
static void StartUpdate(int game_idx) {
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

    // Reset status (do NOT memset over the live CRITICAL_SECTION)
    EnterCriticalSection(&g_wstatus.lock);
    g_wstatus.state = WORKER_IDLE;
    g_wstatus.progress = 0.0;
    g_wstatus.state_desc[0] = '\0';
    g_wstatus.last_log_line[0] = '\0';
    g_wstatus.error_msg[0] = '\0';
    g_wstatus.build_id_before[0] = '\0';
    g_wstatus.build_id_after[0] = '\0';
    LeaveCriticalSection(&g_wstatus.lock);

    // Build worker config
    WorkerConfig cfg = {0};
    strncpy(cfg.server_url,    g_cfg.server_url,    sizeof(cfg.server_url)-1);
    strncpy(cfg.api_key,       g_cfg.api_key,       sizeof(cfg.api_key)-1);
    strncpy(cfg.pc_id,         g_cfg.pc_id,         sizeof(cfg.pc_id)-1);
    strncpy(cfg.steamcmd_path, g_cfg.steamcmd_path, sizeof(cfg.steamcmd_path)-1);
    strncpy(cfg.app_id,        g_games[game_idx].appid, sizeof(cfg.app_id)-1);
    cfg.jitter_disabled = 1;  // GUI-triggered: no jitter (admin is watching)

    char buf[256];
    snprintf(buf, sizeof(buf), "Starting update for %s (App %s)...",
             g_games[game_idx].name, g_games[game_idx].appid);
    AppendLog(buf);

    // Disable Update button, enable Abort
    EnableWindow(g_btn_update, FALSE);
    EnableWindow(g_btn_abort, TRUE);

    g_worker = worker_start(&cfg, &g_wstatus);
}

// ---------------------------------------------------------------------------
// UpdateUIFromWorker (called every 200ms from timer)
// ---------------------------------------------------------------------------
static void UpdateUIFromWorker(void) {
    EnterCriticalSection(&g_wstatus.lock);
    WorkerState state  = g_wstatus.state;
    double      pct    = g_wstatus.progress;
    char        desc[64] = {0}, logline[512] = {0}, errmsg[256] = {0};
    strncpy(desc,    g_wstatus.state_desc,   63);   desc[63] = '\0';
    strncpy(logline, g_wstatus.last_log_line, 511); logline[511] = '\0';
    // Trim a UTF-8 sequence that was cut at the 511-byte boundary
    {
        int n = (int)strlen(logline);
        while (n > 0 && ((unsigned char)logline[n - 1] & 0xC0) == 0x80) n--;
        if (n > 0 && ((unsigned char)logline[n - 1] & 0x80)) n--;
        logline[n] = '\0';
    }
    strncpy(errmsg,  g_wstatus.error_msg,    255);  errmsg[255] = '\0';
    LeaveCriticalSection(&g_wstatus.lock);

    // Progress bar
    SendMessage(g_progress, PBM_SETPOS, (WPARAM)(pct * 10.0), 0);

    // Status text
    char status_buf[256];
    if (desc[0])
        snprintf(status_buf, sizeof(status_buf), "%s - %.1f%%  (%s)",
                 state_name(state), pct, desc);
    else
        snprintf(status_buf, sizeof(status_buf), "%s", state_name(state));
    SetWindowTextA(g_status_txt, status_buf);

    // Append new log line if changed
    static char last_log[512] = {0};
    if (logline[0] && strcmp(logline, last_log) != 0) {
        strncpy(last_log, logline, 511);
        last_log[511] = '\0';
        AppendLog(logline);
    }

    // Done
    if (state == WORKER_DONE_OK || state == WORKER_DONE_FAIL) {
        HANDLE hWorker = g_worker;
        g_worker = NULL; // Prevent re-entrant timer calls during MessageBox loop

        EnableWindow(g_btn_update, TRUE);
        EnableWindow(g_btn_abort, FALSE);

        if (state == WORKER_DONE_OK) {
            AppendLog(">>> Update completed successfully!");
            MessageBoxA(g_hwnd, "Update completed successfully!", "Done", MB_OK | MB_ICONINFORMATION);
        } else {
            char fail_msg[512];
            snprintf(fail_msg, sizeof(fail_msg), "Update failed: %s", errmsg[0] ? errmsg : "Unknown error");
            AppendLog(fail_msg);
            MessageBoxA(g_hwnd, fail_msg, "Update Failed", MB_OK | MB_ICONERROR);
        }

        // Refresh game list to show new buildid
        RefreshGameList();

        if (hWorker) {
            CloseHandle(hWorker);
        }
    }
}

// ---------------------------------------------------------------------------
// AppendLog
// ---------------------------------------------------------------------------
static void AppendLog(const char *line) {
    // SteamCMD stdout is UTF-8; convert to UTF-16 and use the wide edit-control API.
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

        // Labels + Edit boxes
        CreateWindowA("STATIC", "Server URL:",
            WS_CHILD|WS_VISIBLE, 10, 10, 100, 20, hwnd, NULL, NULL, NULL);
        HWND eUrl = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", cfg->server_url,
            WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
            10, 32, 380, 22, hwnd, (HMENU)ID_EDIT_URL, NULL, NULL);

        CreateWindowA("STATIC", "API Key:",
            WS_CHILD|WS_VISIBLE, 10, 62, 100, 20, hwnd, NULL, NULL, NULL);
        HWND eKey = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", cfg->api_key,
            WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
            10, 84, 380, 22, hwnd, (HMENU)ID_EDIT_KEY, NULL, NULL);

        CreateWindowA("STATIC", "steamcmd.exe path:",
            WS_CHILD|WS_VISIBLE, 10, 114, 150, 20, hwnd, NULL, NULL, NULL);
        HWND eCmd = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", cfg->steamcmd_path,
            WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
            10, 136, 300, 22, hwnd, (HMENU)ID_EDIT_CMD, NULL, NULL);
        CreateWindowA("BUTTON", "...",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            318, 136, 72, 22, hwnd, (HMENU)ID_BTN_BROWSE, NULL, NULL);

        CreateWindowA("STATIC", "PC ID:",
            WS_CHILD|WS_VISIBLE, 10, 166, 100, 20, hwnd, NULL, NULL, NULL);
        HWND ePcid = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", cfg->pc_id,
            WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
            10, 188, 200, 22, hwnd, (HMENU)ID_EDIT_PCID, NULL, NULL);

        CreateWindowA("BUTTON", "Save",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_DEFPUSHBUTTON,
            310, 218, 80, 28, hwnd, (HMENU)ID_BTN_SAVE, NULL, NULL);

        (void)eUrl; (void)eKey; (void)eCmd; (void)ePcid;
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
            // Open file dialog to pick steamcmd.exe
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
