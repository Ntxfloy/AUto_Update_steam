// main.c - Steam Auto-Updater client, Win32 GUI entry point
// Native Win32 UI: no external frameworks, single .exe
// v8: UI rebuilt on the same design tokens as the web admin panel.
//     Semantic colour/spacing/type tokens, verified contrast, a real display
//     type scale, keyboard focus rings, grouped button column and a fluid
//     layout. Behaviour (queue, worker, logging, build-id statuses) unchanged.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <uxtheme.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "config.h"
#include "acf.h"
#include "worker.h"
#include "lease.h"
#include "catalog.h"
#include "log.h"

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
#define ID_BTN_OPEN_LOG    115
#define ID_SUBTITLE_TEXT   116
#define ID_LBL_GROUP_RUN   117
#define ID_LBL_GROUP_SEL   118
#define ID_LBL_GROUP_SVC   119
#define ID_TIMER_REFRESH   1001

#define MAX_ROWS 128

// A download that reports no new bytes for this long is almost certainly wedged
// (dead LAN link, Steam CDN hiccup, antivirus holding the file).
#define STALL_WARN_MS   120000

// steamcmd prints "Update state (0x61) downloading, progress: ..." several
// times per second. Printing every one of them turned the log into noise, so
// only one progress line per this many milliseconds reaches the log box.
// The full stream still goes into the debug log file and to the server.
#define LOG_PROGRESS_THROTTLE_MS 2000

// ---------------------------------------------------------------------------
// Design tokens
// ---------------------------------------------------------------------------
// Tier 1: primitives. Same values as the web panel, so both surfaces render
// from one palette. Every pair below was checked by calculation, not by eye:
// body text >= 4.5:1 (WCAG 1.4.3) and control borders >= 3:1 (WCAG 1.4.11).
#define TK_GRAY_1000   RGB(0x0b, 0x0b, 0x0e)
#define TK_GRAY_950    RGB(0x10, 0x10, 0x14)
#define TK_GRAY_900    RGB(0x16, 0x16, 0x1a)
#define TK_GRAY_850    RGB(0x1c, 0x1c, 0x21)
#define TK_GRAY_800    RGB(0x22, 0x22, 0x2a)
#define TK_GRAY_750    RGB(0x2a, 0x2a, 0x33)
#define TK_GRAY_700    RGB(0x34, 0x34, 0x40)
#define TK_GRAY_500    RGB(0x68, 0x68, 0x7f)
#define TK_GRAY_400    RGB(0x9b, 0x9b, 0xab)
#define TK_GRAY_100    RGB(0xe8, 0xe8, 0xef)
#define TK_WHITE       RGB(0xff, 0xff, 0xff)
#define TK_BLUE_650    RGB(0x25, 0x60, 0xe0)
#define TK_BLUE_600    RGB(0x25, 0x63, 0xeb)
#define TK_BLUE_550    RGB(0x2f, 0x6d, 0xf0)
#define TK_BLUE_400    RGB(0x7a, 0xa7, 0xf8)
#define TK_GREEN_400   RGB(0x5f, 0xd9, 0x8b)
#define TK_AMBER_400   RGB(0xf0, 0xc2, 0x50)
#define TK_RED_400     RGB(0xf2, 0x8b, 0x8b)
#define TK_RED_700     RGB(0x8c, 0x22, 0x22)

// Tier 2: semantic. These are the names the UI code is allowed to use.
#define CLR_BG              TK_GRAY_950
#define CLR_SURFACE         TK_GRAY_900
#define CLR_SURFACE_ALT     TK_GRAY_850
#define CLR_SURFACE_SUNKEN  TK_GRAY_1000
#define CLR_SURFACE_HOVER   TK_GRAY_800
#define CLR_SURFACE_ACTIVE  TK_GRAY_750
#define CLR_SEPARATOR       TK_GRAY_750
#define CLR_BORDER          TK_GRAY_500
#define CLR_TEXT            TK_GRAY_100
#define CLR_TEXT_DIM        TK_GRAY_400
#define CLR_TEXT_DISABLED   TK_GRAY_500
#define CLR_ON_ACTION       TK_WHITE
#define CLR_ACTION          TK_BLUE_600
#define CLR_ACTION_HOVER    TK_BLUE_550
#define CLR_ACTION_ACTIVE   TK_BLUE_650
#define CLR_FOCUS_RING      TK_BLUE_400
#define CLR_OK              TK_GREEN_400
#define CLR_WARN            TK_AMBER_400
#define CLR_FAIL            TK_RED_400
#define CLR_FAIL_FILL       TK_RED_700
#define CLR_INFO            TK_BLUE_400

