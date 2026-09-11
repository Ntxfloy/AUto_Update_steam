// main.c - Steam Auto-Updater client, Win32 GUI entry point
// Native Win32 UI: no external frameworks, single .exe
// v5: flicker-free painting (no WS_EX_COMPOSITED, clipped children,
//     WM_ERASEBKGND swallowed), Select all / Deselect all / Select all
//     except PAID, Steam-like progress (bytes/speed/ETA/stall), single
//     instance, sleep + shutdown guards, persistent file log.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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
#define ID_TITLE_TEXT      111
#define ID_BTN_CHECK_ALL   112
#define ID_BTN_UNCHECK_ALL 113
#define ID_BTN_CHECK_F2P   114
#define ID_TIMER_REFRESH   1001

#define MAX_ROWS 128

// A download that reports no new bytes for this long is almost certainly wedged
// (dead LAN link, Steam CDN hiccup, antivirus holding the file).
#define STALL_WARN_MS   120000

// ---------------------------------------------------------------------------
// Dark palette
// ---------------------------------------------------------------------------
#define CLR_BG            RGB(24,  24,  27)
#define CLR_PANEL         RGB(32,  32,  38)
#define CLR_PANEL_ALT     RGB(40,  40,  47)
#define CLR_BORDER        RGB(58,  58,  66)
#define CLR_TEXT          RGB(233, 233, 238)
#define CLR_TEXT_DIM      RGB(148, 148, 160)
#define CLR_ACCENT        RGB(56,  120, 220)
#define CLR_ACCENT_HOVER  RGB(78,  146, 246)
#define CLR_ACCENT_DOWN   RGB(40,  96,  184)
#define CLR_OK            RGB(64,  192, 112)
#define CLR_FAIL          RGB(238, 92,  92)
#define CLR_WARN          RGB(230, 176, 64)

// Undocumented-but-stable DWM attribute for a dark title bar.
// Loaded dynamically so the exe still starts on Windows 7/8.
#define DWMWA_USE_IMMERSIVE_DARK_MODE_OLD 19
#define DWMWA_USE_IMMERSIVE_DARK_MODE     20

static HBRUSH g_br_bg        = NULL;
static HBRUSH g_br_panel     = NULL;
static HBRUSH g_br_panel_alt = NULL;
static HFONT  g_font_ui      = NULL;
static HFONT  g_font_bold    = NULL;
static HFONT  g_font_mono    = NULL;

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
    int  result;      // 0 = none, 1 = ok, 2 = failed, 3 = in progress
} GameRow;

static HWND          g_hwnd            = NULL;
static HWND          g_list            = NULL;
static HWND          g_title_txt       = NULL;
static HWND          g_btn_update      = NULL;
static HWND          g_btn_update_all  = NULL;
static HWND          g_btn_refresh     = NULL;
static HWND          g_btn_settings    = NULL;
static HWND          g_btn_abort       = NULL;
static HWND          g_btn_check_all   = NULL;
static HWND          g_btn_uncheck_all = NULL;
static HWND          g_btn_check_f2p   = NULL;
static HWND          g_progress        = NULL;
static HWND          g_status_txt      = NULL;
static HWND          g_queue_txt       = NULL;
static HWND          g_log_txt         = NULL;

static Config        g_cfg                 = {0};
static char          g_cfg_path[MAX_PATH]  = {0};
static char          g_logfile[MAX_PATH]   = {0};
static GameRow       g_rows[MAX_ROWS]      = {0};
static int           g_row_count           = 0;

static HANDLE        g_worker      = NULL;
static HANDLE        g_single_inst = NULL;
static WorkerStatus  g_wstatus     = {0};

// Sequential queue of row indices
static int           g_queue[MAX_ROWS] = {0};
static int           g_queue_len       = 0;
static int           g_queue_pos       = 0;
static int           g_current_row     = -1;
static int           g_ok_count        = 0;
static int           g_fail_count      = 0;
static int           g_aborting        = 0;

// --- Progress engine state (Steam-like) ------------------------------------
static double        g_pct             = 0.0;   // what we actually draw
static char          g_pct_label[192]  = {0};
static int           g_pct_failed      = 0;
static int           g_pct_warn        = 0;

