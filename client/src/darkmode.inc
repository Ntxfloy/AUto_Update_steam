// darkmode.inc - Windows dark-mode plumbing for the client window.
//
// Included by main.c. This is pure platform glue and never changed with the
// redesign: undocumented uxtheme ordinals, the comctl32 import patch that gives
// us dark scrollbars, the dark title bar and Windows 11 rounded corners.
// Everything is resolved dynamically, so the exe still starts on Windows 7/8.

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