// Tier 3: component tokens.
#define PAD            24   // window margin
#define CARD_PAD        8   // rounded frame painted around a square control
#define COL_GAP        24
#define RIGHT_W       260   // button column
#define RADIUS_LG      14
#define RADIUS_MD      10
#define H_BTN_PRIMARY  44
#define H_BTN          32
#define H_BTN_SM       30
#define H_PROGRESS     34
#define H_STATUS       22
#define H_QUEUE        20
#define H_LOG_MIN     170
#define H_LIST_MIN    200
#define GROUP_LBL_H    18
#define Y_CONTENT     116   // first row under the title block

// Undocumented-but-stable DWM attribute for a dark title bar.
// Loaded dynamically so the exe still starts on Windows 7/8.
#define DWMWA_USE_IMMERSIVE_DARK_MODE_OLD 19
#define DWMWA_USE_IMMERSIVE_DARK_MODE     20

static HBRUSH g_br_bg        = NULL;
static HBRUSH g_br_surface   = NULL;
static HBRUSH g_br_surface_a = NULL;
static HFONT  g_font_ui      = NULL;
static HFONT  g_font_display = NULL;
static HFONT  g_font_label   = NULL;
static HFONT  g_font_mono    = NULL;
static HIMAGELIST g_row_il   = NULL;   // only there to give the rows real height

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
static HWND          g_sub_txt          = NULL;
static HWND          g_lbl_run         = NULL;
static HWND          g_lbl_sel         = NULL;
static HWND          g_lbl_svc         = NULL;
static HWND          g_btn_update      = NULL;
static HWND          g_btn_update_all  = NULL;
static HWND          g_btn_refresh     = NULL;
static HWND          g_btn_settings    = NULL;
static HWND          g_btn_abort       = NULL;
static HWND          g_btn_check_all   = NULL;
static HWND          g_btn_uncheck_all = NULL;
static HWND          g_btn_check_f2p   = NULL;
static HWND          g_btn_open_log    = NULL;
static HWND          g_progress        = NULL;
static HWND          g_status_txt      = NULL;
static HWND          g_queue_txt       = NULL;
static HWND          g_log_txt         = NULL;

static Config        g_cfg                 = {0};
static char          g_cfg_path[MAX_PATH]  = {0};
static char          g_logfile[MAX_PATH]   = {0};
static char          g_dbglog[MAX_PATH]    = {0};
static char          g_sel_path[MAX_PATH]  = {0};
static GameRow       g_rows[MAX_ROWS]      = {0};
static int           g_row_count           = 0;

// Remembered checkbox selection (appids), loaded from selection.ini at start
// and rewritten every time the operator ticks something.
static char          g_sel_ids[MAX_ROWS][32] = 0;
static int           g_sel_count             = 0;
static int           g_sel_loaded            = 0;
static int           g_sel_suppress          = 0;   // we are filling the list ourselves

static HANDLE        g_worker      = NULL;
static HANDLE        g_single_inst = NULL;
static WorkerStatus  g_wstatus     = {0};

// Sequential queue of row indices
static int           g_queue[MAX_ROWS] = {0};
static int           g_queue_len       = 0;
static int           g_queue_pos       = 0;
static int           g_current_row     = -1;
static int           g_ok_count        = 0;
static int           g_skip_count      = 0;
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
static void      SelectionLoad(void);
static void      SelectionSave(void);
static int       SelectionContains(const char *appid);
static void      OpenLogInNotepad(void);
static void      LayoutAll(int w, int h);

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
// Persistent checkbox selection
// ---------------------------------------------------------------------------
static int SelectionContains(const char *appid) {
    for (int i = 0; i < g_sel_count; i++)
        if (strcmp(g_sel_ids[i], appid) == 0) return 1;
    return 0;
}

static void SelectionLoad(void) {
    g_sel_count  = 0;
    g_sel_loaded = 0;
    FILE *f = fopen(g_sel_path, "r");
    if (!f) return;
    char line[64];
    while (fgets(line, sizeof(line), f) && g_sel_count < MAX_ROWS) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        size_t n = strlen(p);
        while (n > 0 && (p[n-1] == '\n' || p[n-1] == '\r' || p[n-1] == ' ')) p[--n] = '\0';
        if (!n || p[0] == '#' || p[0] == '[') continue;
        strncpy(g_sel_ids[g_sel_count], p, 31);
        g_sel_ids[g_sel_count][31] = '\0';
        g_sel_count++;
    }
    fclose(f);
    g_sel_loaded = 1;   // even an empty file is a deliberate "nothing checked"
}

