@echo off
setlocal EnableDelayedExpansion

rem ===================================================
rem   SERVER SETTINGS (Change here if IP changes)
rem ===================================================
set "SERVER_IP=192.168.20.149"
set "SERVER_PORT=8000"
set "API_KEY=zOTHOQY4gcdx6xSg5RzT-G-u9xJJABivqXjrgz_IlLA"

rem Flags
rem Set DO_RUN=1 to launch updater after setup (ideal for club shell autostart)
rem Set FORCE_UPDATE=1 when you want all PCs to re-download latest exe from GitHub
set "DO_RUN=1"
set "FORCE_UPDATE=0"

rem Check Command Line Argument 1
if not "%~1"=="" (
    set "ARG=%~1"
    if /i "!ARG!"=="--run" ( set "DO_RUN=1" ) else if /i "!ARG!"=="/run" ( set "DO_RUN=1" ) else if /i "!ARG!"=="--update" ( set "FORCE_UPDATE=1" ) else if /i "!ARG!"=="/update" ( set "FORCE_UPDATE=1" ) else if not "!ARG:~0,1!"=="-" if not "!ARG:~0,1!"=="/" ( set "SERVER_IP=!ARG!" )
)

rem Check Command Line Argument 2
if not "%~2"=="" (
    set "ARG=%~2"
    if /i "!ARG!"=="--run" ( set "DO_RUN=1" ) else if /i "!ARG!"=="/run" ( set "DO_RUN=1" ) else if /i "!ARG!"=="--update" ( set "FORCE_UPDATE=1" ) else if /i "!ARG!"=="/update" ( set "FORCE_UPDATE=1" ) else if not "!ARG:~0,1!"=="-" if not "!ARG:~0,1!"=="/" ( set "SERVER_IP=!ARG!" )
)

rem Format SERVER_URL
if "!SERVER_IP:~0,4!"=="http" (
    set "SERVER_URL=!SERVER_IP!"
) else (
    echo !SERVER_IP! | findstr /c:":" >nul
    if errorlevel 1 (
        set "SERVER_URL=http://!SERVER_IP!:!SERVER_PORT!"
    ) else (
        set "SERVER_URL=http://!SERVER_IP!"
    )
)

echo ===================================================
echo   Steam Auto-Updater - Client Auto-Setup
echo   Target Server: !SERVER_URL!
echo ===================================================

rem ---------------------------------------------------
rem 1. Choose the target drive
rem
rem "if exist D:\" was not enough: D: can be a DVD drive, a card reader or a
rem mounted USB stick on some machines, and then the whole updater would land
rem on removable media. We ask Windows for real fixed disks (DriveType=3) with
rem at least 3 GB free and take the first one in order of preference.
rem If PowerShell is unavailable for any reason we fall back to the old logic.
rem ---------------------------------------------------
set "TARGET_DRIVE="
for /f "usebackq delims=" %%D in (`powershell -NoProfile -ExecutionPolicy Bypass -Command "$p='D:','E:','F:','G:','C:'; foreach($x in $p){ $v=Get-CimInstance Win32_LogicalDisk -Filter ('DeviceID=''' + $x + ''' and DriveType=3'); if($v -and $v.FreeSpace -gt 3GB){ [Console]::Out.WriteLine($x); break } }"`) do set "TARGET_DRIVE=%%D"

if not defined TARGET_DRIVE (
    echo [!] Could not query disks, falling back to the simple check.
    if exist "D:\" ( set "TARGET_DRIVE=D:" ) else ( set "TARGET_DRIVE=C:" )
)

set "TARGET_DIR=!TARGET_DRIVE!\SteamUpdater"
if not exist "!TARGET_DIR!" mkdir "!TARGET_DIR!"
echo [*] Target folder: !TARGET_DIR!

rem 2. Find or Install SteamCMD
set "STEAMCMD_EXE="