static unsigned long long g_bytes_done  = 0;
static unsigned long long g_bytes_total = 0;
static double        g_speed_bps       = 0.0;   // smoothed bytes/sec
static ULONGLONG     g_tick_bytes      = 0;     // last time bytes changed
static ULONGLONG     g_tick_sample     = 0;     // last speed sample
static unsigned long long g_bytes_sample = 0;
static ULONGLONG     g_tick_job_start  = 0;
static char          g_stage[32]       = {0};
static double        g_stage_floor     = 0.0;   // monotonic guard

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
static void      SetControlsBusy(int busy);
static void      DrawProgress(double pct, const char *label, int failed, int warn);
static void      ProgressReset(void);
static void      ProgressUpdate(WorkerState state, double raw_pct,
                                const char *desc, const char *logline);

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
// Small formatting helpers
// ---------------------------------------------------------------------------
static void fmt_bytes(unsigned long long b, char *out, size_t n) {
    double v = (double)b;
    if (v >= 1024.0*1024.0*1024.0) snprintf(out, n, "%.2f GB", v/(1024.0*1024.0*1024.0));
    else if (v >= 1024.0*1024.0)   snprintf(out, n, "%.0f MB", v/(1024.0*1024.0));
    else if (v >= 1024.0)          snprintf(out, n, "%.0f KB", v/1024.0);
    else                           snprintf(out, n, "%llu B", b);
}

static void fmt_secs(double s, char *out, size_t n) {
    if (s < 0 || s > 86400.0*7) { snprintf(out, n, "--"); return; }
    int t = (int)(s + 0.5);
    if (t < 60)        snprintf(out, n, "%d sec", t);
    else if (t < 3600) snprintf(out, n, "%d min %02d sec", t/60, t%60);
    else               snprintf(out, n, "%d h %02d min", t/3600, (t%3600)/60);
}

// ---------------------------------------------------------------------------
// Dark theming helpers
// ---------------------------------------------------------------------------
static void enable_dark_titlebar(HWND hwnd) {
    HMODULE dwm = LoadLibraryA("dwmapi.dll");
    if (!dwm) return;
    typedef HRESULT (WINAPI *SetAttrFn)(HWND, DWORD, LPCVOID, DWORD);
    SetAttrFn set_attr = (SetAttrFn)GetProcAddress(dwm, "DwmSetWindowAttribute");
    if (set_attr) {
        BOOL on = TRUE;
        if (FAILED(set_attr(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &on, sizeof(on))))
            set_attr(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE_OLD, &on, sizeof(on));
    }
    FreeLibrary(dwm);
}

static void fill_round(HDC dc, RECT rc, COLORREF fill, COLORREF border, int radius) {
    HBRUSH br = CreateSolidBrush(fill);
    HPEN   pn = CreatePen(PS_SOLID, 1, border);
    HBRUSH ob = (HBRUSH)SelectObject(dc, br);
    HPEN   op = (HPEN)SelectObject(dc, pn);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pn);
}

// --- Owner-drawn buttons: hover tracking via a tiny subclass ---------------
static WNDPROC g_btn_oldproc = NULL;

static LRESULT CALLBACK BtnSubProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND:
        // The button paints its whole rect in WM_DRAWITEM. Letting Windows
        // erase it first is exactly what made the UI blink.
        return 1;
    case WM_MOUSEMOVE:
        if (!GetWindowLongPtrA(h, GWLP_USERDATA)) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
            SetWindowLongPtrA(h, GWLP_USERDATA, 1);
            TrackMouseEvent(&tme);
            InvalidateRect(h, NULL, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        SetWindowLongPtrA(h, GWLP_USERDATA, 0);
        InvalidateRect(h, NULL, FALSE);
        break;
    }
    return CallWindowProcA(g_btn_oldproc, h, m, w, l);
}