static void SelectionSave(void) {
    if (!g_list || g_sel_suppress) return;
    g_sel_count = 0;
    for (int i = 0; i < g_row_count && g_sel_count < MAX_ROWS; i++) {
        if (!ListView_GetCheckState(g_list, i)) continue;
        strncpy(g_sel_ids[g_sel_count], g_rows[i].appid, 31);
        g_sel_ids[g_sel_count][31] = '\0';
        g_sel_count++;
    }
    g_sel_loaded = 1;
    FILE *f = fopen(g_sel_path, "w");
    if (!f) return;
    fputs("# Steam Auto-Updater: games checked last time (one AppID per line)\n", f);
    for (int i = 0; i < g_sel_count; i++) fprintf(f, "%s\n", g_sel_ids[i]);
    fclose(f);
    LOG_DEBUG("ui", "selection saved: %d row(s)", g_sel_count);
}

// ---------------------------------------------------------------------------
// Dark theming helpers
// ---------------------------------------------------------------------------
typedef HTHEME (WINAPI *fnOpenNcThemeData)(HWND hWnd, LPCWSTR pszClassList);
static fnOpenNcThemeData g_pfnOpenNcThemeData = NULL;

static HTHEME WINAPI MyOpenThemeData(HWND hWnd, LPCWSTR classList) {
    if (classList && wcscmp(classList, L"ScrollBar") == 0) {
        hWnd = NULL;
        classList = L"Explorer::ScrollBar";
    }
    return g_pfnOpenNcThemeData(hWnd, classList);
}

typedef struct {
    DWORD Attributes;
    DWORD DllNameRVA;
    DWORD ModuleHandleRVA;
    DWORD ImportAddressTableRVA;
    DWORD ImportNameTableRVA;
    DWORD BoundImportAddressTableRVA;
    DWORD UnloadInformationTableRVA;
    DWORD TimeDateStamp;
} ImgDelayDescr;

static void FixDarkScrollBar(void) {
    HMODULE hUxTheme = LoadLibraryA("uxtheme.dll");
    if (!hUxTheme) return;

    g_pfnOpenNcThemeData = (fnOpenNcThemeData)GetProcAddress(hUxTheme, MAKEINTRESOURCEA(49));
    if (!g_pfnOpenNcThemeData) return;

    HMODULE hComctl = LoadLibraryA("comctl32.dll");
    if (!hComctl) return;

    BYTE *base = (BYTE *)hComctl;
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;

    IMAGE_DATA_DIRECTORY delayDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (delayDir.VirtualAddress) {
        ImgDelayDescr *desc = (ImgDelayDescr *)(base + delayDir.VirtualAddress);
        for (; desc->DllNameRVA; ++desc) {
            const char *dll = (const char *)(base + desc->DllNameRVA);
            if (_stricmp(dll, "uxtheme.dll") == 0) {
                PIMAGE_THUNK_DATA impName = (PIMAGE_THUNK_DATA)(base + desc->ImportNameTableRVA);
                PIMAGE_THUNK_DATA impAddr = (PIMAGE_THUNK_DATA)(base + desc->ImportAddressTableRVA);
                for (; impName->u1.Ordinal; ++impName, ++impAddr) {
                    if (IMAGE_SNAP_BY_ORDINAL(impName->u1.Ordinal) && IMAGE_ORDINAL(impName->u1.Ordinal) == 49) {
                        DWORD oldProt;
                        if (VirtualProtect(impAddr, sizeof(IMAGE_THUNK_DATA), PAGE_READWRITE, &oldProt)) {
                            impAddr->u1.Function = (ULONG_PTR)MyOpenThemeData;
                            VirtualProtect(impAddr, sizeof(IMAGE_THUNK_DATA), oldProt, &oldProt);
                        }
                        break;
                    }
                }
                break;
            }
        }
    }

    IMAGE_DATA_DIRECTORY impDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (impDir.VirtualAddress) {
        PIMAGE_IMPORT_DESCRIPTOR desc = (PIMAGE_IMPORT_DESCRIPTOR)(base + impDir.VirtualAddress);
        for (; desc->Name; ++desc) {
            const char *dll = (const char *)(base + desc->Name);
            if (_stricmp(dll, "uxtheme.dll") == 0) {
                PIMAGE_THUNK_DATA impName = (PIMAGE_THUNK_DATA)(base + desc->OriginalFirstThunk);
                PIMAGE_THUNK_DATA impAddr = (PIMAGE_THUNK_DATA)(base + desc->FirstThunk);
                for (; impName->u1.Ordinal; ++impName, ++impAddr) {
                    if (IMAGE_SNAP_BY_ORDINAL(impName->u1.Ordinal) && IMAGE_ORDINAL(impName->u1.Ordinal) == 49) {
                        DWORD oldProt;
                        if (VirtualProtect(impAddr, sizeof(IMAGE_THUNK_DATA), PAGE_READWRITE, &oldProt)) {
                            impAddr->u1.Function = (ULONG_PTR)MyOpenThemeData;
                            VirtualProtect(impAddr, sizeof(IMAGE_THUNK_DATA), oldProt, &oldProt);
                        }
                        break;
                    }
                }
                break;
            }
        }
    }
}