if exist "!TARGET_DIR!\steamcmd\steamcmd.exe" (
    set "STEAMCMD_EXE=!TARGET_DIR!\steamcmd\steamcmd.exe"
) else if exist "D:\SteamCMD\steamcmd.exe" (
    set "STEAMCMD_EXE=D:\SteamCMD\steamcmd.exe"
) else if exist "C:\SteamCMD\steamcmd.exe" (
    set "STEAMCMD_EXE=C:\SteamCMD\steamcmd.exe"
) else if exist "d:\Ayder_dontdelete\steamcmd\steamcmd.exe" (
    set "STEAMCMD_EXE=d:\Ayder_dontdelete\steamcmd\steamcmd.exe"
)

if not defined STEAMCMD_EXE (
    for /d %%G in ("%LOCALAPPDATA%\Microsoft\WinGet\Packages\Valve.SteamCMD*") do (
        if exist "%%G\steamcmd.exe" set "STEAMCMD_EXE=%%G\steamcmd.exe"
    )
)
if not defined STEAMCMD_EXE (
    for /d %%G in ("C:\Users\*\AppData\Local\Microsoft\WinGet\Packages\Valve.SteamCMD*") do (
        if exist "%%G\steamcmd.exe" set "STEAMCMD_EXE=%%G\steamcmd.exe"
    )
)
if not defined STEAMCMD_EXE (
    for /f "delims=" %%I in ('where steamcmd.exe 2^>nul') do (
        if exist "%%I" set "STEAMCMD_EXE=%%I"
    )
)

set "STEAMCMD_FRESH=0"

if not defined STEAMCMD_EXE (
    echo [*] steamcmd.exe not found. Downloading official SteamCMD portable...
    set "STEAMCMD_DIR=!TARGET_DIR!\steamcmd"
    if not exist "!STEAMCMD_DIR!" mkdir "!STEAMCMD_DIR!"
    set "ZIP_FILE=!STEAMCMD_DIR!\steamcmd.zip"
    
    powershell -NoProfile -ExecutionPolicy Bypass -Command ^
        "$ProgressPreference = 'SilentlyContinue';" ^
        "[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12;" ^
        "Invoke-WebRequest -Uri 'https://steamcdn-a.akamaihd.net/client/installer/steamcmd.zip' -OutFile '!ZIP_FILE!' -UseBasicParsing;" ^
        "Expand-Archive -Path '!ZIP_FILE!' -DestinationPath '!STEAMCMD_DIR!' -Force;" ^
        "Remove-Item -Path '!ZIP_FILE!' -Force -ErrorAction SilentlyContinue;"

    if exist "!STEAMCMD_DIR!\steamcmd.exe" (
        set "STEAMCMD_EXE=!STEAMCMD_DIR!\steamcmd.exe"
        set "STEAMCMD_FRESH=1"
        echo [+] SteamCMD ready: !STEAMCMD_EXE!
    ) else (
        echo [!] Warning: Could not download steamcmd automatically.
    )
) else (
    echo [+] Found SteamCMD: !STEAMCMD_EXE!
)

rem 3. Download or update steam_updater.exe
set "CLIENT_EXE=!TARGET_DIR!\steam_updater.exe"

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "$ProgressPreference = 'SilentlyContinue';" ^
    "[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12;" ^
    "$url = 'https://raw.githubusercontent.com/Ntxfloy/AUto_Update_steam/fix/delta-and-reliability/client_build/steam_updater.exe';" ^
    "$dst = '!CLIENT_EXE!';" ^
    "$need = $false;" ^
    "if ('!FORCE_UPDATE!' -eq '1' -or -not (Test-Path $dst)) { $need = $true }" ^
    "else {" ^
    "    try {" ^
    "        $head = Invoke-WebRequest -Uri $url -Method Head -UseBasicParsing -TimeoutSec 5;" ^
    "        $remLen = [int64]$head.Headers['Content-Length'];" ^
    "        $locLen = (Get-Item $dst).Length;" ^
    "        if ($remLen -gt 0 -and $remLen -ne $locLen) { $need = $true }" ^
    "    } catch { $need = $false }" ^
    "};" ^
    "if ($need) {" ^
    "    Write-Host '[*] Downloading latest steam_updater.exe from GitHub...';" ^
    "    Invoke-WebRequest -Uri $url -OutFile ($dst + '.tmp') -UseBasicParsing -TimeoutSec 30;" ^
    "    if (Test-Path ($dst + '.tmp')) {" ^
    "        Move-Item -Path ($dst + '.tmp') -Destination $dst -Force;" ^
    "        Write-Host '[+] steam_updater.exe updated successfully.';" ^
    "    } else {" ^
    "        Write-Host '[-] Error downloading steam_updater.exe.';" ^
    "    }" ^
    "} else {" ^
    "    Write-Host '[+] steam_updater.exe is up to date.';" ^
    "}"