static HWND make_button(HWND parent, const char *text, int id,
                        int x, int y, int cx, int cy, int disabled) {
    HWND b = CreateWindowA("BUTTON", text,
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_OWNERDRAW | (disabled ? WS_DISABLED : 0),
        x, y, cx, cy, parent, (HMENU)(LONG_PTR)id, GetModuleHandle(NULL), NULL);
    SendMessage(b, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
    WNDPROC prev = (WNDPROC)SetWindowLongPtrA(b, GWLP_WNDPROC, (LONG_PTR)BtnSubProc);
    if (!g_btn_oldproc) g_btn_oldproc = prev;
    return b;
}

// --- Owner-drawn ListView header -------------------------------------------
static WNDPROC g_hdr_oldproc = NULL;

static LRESULT CALLBACK HeaderSubProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HBRUSH br = CreateSolidBrush(CLR_PANEL_ALT);
        FillRect(dc, &rc, br);
        DeleteObject(br);

        HFONT of = (HFONT)SelectObject(dc, g_font_ui);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, CLR_TEXT_DIM);

        int n = (int)SendMessageA(h, HDM_GETITEMCOUNT, 0, 0);
        for (int i = 0; i < n; i++) {
            RECT ir;
            if (!SendMessageA(h, HDM_GETITEMRECT, (WPARAM)i, (LPARAM)&ir)) continue;
            char txt[64] = {0};
            HDITEMA it = {0};
            it.mask = HDI_TEXT;
            it.pszText = txt;
            it.cchTextMax = sizeof(txt) - 1;
            SendMessageA(h, HDM_GETITEMA, (WPARAM)i, (LPARAM)&it);

            RECT tr = ir;
            tr.left += 10;
            DrawTextA(dc, txt, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            HPEN pn = CreatePen(PS_SOLID, 1, CLR_BORDER);
            HPEN op = (HPEN)SelectObject(dc, pn);
            MoveToEx(dc, ir.right - 1, ir.top + 5, NULL);
            LineTo(dc, ir.right - 1, ir.bottom - 5);
            SelectObject(dc, op);
            DeleteObject(pn);
        }

        // bottom separator
        HPEN pn = CreatePen(PS_SOLID, 1, CLR_BORDER);
        HPEN op = (HPEN)SelectObject(dc, pn);
        MoveToEx(dc, rc.left, rc.bottom - 1, NULL);
        LineTo(dc, rc.right, rc.bottom - 1);
        SelectObject(dc, op);
        DeleteObject(pn);

        SelectObject(dc, of);
        EndPaint(h, &ps);
        return 0;
    }
    return CallWindowProcA(g_hdr_oldproc, h, m, w, l);
}

// ---------------------------------------------------------------------------
// Keep the machine awake while a queue is running.
// A 40 GB install on a club PC with a 15-minute sleep timer used to die
// halfway through and leave the account leased.
// ---------------------------------------------------------------------------
static void keep_awake(int on) {
    if (on) SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_AWAYMODE_REQUIRED);
    else    SetThreadExecutionState(ES_CONTINUOUS);
}