typedef BOOL (WINAPI *fnAllowDarkModeForWindow)(HWND, BOOL);
static fnAllowDarkModeForWindow g_pfnAllowDarkModeForWindow = NULL;

static void init_app_dark_mode(void) {
    HMODULE uxtheme = LoadLibraryA("uxtheme.dll");
    if (uxtheme) {
        typedef enum { Default = 0, AllowDark = 1, ForceDark = 2, ForceLight = 3, Max = 4 } PreferredAppMode;
        typedef PreferredAppMode (WINAPI *fnSetPreferredAppMode)(PreferredAppMode);
        fnSetPreferredAppMode set_mode = (fnSetPreferredAppMode)GetProcAddress(uxtheme, MAKEINTRESOURCEA(135));
        if (set_mode) {
            set_mode(ForceDark);
        } else {
            typedef BOOL (WINAPI *fnAllowDarkModeForApp)(BOOL);
            fnAllowDarkModeForApp allow_app = (fnAllowDarkModeForApp)GetProcAddress(uxtheme, MAKEINTRESOURCEA(135));
            if (allow_app) allow_app(TRUE);
        }

        typedef void (WINAPI *fnRefreshImmersiveColorPolicyState)(void);
        fnRefreshImmersiveColorPolicyState refresh = (fnRefreshImmersiveColorPolicyState)GetProcAddress(uxtheme, MAKEINTRESOURCEA(104));
        if (refresh) refresh();

        typedef void (WINAPI *fnFlushMenuThemes)(void);
        fnFlushMenuThemes flush = (fnFlushMenuThemes)GetProcAddress(uxtheme, MAKEINTRESOURCEA(136));
        if (flush) flush();

        g_pfnAllowDarkModeForWindow = (fnAllowDarkModeForWindow)GetProcAddress(uxtheme, MAKEINTRESOURCEA(133));
    }

    FixDarkScrollBar();
}

static void enable_dark_control(HWND ctl) {
    if (!ctl) return;
    if (g_pfnAllowDarkModeForWindow) g_pfnAllowDarkModeForWindow(ctl, TRUE);
    if (FAILED(SetWindowTheme(ctl, L"DarkMode_Explorer", NULL)))
        SetWindowTheme(ctl, L"Explorer", NULL);
    SendMessageW(ctl, WM_THEMECHANGED, 0, 0);
}