rem ---------------------------------------------------
rem 4. Check, Create or Update updater.ini
rem
rem The parser now tolerates "key = value" with spaces around the '=' and
rem ignores comment / section lines, so a hand-edited ini is not silently
rem misread (that used to produce keys like "server_url " that matched
rem nothing, and the file was rewritten on every run).
rem ---------------------------------------------------
set "INI_FILE=!TARGET_DIR!\updater.ini"
set "CURRENT_URL="
set "CURRENT_PC_ID="
set "CURRENT_KEY="
set "CURRENT_STEAMCMD="

if exist "!INI_FILE!" (
    for /f "usebackq tokens=1,* delims==" %%A in ("!INI_FILE!") do (
        set "KEY_NAME=%%A"
        set "VAL=%%B"

        rem key: drop every space, so " server_url " -> "server_url"
        set "KEY_NAME=!KEY_NAME: =!"
        set "KEY_NAME=!KEY_NAME:	ab=!"

        rem skip comments and [sections]
        set "SKIP=0"
        if "!KEY_NAME!"==""           set "SKIP=1"
        if "!KEY_NAME:~0,1!"==";"     set "SKIP=1"
        if "!KEY_NAME:~0,1!"=="#"     set "SKIP=1"
        if "!KEY_NAME:~0,1!"=="["     set "SKIP=1"

        if "!SKIP!"=="0" if defined VAL (
            rem value: trim leading spaces, then trailing spaces (paths may
            rem contain inner spaces, so only the edges are touched)
            for /f "tokens=* delims= " %%X in ("!VAL!") do set "VAL=%%X"
            if defined VAL if "!VAL:~-1!"==" " set "VAL=!VAL:~0,-1!"
            if defined VAL if "!VAL:~-1!"==" " set "VAL=!VAL:~0,-1!"
            if defined VAL if "!VAL:~-1!"==" " set "VAL=!VAL:~0,-1!"

            if /i "!KEY_NAME!"=="server_url"    set "CURRENT_URL=!VAL!"
            if /i "!KEY_NAME!"=="pc_id"         set "CURRENT_PC_ID=!VAL!"
            if /i "!KEY_NAME!"=="api_key"       set "CURRENT_KEY=!VAL!"
            if /i "!KEY_NAME!"=="steamcmd_path" set "CURRENT_STEAMCMD=!VAL!"
        )
    )
)

if not defined CURRENT_PC_ID set "CURRENT_PC_ID=%COMPUTERNAME%"
if not defined CURRENT_STEAMCMD set "CURRENT_STEAMCMD=!STEAMCMD_EXE!"
if not defined STEAMCMD_EXE set "STEAMCMD_EXE=!CURRENT_STEAMCMD!"

rem If the remembered steamcmd path no longer exists, prefer the one we found
rem now - otherwise the client keeps pointing at a deleted folder.
if defined CURRENT_STEAMCMD (
    if not exist "!CURRENT_STEAMCMD!" (
        if defined STEAMCMD_EXE if exist "!STEAMCMD_EXE!" (
            echo [*] Stored steamcmd path is gone, using !STEAMCMD_EXE!
            set "CURRENT_STEAMCMD=!STEAMCMD_EXE!"
        )
    )
)

set "NEEDS_WRITE=0"
if not exist "!INI_FILE!" (
    set "NEEDS_WRITE=1"
    echo [*] Creating new updater.ini...
) else if not "!CURRENT_URL!"=="!SERVER_URL!" (
    set "NEEDS_WRITE=1"
    echo [*] Server IP/URL changed:
    echo     Old: !CURRENT_URL!
    echo     New: !SERVER_URL!
    echo [*] Updating updater.ini with new IP...
) else if not "!CURRENT_KEY!"=="!API_KEY!" (
    set "NEEDS_WRITE=1"
    echo [*] API Key changed, updating updater.ini...
) else if not "!CURRENT_STEAMCMD!"=="!STEAMCMD_EXE!" (
    set "NEEDS_WRITE=1"
    echo [*] SteamCMD path changed, updating updater.ini...
) else (
    echo [+] updater.ini is up to date: !CURRENT_URL! [PC: !CURRENT_PC_ID!]
)