// ---------------------------------------------------------------------------
// WinMain
// ---------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
    (void)hPrev; (void)lpCmd;

    // --- single instance ---------------------------------------------------
    // Two copies at once would fight over the same lease.json and could kill
    // each other's steamcmd. Focus the running window instead.
    g_single_inst = CreateMutexA(NULL, TRUE, "Global\\SteamAutoUpdater_SingleInstance");
    if (g_single_inst && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND prev_win = FindWindowA("SteamAutoUpdater", NULL);
        if (prev_win) {
            ShowWindow(prev_win, SW_RESTORE);
            SetForegroundWindow(prev_win);
        } else {
            MessageBoxA(NULL, "Steam Auto-Updater is already running.",
                        "Already running", MB_OK | MB_ICONINFORMATION);
        }
        return 0;
    }

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);

    g_br_bg        = CreateSolidBrush(CLR_BG);
    g_br_panel     = CreateSolidBrush(CLR_PANEL);
    g_br_panel_alt = CreateSolidBrush(CLR_PANEL_ALT);

    g_font_ui = CreateFontA(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS, "Segoe UI");
    g_font_bold = CreateFontA(-19, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS, "Segoe UI");
    g_font_mono = CreateFontA(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");

    // config + log file live next to the exe
    GetModuleFileNameA(NULL, g_cfg_path, MAX_PATH);
    char *last_slash = strrchr(g_cfg_path, '\\');
    if (last_slash) *(last_slash + 1) = '\0';
    strncpy(g_logfile, g_cfg_path, MAX_PATH - 1);
    strncat(g_logfile,  "updater.log", MAX_PATH - strlen(g_logfile) - 1);
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
    // No class background brush: we paint the whole client area in WM_PAINT.
    // Letting Windows erase with a brush first is a guaranteed flash.
    wc.hbrBackground = NULL;
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    // WS_EX_COMPOSITED (an XP-era double-buffering hack) fights with DWM on
    // Windows 10/11 and with the ListView's own double buffering: the window
    // repainted in a loop and the whole UI strobed. WS_CLIPCHILDREN instead.
    g_hwnd = CreateWindowExA(WS_EX_APPWINDOW, "SteamAutoUpdater",
                              "Steam Auto-Updater v5.0 (F2P)",
                              WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                              CW_USEDEFAULT, CW_USEDEFAULT, 920, 760,
                              NULL, NULL, hInst, NULL);
    enable_dark_titlebar(g_hwnd);
    ShowWindow(g_hwnd, nShow);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    keep_awake(0);
    DeleteCriticalSection(&g_wstatus.lock);
    if (g_single_inst) { ReleaseMutex(g_single_inst); CloseHandle(g_single_inst); }
    return (int)msg.wParam;
}

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
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CREATE: {
        HINSTANCE hi = GetModuleHandle(NULL);

        g_title_txt = CreateWindowA("STATIC", "Steam Auto-Updater",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | SS_LEFT,
            16, 12, 400, 26, hwnd, (HMENU)ID_TITLE_TEXT, hi, NULL);
        SendMessage(g_title_txt, WM_SETFONT, (WPARAM)g_font_bold, TRUE);

        g_list = CreateWindowExA(0, WC_LISTVIEWA, "",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | LVS_REPORT | LVS_SINGLESEL
            | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
            16, 46, 600, 300, hwnd, (HMENU)ID_LIST_GAMES, hi, NULL);
        ListView_SetExtendedListViewStyle(g_list,
            LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        SendMessage(g_list, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
        ListView_SetBkColor(g_list, CLR_PANEL);
        ListView_SetTextBkColor(g_list, CLR_PANEL);
        ListView_SetTextColor(g_list, CLR_TEXT);

        HWND hdr = ListView_GetHeader(g_list);
        if (hdr) {
            SendMessage(hdr, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
            g_hdr_oldproc = (WNDPROC)SetWindowLongPtrA(hdr, GWLP_WNDPROC, (LONG_PTR)HeaderSubProc);
        }

        LVCOLUMNA col = {0};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx = 70;  col.pszText = (LPSTR)"AppID";   ListView_InsertColumn(g_list, 0, &col);
        col.cx = 220; col.pszText = (LPSTR)"Game";    ListView_InsertColumn(g_list, 1, &col);
        col.cx = 85;  col.pszText = (LPSTR)"Size";    ListView_InsertColumn(g_list, 2, &col);
        col.cx = 90;  col.pszText = (LPSTR)"BuildID"; ListView_InsertColumn(g_list, 3, &col);
        col.cx = 210; col.pszText = (LPSTR)"Status";  ListView_InsertColumn(g_list, 4, &col);

        g_btn_update_all  = make_button(hwnd, "Update ALL checked",    ID_BTN_UPDATE_ALL,  630, 46,  240, 40, 0);
        g_btn_update      = make_button(hwnd, "Update selected",       ID_BTN_UPDATE,      630, 94,  240, 32, 0);
        g_btn_check_all   = make_button(hwnd, "Select all",            ID_BTN_CHECK_ALL,   630, 134, 240, 30, 0);
        g_btn_uncheck_all = make_button(hwnd, "Deselect all",          ID_BTN_UNCHECK_ALL, 630, 168, 240, 30, 0);
        g_btn_check_f2p   = make_button(hwnd, "Select all except PAID",ID_BTN_CHECK_F2P,   630, 202, 240, 30, 0);
        g_btn_refresh     = make_button(hwnd, "Refresh list",          ID_BTN_REFRESH,     630, 242, 240, 30, 0);
        g_btn_settings    = make_button(hwnd, "Settings",              ID_BTN_SETTINGS,    630, 276, 240, 30, 0);
        g_btn_abort       = make_button(hwnd, "Abort",                 ID_BTN_ABORT,       630, 314, 240, 32, 1);

        // Our own progress bar: a static we paint ourselves, so it can be dark
        // and can show percentage, stage, bytes, speed and ETA inside the bar.
        g_progress = CreateWindowA("STATIC", "",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | SS_OWNERDRAW,
            16, 360, 854, 30, hwnd, (HMENU)ID_PROGRESS, hi, NULL);

        g_status_txt = CreateWindowA("STATIC", "Idle",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | SS_LEFT | SS_ENDELLIPSIS,
            16, 398, 854, 20, hwnd, (HMENU)ID_STATUS_TEXT, hi, NULL);
        SendMessage(g_status_txt, WM_SETFONT, (WPARAM)g_font_ui, TRUE);

        g_queue_txt = CreateWindowA("STATIC", "Queue: empty",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | SS_LEFT | SS_ENDELLIPSIS,
            16, 420, 854, 20, hwnd, (HMENU)ID_QUEUE_TEXT, hi, NULL);
        SendMessage(g_queue_txt, WM_SETFONT, (WPARAM)g_font_ui, TRUE);

        g_log_txt = CreateWindowExA(0, "EDIT", "",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_VSCROLL
            | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            16, 446, 854, 250, hwnd, (HMENU)ID_LOG_TEXT, hi, NULL);
        SendMessage(g_log_txt, EM_SETLIMITTEXT, 262144, 0);
        SendMessage(g_log_txt, WM_SETFONT, (WPARAM)g_font_mono, TRUE);

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

        SetTimer(hwnd, ID_TIMER_REFRESH, 100, NULL);
        break;
    }

    // --- dark colours for the standard controls --------------------------
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        HWND ctl = (HWND)lp;
        SetBkMode(dc, TRANSPARENT);
        if (ctl == g_log_txt) {
            SetTextColor(dc, CLR_TEXT_DIM);
            SetBkColor(dc, CLR_PANEL);
            return (LRESULT)g_br_panel;
        }
        if (ctl == g_title_txt)  SetTextColor(dc, CLR_TEXT);
        else if (ctl == g_queue_txt) SetTextColor(dc, CLR_TEXT_DIM);
        else                         SetTextColor(dc, CLR_TEXT);
        SetBkColor(dc, CLR_BG);
        return (LRESULT)g_br_bg;
    }

    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, CLR_TEXT);
        SetBkColor(dc, CLR_PANEL_ALT);
        return (LRESULT)g_br_panel_alt;
    }

    // --- owner-drawn buttons + progress bar ------------------------------
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *di = (DRAWITEMSTRUCT *)lp;

        if (di->CtlID == ID_PROGRESS) {
            RECT rc = di->rcItem;
            fill_round(di->hDC, rc, CLR_PANEL, CLR_BORDER, 8);

            double p = g_pct;
            if (p < 0.0)   p = 0.0;
            if (p > 100.0) p = 100.0;
            int track = rc.right - rc.left - 4;
            int w = (int)(track * (p / 100.0));
            if (w > 2) {
                RECT fr = { rc.left + 2, rc.top + 2, rc.left + 2 + w, rc.bottom - 2 };
                COLORREF c = g_pct_failed ? CLR_FAIL
                           : g_pct_warn   ? CLR_WARN
                           : (p >= 99.999 ? CLR_OK : CLR_ACCENT);
                fill_round(di->hDC, fr, c, c, 7);
            }

            SetBkMode(di->hDC, TRANSPARENT);
            SetTextColor(di->hDC, CLR_TEXT);
            HFONT of = (HFONT)SelectObject(di->hDC, g_font_ui);
            DrawTextA(di->hDC, g_pct_label, -1, &rc,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(di->hDC, of);
            return TRUE;
        }

        if (di->CtlType == ODT_BUTTON) {
            int hovered  = (int)GetWindowLongPtrA(di->hwndItem, GWLP_USERDATA);
            int pressed  = (di->itemState & ODS_SELECTED) ? 1 : 0;
            int disabled = (di->itemState & ODS_DISABLED) ? 1 : 0;
            int primary  = (di->CtlID == ID_BTN_UPDATE_ALL);

            COLORREF fill, border, text;
            if (disabled) {
                fill = CLR_PANEL; border = CLR_BORDER; text = RGB(110, 110, 120);
            } else if (primary) {
                fill   = pressed ? CLR_ACCENT_DOWN : (hovered ? CLR_ACCENT_HOVER : CLR_ACCENT);
                border = fill;
                text   = RGB(255, 255, 255);
            } else {
                fill   = pressed ? CLR_BG : (hovered ? CLR_PANEL_ALT : CLR_PANEL);
                border = hovered ? CLR_ACCENT : CLR_BORDER;
                text   = CLR_TEXT;
            }
            if (di->CtlID == ID_BTN_ABORT && !disabled) {
                fill   = pressed ? RGB(170, 60, 60) : (hovered ? RGB(214, 78, 78) : CLR_PANEL);
                border = hovered || pressed ? RGB(214, 78, 78) : CLR_BORDER;
                text   = hovered || pressed ? RGB(255, 255, 255) : CLR_FAIL;
            }

            fill_round(di->hDC, di->rcItem, fill, border, 8);

            char txt[128] = {0};
            GetWindowTextA(di->hwndItem, txt, sizeof(txt) - 1);
            SetBkMode(di->hDC, TRANSPARENT);
            SetTextColor(di->hDC, text);
            HFONT of = (HFONT)SelectObject(di->hDC, g_font_ui);
            DrawTextA(di->hDC, txt, -1, &di->rcItem,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(di->hDC, of);
            return TRUE;
        }
        break;
    }

    // --- list view: custom colours + double-click to update one game -----
    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)lp;

        if (nh->idFrom == ID_LIST_GAMES && nh->code == NM_DBLCLK) {
            if (!g_worker) SendMessage(hwnd, WM_COMMAND, ID_BTN_UPDATE, 0);
            return 0;
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
                cd->clrTextBk = (row % 2) ? CLR_PANEL_ALT : CLR_PANEL;
                if (cd->iSubItem == 4 && row >= 0 && row < g_row_count) {
                    switch (g_rows[row].result) {
                    case 1:  cd->clrText = CLR_OK;   break;
                    case 2:  cd->clrText = CLR_FAIL; break;
                    case 3:  cd->clrText = CLR_WARN; break;
                    default: cd->clrText = g_rows[row].installed ? CLR_TEXT : CLR_TEXT_DIM;
                    }
                } else {
                    cd->clrText = (row >= 0 && row < g_row_count && !g_rows[row].installed)
                                  ? CLR_TEXT_DIM : CLR_TEXT;
                }
                return CDRF_NEWFONT;
            }
            }
        }
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
                // Paid titles cannot be updated by a pool account: a service
                // account simply does not own them. Say so before wasting a
                // lease and a login attempt on every one of them.
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
            StartQueue();
        }
        else if (ctrl_id == ID_BTN_CHECK_ALL) {
            for (int i = 0; i < g_row_count; i++)
                ListView_SetCheckState(g_list, i, TRUE);
            AppendLog("All rows checked.");
        }
        else if (ctrl_id == ID_BTN_UNCHECK_ALL) {
            for (int i = 0; i < g_row_count; i++)
                ListView_SetCheckState(g_list, i, FALSE);
            AppendLog("All rows unchecked.");
        }
        else if (ctrl_id == ID_BTN_CHECK_F2P) {
            int n = 0;
            for (int i = 0; i < g_row_count; i++) {
                int want = g_rows[i].is_f2p ? 1 : 0;
                ListView_SetCheckState(g_list, i, want ? TRUE : FALSE);
                n += want;
            }
            char b[96];
            snprintf(b, sizeof(b), "Checked %d free-to-play rows, paid titles skipped.", n);
            AppendLog(b);
        }
        else if (ctrl_id == ID_BTN_ABORT) {
            if (MessageBoxA(hwnd,
                    "Stop after the current game?\n\n"
                    "The running steamcmd will be terminated and the account released. "
                    "A partially downloaded game stays partial and resumes next time.",
                    "Abort", MB_YESNO | MB_ICONQUESTION) != IDYES)
                return 0;
            g_aborting = 1;
            worker_request_abort();
            g_queue_len = g_queue_pos;   // stop after the current game
            EnableWindow(g_btn_abort, FALSE);
            SetWindowTextA(g_status_txt, "Aborting...");
            AppendLog(">>> Abort requested by the operator.");
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

    case WM_SIZE: {
        int w = LOWORD(lp), h = HIWORD(lp);
        if (w > 0 && h > 0) {
            int right_w = 240, gap = 14;
            int list_w  = w - right_w - gap - 32;
            if (list_w < 200) list_w = 200;
            int bx = 16 + list_w + gap;

            MoveWindow(g_title_txt,       16, 12, list_w, 26, TRUE);
            MoveWindow(g_list,            16, 46, list_w, 300, TRUE);
            MoveWindow(g_btn_update_all,  bx, 46,  right_w, 40, TRUE);
            MoveWindow(g_btn_update,      bx, 94,  right_w, 32, TRUE);
            MoveWindow(g_btn_check_all,   bx, 134, right_w, 30, TRUE);
            MoveWindow(g_btn_uncheck_all, bx, 168, right_w, 30, TRUE);
            MoveWindow(g_btn_check_f2p,   bx, 202, right_w, 30, TRUE);
            MoveWindow(g_btn_refresh,     bx, 242, right_w, 30, TRUE);
            MoveWindow(g_btn_settings,    bx, 276, right_w, 30, TRUE);
            MoveWindow(g_btn_abort,       bx, 314, right_w, 32, TRUE);
            MoveWindow(g_progress,   16, 360, w-32, 30, TRUE);
            MoveWindow(g_status_txt, 16, 398, w-32, 20, TRUE);
            MoveWindow(g_queue_txt,  16, 420, w-32, 20, TRUE);
            MoveWindow(g_log_txt,    16, 446, w-32, h > 480 ? h-462 : 40, TRUE);
        }
        break;
    }

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        mmi->ptMinTrackSize.x = 820;
        mmi->ptMinTrackSize.y = 660;
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
        PostQuitMessage(0);
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Progress engine
// ---------------------------------------------------------------------------
static void DrawProgress(double pct, const char *label, int failed, int warn) {
    // Repaint only when something visibly changed: a 100 ms timer that
    // invalidates the bar unconditionally is its own source of flicker.
    char newlabel[192];
    if (label && label[0])
        snprintf(newlabel, sizeof(newlabel), "%.1f%%   -   %s", pct, label);
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

    if (changed && g_progress) InvalidateRect(g_progress, NULL, FALSE);
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

    if (strstr(stage, "prealloc"))    return 0.0  + raw * 0.02;   //  0 -  2 %
    if (strstr(stage, "download"))    return 2.0  + raw * 0.86;   //  2 - 88 %
    if (strstr(stage, "verif"))       return 88.0 + raw * 0.07;   // 88 - 95 %
    if (strstr(stage, "commit"))      return 95.0 + raw * 0.04;   // 95 - 99 %
    if (strstr(stage, "reconfig"))    return 99.0;
    if (strstr(stage, "validat"))     return 88.0 + raw * 0.07;
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

    // Smoothed speed: EMA over ~1.5 second samples, so the number does not
    // jitter between 3 MB/s and 90 MB/s twice a second.
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

    // Build the Steam-style label
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
        snprintf(label, sizeof(label), "%s   -   no new data for %llu s (network? antivirus?)",
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

    SendMessage(g_list, WM_SETREDRAW, FALSE, 0);
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
        // Default selection = installed free-to-play games. Paid rows stay
        // unchecked: a pool account cannot update them anyway.
        ListView_SetCheckState(g_list, i,
            (g_rows[i].installed && g_rows[i].is_f2p) ? TRUE : FALSE);
    }
    SendMessage(g_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_list, NULL, FALSE);

    char buf[160];
    snprintf(buf, sizeof(buf), "List refreshed: %d rows (%d installed on this PC).",
             g_row_count, n_inst);
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
// Queue handling
// ---------------------------------------------------------------------------
static void SetQueueText(void) {
    char buf[256];
    if (g_queue_len == 0) {
        snprintf(buf, sizeof(buf), "Queue: empty");
    } else {
        int cur = g_queue_pos > 0 ? g_queue_pos : 1;
        char el[32] = {0};
        fmt_secs((double)((GetTickCount64() - g_tick_job_start) / 1000), el, sizeof(el));
        snprintf(buf, sizeof(buf),
                 "Queue: %d / %d   |   OK: %d   Failed: %d   |   current game: %s",
                 cur, g_queue_len, g_ok_count, g_fail_count, el);
    }
    // Rewriting identical text 10 times per second makes the label flash.
    char old[256] = {0};
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
    g_rows[row_idx].result = 3;
    ListView_SetItemText(g_list, row_idx, 4, g_rows[row_idx].status);
    ListView_EnsureVisible(g_list, row_idx, FALSE);

    g_current_row = row_idx;
    ProgressReset();
    DrawProgress(0.0, g_rows[row_idx].installed ? "starting update" : "starting install", 0, 0);

    g_worker = worker_start(&cfg, &g_wstatus);
    if (!g_worker) {
        AppendLog("ERROR: failed to start the worker thread.");
        g_fail_count++;
        strncpy(g_rows[row_idx].status, "FAILED: thread", sizeof(g_rows[row_idx].status)-1);
        g_rows[row_idx].result = 2;
        ListView_SetItemText(g_list, row_idx, 4, g_rows[row_idx].status);
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

    static char last_log[512] = {0};
    if (logline[0] && strcmp(logline, last_log) != 0) {
        strncpy(last_log, logline, 511);
        last_log[511] = '\0';
        AppendLog(logline);
    }

    if (state != WORKER_DONE_OK && state != WORKER_DONE_FAIL) {
        ProgressUpdate(state, pct, desc, logline);

        char status_buf[380];
        const char *game = (g_current_row >= 0 && g_current_row < g_row_count)
                         ? g_rows[g_current_row].name : "";
        char el[32];
        fmt_secs((double)((GetTickCount64() - g_tick_job_start) / 1000), el, sizeof(el));
        snprintf(status_buf, sizeof(status_buf), "%s  -  %s  -  running for %s%s",
                 game, state_name(state), el, g_aborting ? "  (aborting)" : "");
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
            g_ok_count++;
            char st[64];
            EnterCriticalSection(&g_wstatus.lock);
            int same_build = (g_wstatus.build_id_before[0] && g_wstatus.build_id_after[0] &&
                              strcmp(g_wstatus.build_id_before, g_wstatus.build_id_after) == 0);
            strncpy(g_rows[g_current_row].buildid, g_wstatus.build_id_after,
                    sizeof(g_rows[g_current_row].buildid)-1);
            LeaveCriticalSection(&g_wstatus.lock);
            snprintf(st, sizeof(st), same_build ? "OK (already latest)" : "OK (updated)");
            strncpy(g_rows[g_current_row].status, st, sizeof(g_rows[g_current_row].status)-1);
            g_rows[g_current_row].installed = 1;
            g_rows[g_current_row].result    = 1;
            ListView_SetItemText(g_list, g_current_row, 3, g_rows[g_current_row].buildid);
            DrawProgress(100.0, "complete", 0, 0);
            AppendLog(">>> Done.");
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
        }
        ListView_SetItemText(g_list, g_current_row, 4, g_rows[g_current_row].status);
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
    snprintf(summary, sizeof(summary), "%s Success: %d, failed: %d.",
             g_aborting ? "Aborted." : "Finished.", g_ok_count, g_fail_count);
    SetWindowTextA(g_status_txt, summary);
    DrawProgress(100.0, summary, g_fail_count ? 1 : 0, 0);
    AppendLog(summary);

    // Refresh sizes / build ids so the list matches reality after the run.
    RefreshGameList();

    MessageBoxA(g_hwnd, summary, "Update finished",
                MB_OK | (g_fail_count ? MB_ICONWARNING : MB_ICONINFORMATION));
}

// ---------------------------------------------------------------------------
// AppendLog: to the on-screen log and to updater.log next to the exe.
// The file matters: on 50 machines nobody is watching the window, and the
// exact steamcmd line is the only way to tell a bad password from a dead disk.
// ---------------------------------------------------------------------------
static void AppendLog(const char *line) {
    if (!line) return;

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

    int wlen = MultiByteToWideChar(CP_UTF8, 0, line, -1, NULL, 0);
    if (wlen <= 0) return;

    WCHAR  stackbuf[1024];
    WCHAR *wline = stackbuf;
    if (wlen > (int)(sizeof(stackbuf) / sizeof(stackbuf[0]))) {
        wline = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (size_t)wlen * sizeof(WCHAR));
        if (!wline) return;
    }
    MultiByteToWideChar(CP_UTF8, 0, line, -1, wline, wlen);

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

static HWND make_label(HWND parent, const char *text, int x, int y, int cx) {
    HWND h = CreateWindowA("STATIC", text, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                           x, y, cx, 20, parent, NULL, GetModuleHandle(NULL), NULL);
    SendMessage(h, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
    return h;
}

static HWND make_edit(HWND parent, const char *text, int id, int x, int y, int cx, DWORD extra) {
    HWND h = CreateWindowExA(0, "EDIT", text,
                             WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | ES_AUTOHSCROLL | extra,
                             x, y, cx, 24, parent, (HMENU)(LONG_PTR)id,
                             GetModuleHandle(NULL), NULL);
    SendMessage(h, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
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

        make_label(hwnd, "Server URL:", 14, 12, 120);
        make_edit(hwnd, cfg->server_url, ID_EDIT_URL, 14, 34, 380, 0);

        make_label(hwnd, "API Key:", 14, 66, 120);
        make_edit(hwnd, cfg->api_key, ID_EDIT_KEY, 14, 88, 380, ES_PASSWORD);

        make_label(hwnd, "steamcmd.exe path:", 14, 120, 180);
        make_edit(hwnd, cfg->steamcmd_path, ID_EDIT_CMD, 14, 142, 300, 0);
        make_button(hwnd, "...", ID_BTN_BROWSE, 322, 142, 72, 24, 0);

        make_label(hwnd, "PC ID:", 14, 174, 120);
        make_edit(hwnd, cfg->pc_id, ID_EDIT_PCID, 14, 196, 200, 0);

        make_button(hwnd, "Save", ID_BTN_SAVE, 304, 232, 90, 30, 0);
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
        SetTextColor(dc, CLR_TEXT);
        SetBkColor(dc, CLR_PANEL_ALT);
        return (LRESULT)g_br_panel_alt;
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
                    CW_USEDEFAULT, CW_USEDEFAULT, 425, 300,
                    parent, NULL, GetModuleHandle(NULL), &g_cfg);
}