static void enable_dark_titlebar(HWND hwnd) {
    if (g_pfnAllowDarkModeForWindow) g_pfnAllowDarkModeForWindow(hwnd, TRUE);

    HMODULE dwm = LoadLibraryA("dwmapi.dll");
    if (!dwm) return;
    typedef HRESULT (WINAPI *SetAttrFn)(HWND, DWORD, LPCVOID, DWORD);
    SetAttrFn set_attr = (SetAttrFn)GetProcAddress(dwm, "DwmSetWindowAttribute");
    if (set_attr) {
        BOOL on = TRUE;
        if (FAILED(set_attr(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &on, sizeof(on))))
            set_attr(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE_OLD, &on, sizeof(on));

        // Windows 11 rounded window corners (DWMWA_WINDOW_CORNER_PREFERENCE = 33, DWMWCP_ROUND = 2)
        DWORD corner = 2;
        set_attr(hwnd, 33, &corner, sizeof(corner));
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

// A visible keyboard focus ring. Win32 owner-drawn buttons get none for free,
// which made the window unusable without a mouse.
static void draw_focus_ring(HDC dc, RECT rc, int radius) {
    HPEN   pn = CreatePen(PS_SOLID, 2, CLR_FOCUS_RING);
    HPEN   op = (HPEN)SelectObject(dc, pn);
    HBRUSH ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
    InflateRect(&rc, -2, -2);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pn);
}

// Rounded card painted behind a child control, so square Win32 controls
// (ListView, EDIT) end up looking like modern rounded panels.
static void paint_card(HDC dc, HWND parent, HWND child, int radius) {
    if (!child) return;
    RECT rc;
    GetWindowRect(child, &rc);
    MapWindowPoints(NULL, parent, (POINT *)&rc, 2);
    InflateRect(&rc, CARD_PAD, CARD_PAD);
    fill_round(dc, rc, CLR_SURFACE, CLR_SEPARATOR, radius);
}

// --- Owner-drawn buttons: hover tracking via a tiny subclass ---------------
static WNDPROC g_btn_oldproc = NULL;
static WNDPROC g_progress_oldproc = NULL;

static LRESULT CALLBACK ProgressSubProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_WINDOWPOSCHANGED || m == WM_SIZE) {
        InvalidateRect(h, NULL, TRUE);
    }
    return CallWindowProcA(g_progress_oldproc, h, m, w, l);
}

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
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP | BS_OWNERDRAW
        | (disabled ? WS_DISABLED : 0),
        x, y, cx, cy, parent, (HMENU)(LONG_PTR)id, GetModuleHandle(NULL), NULL);
    SendMessage(b, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
    WNDPROC prev = (WNDPROC)SetWindowLongPtrA(b, GWLP_WNDPROC, (LONG_PTR)BtnSubProc);
    if (!g_btn_oldproc) g_btn_oldproc = prev;
    return b;
}

// Uppercase eyebrow label above a button group, so the right column reads as
// three intentional groups instead of nine stacked buttons.
static HWND make_group_label(HWND parent, const char *text, int id,
                             int x, int y, int cx) {
    HWND h = CreateWindowA("STATIC", text,
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | SS_LEFT,
        x, y, cx, GROUP_LBL_H, parent, (HMENU)(LONG_PTR)id, GetModuleHandle(NULL), NULL);
    SendMessage(h, WM_SETFONT, (WPARAM)g_font_label, TRUE);
    return h;
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
        HBRUSH br = CreateSolidBrush(CLR_SURFACE_ALT);
        FillRect(dc, &rc, br);
        DeleteObject(br);

        HFONT of = (HFONT)SelectObject(dc, g_font_label);
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
        }

        // Single hairline under the header instead of a divider per column:
        // fewer lines, same structure.
        HPEN pn = CreatePen(PS_SOLID, 1, CLR_SEPARATOR);
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
// ---------------------------------------------------------------------------
static void keep_awake(int on) {
    if (on) SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_AWAYMODE_REQUIRED);
    else    SetThreadExecutionState(ES_CONTINUOUS);
}