if "!NEEDS_WRITE!"=="1" (
    (
        echo ; Steam Auto-Updater client configuration
        echo [AutoUpdater]
        echo server_url=!SERVER_URL!
        echo api_key=!API_KEY!
        echo steamcmd_path=!CURRENT_STEAMCMD!
        echo pc_id=!CURRENT_PC_ID!
    ) > "!INI_FILE!"
    echo [+] updater.ini saved successfully!
)

rem 5. Desktop Shortcut (Universal: Public, xuser, and all local profiles)
echo [*] Checking Desktop shortcuts...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "$tExe = '!CLIENT_EXE!';" ^
    "$tDir = '!TARGET_DIR!';" ^
    "$ws = New-Object -ComObject WScript.Shell;" ^
    "$dirs = @();" ^
    "if ($env:PUBLIC -and (Test-Path ($env:PUBLIC + '\Desktop'))) { $dirs += ($env:PUBLIC + '\Desktop') };" ^
    "if ($env:USERPROFILE -and (Test-Path ($env:USERPROFILE + '\Desktop'))) { $dirs += ($env:USERPROFILE + '\Desktop') };" ^
    "if (Test-Path 'C:\Users') {" ^
    "    Get-ChildItem -Path 'C:\Users' -Directory -ErrorAction SilentlyContinue | ForEach-Object {" ^
    "        $n = $_.Name;" ^
    "        if ($n -notin @('All Users','Default','Default User','desktop.ini','Public')) {" ^
    "            $p1 = Join-Path $_.FullName 'Desktop';" ^
    "            $p2 = Join-Path $_.FullName 'Рабочий стол';" ^
    "            if (Test-Path $p1) { $dirs += $p1 };" ^
    "            if (Test-Path $p2) { $dirs += $p2 };" ^
    "        }" ^
    "    };" ^
    "};" ^
    "$dirs = $dirs | Select-Object -Unique;" ^
    "foreach ($d in $dirs) {" ^
    "    $f1 = Join-Path $d 'Steam Auto-Updater.lnk';" ^
    "    $f2 = Join-Path $d 'Steam Updater.lnk';" ^
    "    if ((Test-Path $f1) -or (Test-Path $f2)) {" ^
    "        Write-Host ('[+] Shortcut already exists in: ' + $d);" ^
    "    } else {" ^
    "        try {" ^
    "            $s = $ws.CreateShortcut($f1);" ^
    "            $s.TargetPath = $tExe;" ^
    "            $s.WorkingDirectory = $tDir;" ^
    "            $s.Description = 'Steam Auto-Updater';" ^
    "            $s.Save();" ^
    "            if (Test-Path $f1) { Write-Host ('[+] Desktop shortcut created in: ' + $d) }" ^
    "        } catch {}" ^
    "    }" ^
    "}"

rem ---------------------------------------------------
rem 6. Warm up a freshly downloaded SteamCMD
rem
rem The very first steamcmd start downloads its own runtime (1-2 minutes).
rem Without this the wait happens inside the first update session and looks
rem exactly like a hang with no progress.
rem ---------------------------------------------------
if "!STEAMCMD_FRESH!"=="1" (
    if exist "!STEAMCMD_EXE!" (
        echo [*] Warming up SteamCMD ^(first run self-update, 1-2 min^)...
        "!STEAMCMD_EXE!" +quit >nul 2>&1
        echo [+] SteamCMD warm-up done.
    )
)

echo ===================================================
echo   Setup Complete! Everything is ready in:
echo   !TARGET_DIR!
echo ===================================================

if "!DO_RUN!"=="1" (
    echo [*] Launching Steam Auto-Updater...
    start "" /d "!TARGET_DIR!" "!CLIENT_EXE!"
)