// Open the debug log without dragging shell32 into the link line.
static void OpenLogInNotepad(void) {
    const char *path = log_file_path();
    if (!path || !path[0]) return;
    char cmd[MAX_PATH + 32];
    snprintf(cmd, sizeof(cmd), "notepad.exe \"%s\"", path);
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = {0};
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

// ---------------------------------------------------------------------------
// Layout: one function computes every rect from the client size, so the window
// scales instead of hiding the log behind hardcoded Y offsets.
// ---------------------------------------------------------------------------
static void LayoutAll(int w, int h) {
    if (w <= 0 || h <= 0 || !g_list) return;

    int full_w = w - PAD * 2;
    if (full_w < 400) full_w = 400;

    int list_w = full_w - RIGHT_W - COL_GAP;
    if (list_w < 360) list_w = 360;
    int bx = PAD + list_w + COL_GAP;

    int bottom_h = H_PROGRESS + 12 + H_STATUS + H_QUEUE + 14 + H_LOG_MIN;
    int list_h   = h - Y_CONTENT - bottom_h - PAD;
    if (list_h < H_LIST_MIN) list_h = H_LIST_MIN;

    int log_h = h - (Y_CONTENT + list_h + bottom_h - H_LOG_MIN) - PAD;
    if (log_h < H_LOG_MIN) log_h = H_LOG_MIN;

    int y_progress = Y_CONTENT + list_h + 26;
    int y_status   = y_progress + H_PROGRESS + 12;
    int y_queue    = y_status + H_STATUS;
    int y_log      = y_queue + H_QUEUE + 14;

    MoveWindow(g_title_txt, PAD, 26, full_w, 46, TRUE);
    MoveWindow(g_sub_txt,   PAD, 76, full_w, 20, TRUE);
    MoveWindow(g_list,      PAD, Y_CONTENT, list_w, list_h, TRUE);

    int other_cols = 150 + 80 + 90 + 70;
    int game_w = list_w - other_cols - 30;
    if (game_w > 140) ListView_SetColumnWidth(g_list, 0, game_w);

    int y = Y_CONTENT;
    MoveWindow(g_lbl_run, bx, y, RIGHT_W, GROUP_LBL_H, TRUE);          y += GROUP_LBL_H + 6;
    MoveWindow(g_btn_update_all, bx, y, RIGHT_W, H_BTN_PRIMARY, TRUE); y += H_BTN_PRIMARY + 8;
    MoveWindow(g_btn_update,     bx, y, RIGHT_W, H_BTN, TRUE);         y += H_BTN + 8;
    MoveWindow(g_btn_abort,      bx, y, RIGHT_W, H_BTN, TRUE);         y += H_BTN + 22;

    MoveWindow(g_lbl_sel, bx, y, RIGHT_W, GROUP_LBL_H, TRUE);          y += GROUP_LBL_H + 6;
    MoveWindow(g_btn_check_all,   bx, y, RIGHT_W, H_BTN_SM, TRUE);     y += H_BTN_SM + 6;
    MoveWindow(g_btn_uncheck_all, bx, y, RIGHT_W, H_BTN_SM, TRUE);     y += H_BTN_SM + 6;
    MoveWindow(g_btn_check_f2p,   bx, y, RIGHT_W, H_BTN_SM, TRUE);     y += H_BTN_SM + 22;

    MoveWindow(g_lbl_svc, bx, y, RIGHT_W, GROUP_LBL_H, TRUE);          y += GROUP_LBL_H + 6;
    MoveWindow(g_btn_refresh,  bx, y, RIGHT_W, H_BTN_SM, TRUE);        y += H_BTN_SM + 6;
    MoveWindow(g_btn_settings, bx, y, RIGHT_W, H_BTN_SM, TRUE);        y += H_BTN_SM + 6;
    MoveWindow(g_btn_open_log, bx, y, RIGHT_W, H_BTN_SM, TRUE);

    MoveWindow(g_progress,   PAD, y_progress, full_w, H_PROGRESS, TRUE);
    MoveWindow(g_status_txt, PAD, y_status,   full_w, H_STATUS, TRUE);
    MoveWindow(g_queue_txt,  PAD, y_queue,    full_w, H_QUEUE, TRUE);
    MoveWindow(g_log_txt,    PAD, y_log,      full_w, log_h, TRUE);

    RedrawWindow(g_progress, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    InvalidateRect(g_status_txt, NULL, TRUE);
    InvalidateRect(g_queue_txt, NULL, TRUE);
    InvalidateRect(g_hwnd, NULL, TRUE);
}

// ---------------------------------------------------------------------------
// WinMain
// ---------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
    (void)hPrev; (void)lpCmd;

    // --- single instance ---------------------------------------------------
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

    init_app_dark_mode();

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);

    g_br_bg        = CreateSolidBrush(CLR_BG);
    g_br_surface   = CreateSolidBrush(CLR_SURFACE);
    g_br_surface_a = CreateSolidBrush(CLR_SURFACE_ALT);

    // Type scale: 38px display over 15px body is a 2.5x jump, so the window
    // has an obvious first place for the eye.
    g_font_ui = CreateFontA(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS, "Segoe UI");
    g_font_display = CreateFontA(-38, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS, "Segoe UI");
    g_font_label = CreateFontA(-12, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS, "Segoe UI");
    g_font_mono = CreateFontA(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");

    // config, log files and remembered selection live next to the exe
    GetModuleFileNameA(NULL, g_cfg_path, MAX_PATH);
    char *last_slash = strrchr(g_cfg_path, '\\');
    if (last_slash) *(last_slash + 1) = '\0';
    strncpy(g_logfile,  g_cfg_path, MAX_PATH - 1);
    strncpy(g_dbglog,   g_cfg_path, MAX_PATH - 1);
    strncpy(g_sel_path, g_cfg_path, MAX_PATH - 1);
    strncat(g_logfile,  "updater.log",        MAX_PATH - strlen(g_logfile)  - 1);
    strncat(g_dbglog,   "updater-debug.log",  MAX_PATH - strlen(g_dbglog)   - 1);
    strncat(g_sel_path, "selection.ini",      MAX_PATH - strlen(g_sel_path) - 1);
    strncat(g_cfg_path, "updater.ini",        MAX_PATH - strlen(g_cfg_path) - 1);
    int cfg_ok = config_load(g_cfg_path, &g_cfg);
    SelectionLoad();

    // --- verbose logging: file next to the exe + upload to the server -----
    // updater.log stays the short, human-readable UI log; updater-debug.log
    // gets every single line (including the raw steamcmd stream) and the same
    // lines are pushed to the server so 50 PCs can be debugged from one page.
    log_init(g_dbglog, LOG_LEVEL_DEBUG);
    char pc_name[64] = {0};
    if (g_cfg.pc_id[0]) {
        strncpy(pc_name, g_cfg.pc_id, sizeof(pc_name) - 1);
    } else {
        DWORD n = sizeof(pc_name);
        GetComputerNameA(pc_name, &n);
    }
    log_set_pc_id(pc_name);
    if (g_cfg.server_url[0] && g_cfg.api_key[0]) {
        log_set_remote(g_cfg.server_url, g_cfg.api_key, 1);
        log_set_remote_level(LOG_LEVEL_DEBUG);
    }
    LOG_INFO("app", "=== Steam Auto-Updater v8.0 starting, session %s ===", log_session_id());
    LOG_INFO("app", "pc_id=%s server=%s steamcmd=%s",
             pc_name,
             g_cfg.server_url[0] ? g_cfg.server_url : "(not set)",
             g_cfg.steamcmd_path[0] ? g_cfg.steamcmd_path : "(not set)");
    LOG_INFO("app", "debug log: %s", g_dbglog);

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
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = "SteamAutoUpdater";
    // No class background brush: we paint the whole client area in WM_PAINT.
    wc.hbrBackground = NULL;
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    // WS_EX_COMPOSITED (an XP-era double-buffering hack) fights with DWM on
    // Windows 10/11 and with the ListView's own double buffering.
    g_hwnd = CreateWindowExA(WS_EX_APPWINDOW, "SteamAutoUpdater",
                              "Steam Auto-Updater",
                              WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1120, 880,
                              NULL, NULL, hInst, NULL);
    enable_dark_titlebar(g_hwnd);
    ShowWindow(g_hwnd, nShow);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (IsDialogMessageA(g_hwnd, &msg)) continue;   // Tab / arrows / Enter
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    keep_awake(0);
    LOG_INFO("app", "=== exiting, %d ok / %d failed in this session ===",
             g_ok_count, g_fail_count);
    log_shutdown();
    DeleteCriticalSection(&g_wstatus.lock);
    if (g_row_il) ImageList_Destroy(g_row_il);
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
        // Cards behind the two square controls, so the layout reads as panels.
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
            "Tick the games, press Update. Fresh installs go to a non-system disk only.",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | SS_LEFT | SS_ENDELLIPSIS,
            PAD, 76, 600, 20, hwnd, (HMENU)ID_SUBTITLE_TEXT, hi, NULL);
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

        // 15px text in an 18px row was the "cramped" look. A 1x26 image list is
        // the supported way to give a report-mode ListView taller rows.
        g_row_il = ImageList_Create(1, 26, ILC_COLOR32, 1, 1);
        if (g_row_il) ListView_SetImageList(g_list, g_row_il, LVSIL_SMALL);

        HWND hdr = ListView_GetHeader(g_list);
        if (hdr) {
            SendMessage(hdr, WM_SETFONT, (WPARAM)g_font_label, TRUE);
            g_hdr_oldproc = (WNDPROC)SetWindowLongPtrA(hdr, GWLP_WNDPROC, (LONG_PTR)HeaderSubProc);
        }

        // Col 0 is Game name so checkboxes are next to game title!
        LVCOLUMNA col = {0};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx = 240; col.pszText = (LPSTR)"GAME";    ListView_InsertColumn(g_list, 0, &col);
        col.cx = 150; col.pszText = (LPSTR)"STATUS";  ListView_InsertColumn(g_list, 1, &col);
        col.cx = 80;  col.pszText = (LPSTR)"SIZE";    ListView_InsertColumn(g_list, 2, &col);
        col.cx = 90;  col.pszText = (LPSTR)"BUILD";   ListView_InsertColumn(g_list, 3, &col);
        col.cx = 70;  col.pszText = (LPSTR)"APPID";   ListView_InsertColumn(g_list, 4, &col);

        enable_dark_control(g_list);

        int bx = PAD + 660 + COL_GAP;

        g_lbl_run = make_group_label(hwnd, "RUN", ID_LBL_GROUP_RUN, bx, Y_CONTENT, RIGHT_W);
        g_btn_update_all  = make_button(hwnd, "Update checked games", ID_BTN_UPDATE_ALL,  bx, 0, RIGHT_W, H_BTN_PRIMARY, 0);
        g_btn_update      = make_button(hwnd, "Update selected row",  ID_BTN_UPDATE,      bx, 0, RIGHT_W, H_BTN, 0);
        g_btn_abort       = make_button(hwnd, "Stop",                 ID_BTN_ABORT,       bx, 0, RIGHT_W, H_BTN, 1);

        g_lbl_sel = make_group_label(hwnd, "SELECTION", ID_LBL_GROUP_SEL, bx, 0, RIGHT_W);
        g_btn_check_all   = make_button(hwnd, "Select all",           ID_BTN_CHECK_ALL,   bx, 0, RIGHT_W, H_BTN_SM, 0);
        g_btn_uncheck_all = make_button(hwnd, "Clear selection",      ID_BTN_UNCHECK_ALL, bx, 0, RIGHT_W, H_BTN_SM, 0);
        g_btn_check_f2p   = make_button(hwnd, "Only free-to-play",    ID_BTN_CHECK_F2P,   bx, 0, RIGHT_W, H_BTN_SM, 0);

        g_lbl_svc = make_group_label(hwnd, "SERVICE", ID_LBL_GROUP_SVC, bx, 0, RIGHT_W);
        g_btn_refresh     = make_button(hwnd, "Refresh list",         ID_BTN_REFRESH,     bx, 0, RIGHT_W, H_BTN_SM, 0);
        g_btn_settings    = make_button(hwnd, "Settings",             ID_BTN_SETTINGS,    bx, 0, RIGHT_W, H_BTN_SM, 0);
        g_btn_open_log    = make_button(hwnd, "Open debug log",       ID_BTN_OPEN_LOG,    bx, 0, RIGHT_W, H_BTN_SM, 0);

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
        if (ctl == g_title_txt)                     SetTextColor(dc, CLR_TEXT);
        else if (ctl == g_sub_txt)                  SetTextColor(dc, CLR_TEXT_DIM);
        else if (ctl == g_lbl_run || ctl == g_lbl_sel || ctl == g_lbl_svc)
                                                    SetTextColor(dc, CLR_TEXT_DIM);
        else if (ctl == g_queue_txt)                SetTextColor(dc, CLR_TEXT_DIM);
        else                                        SetTextColor(dc, CLR_TEXT);
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

            // The label crosses the fill edge, so it is drawn twice with a clip:
            // white over the filled part, normal text over the empty track.
            // One single colour always failed contrast on one of the two halves.
            SetBkMode(di->hDC, TRANSPARENT);
            HFONT of = (HFONT)SelectObject(di->hDC, g_font_ui);
            int filled_text = (g_pct_warn || (!g_pct_failed && p < 99.999));

            if (w > 4) {
                HRGN clip = CreateRectRgn(fr.left, fr.top, fr.right, fr.bottom);
                SelectClipRgn(di->hDC, clip);
                SetTextColor(di->hDC, filled_text ? CLR_BG : CLR_ON_ACTION);
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

            int hovered  = (int)GetWindowLongPtrA(di->hwndItem, GWLP_USERDATA);
            int pressed  = (di->itemState & ODS_SELECTED) ? 1 : 0;
            int disabled = (di->itemState & ODS_DISABLED) ? 1 : 0;
            int focused  = (di->itemState & ODS_FOCUS) ? 1 : 0;
            int primary  = (di->CtlID == ID_BTN_UPDATE_ALL);
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
                border = hovered || pressed ? CLR_FAIL : CLR_BORDER;
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
            DrawTextA(di->hDC, txt, -1, &rc,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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
                    // Status column
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
                    // Game column
                    cd->clrText = g_rows[row].installed ? CLR_TEXT : CLR_TEXT_DIM;
                } else {
                    // Other columns (Size, BuildID, AppID)
                    cd->clrText = CLR_TEXT_DIM;
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
                    "Abort", MB_YESNO | MB_ICONQUESTION) != IDYES)
                return 0;
            g_aborting = 1;
            worker_request_abort();
            g_queue_len = g_queue_pos;   // stop after the current game
            EnableWindow(g_btn_abort, FALSE);
            SetWindowTextA(g_status_txt, "Stopping after the current game...");
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
// Queue handling
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

static int StartRow(int row_idx) {
    EnterCriticalSection(&g_wstatus.lock);
    g_wstatus.state = WORKER_IDLE;
    g_wstatus.progress = 0.0;
    g_wstatus.state_desc[0]     = '\0';
    g_wstatus.last_log_line[0]  = '\0';