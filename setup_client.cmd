@echo off
setlocal EnableDelayedExpansion

rem ===================================================
rem   SERVER SETTINGS (Change here if IP changes)
rem ===================================================
set "SERVER_IP=192.168.20.149"
set "SERVER_PORT=8000"
set "API_KEY=zOTHOQY4gcdx6xSg5RzT-G-u9xJJABivqXjrgz_IlLA"

rem Flags
set "DO_RUN=0"
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

rem 1. Choose Target Drive (Prefer D:, fallback to C:)
if exist "D:\" (
    set "TARGET_DIR=D:\SteamUpdater"
) else (
    set "TARGET_DIR=C:\SteamUpdater"
)

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
        echo [+] SteamCMD ready: !STEAMCMD_EXE!
    ) else (
        echo [!] Warning: Could not download steamcmd automatically.
    )
) else (
    echo [+] Found SteamCMD: !STEAMCMD_EXE!
)

rem 3. Download or update steam_updater.exe
set "CLIENT_EXE=!TARGET_DIR!\steam_updater.exe"

if exist "!CLIENT_EXE!" (
    if "!FORCE_UPDATE!"=="1" (
        echo [*] Update requested. Downloading latest steam_updater.exe from GitHub...
        powershell -NoProfile -ExecutionPolicy Bypass -Command ^
            "$ProgressPreference = 'SilentlyContinue';" ^
            "[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12;" ^
            "Invoke-WebRequest -Uri 'https://raw.githubusercontent.com/Ntxfloy/AUto_Update_steam/feat/dark-ui-and-layout-fixes/client_build/steam_updater.exe' -OutFile '!CLIENT_EXE!' -UseBasicParsing;"
        echo [+] steam_updater.exe updated.
    ) else (
        echo [+] steam_updater.exe already installed.
    )
) else (
    echo [*] Downloading steam_updater.exe from GitHub...
    powershell -NoProfile -ExecutionPolicy Bypass -Command ^
        "$ProgressPreference = 'SilentlyContinue';" ^
        "[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12;" ^
        "Invoke-WebRequest -Uri 'https://raw.githubusercontent.com/Ntxfloy/AUto_Update_steam/feat/dark-ui-and-layout-fixes/client_build/steam_updater.exe' -OutFile '!CLIENT_EXE!' -UseBasicParsing;"
    
    if exist "!CLIENT_EXE!" (
        echo [+] steam_updater.exe downloaded successfully.
    ) else (
        echo [!] Error downloading steam_updater.exe!
    )
)

rem 4. Check, Create or Update updater.ini
set "INI_FILE=!TARGET_DIR!\updater.ini"
set "CURRENT_URL="
set "CURRENT_PC_ID="
set "CURRENT_KEY="
set "CURRENT_STEAMCMD="

if exist "!INI_FILE!" (
    for /f "tokens=1,* delims==" %%A in ('type "!INI_FILE!" 2^>nul') do (
        set "KEY_NAME=%%A"
        set "VAL=%%B"
        if /i "!KEY_NAME!"=="server_url" set "CURRENT_URL=!VAL!"
        if /i "!KEY_NAME!"=="pc_id" set "CURRENT_PC_ID=!VAL!"
        if /i "!KEY_NAME!"=="api_key" set "CURRENT_KEY=!VAL!"
        if /i "!KEY_NAME!"=="steamcmd_path" set "CURRENT_STEAMCMD=!VAL!"
    )
)

if not defined CURRENT_PC_ID set "CURRENT_PC_ID=%COMPUTERNAME%"
if not defined CURRENT_STEAMCMD set "CURRENT_STEAMCMD=!STEAMCMD_EXE!"
if not defined STEAMCMD_EXE set "STEAMCMD_EXE=!CURRENT_STEAMCMD!"

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
) else (
    echo [+] updater.ini is up to date: !CURRENT_URL! [PC: !CURRENT_PC_ID!]
)

if "!NEEDS_WRITE!"=="1" (
    (
        echo ; Steam Auto-Updater client configuration
        echo [AutoUpdater]
        echo server_url=!SERVER_URL!
        echo api_key=!API_KEY!
        echo steamcmd_path=!STEAMCMD_EXE!
        echo pc_id=!CURRENT_PC_ID!
    ) > "!INI_FILE!"
    echo [+] updater.ini saved successfully!
)

rem 5. Desktop Shortcut
set "DESK_PUBLIC=%PUBLIC%\Desktop"
set "DESK_USER=%USERPROFILE%\Desktop"

set "SHORTCUT_EXISTS=0"
if exist "!DESK_PUBLIC!\Steam Auto-Updater.lnk" set "SHORTCUT_EXISTS=1"
if exist "!DESK_PUBLIC!\Steam Updater.lnk" set "SHORTCUT_EXISTS=1"
if exist "!DESK_USER!\Steam Auto-Updater.lnk" set "SHORTCUT_EXISTS=1"
if exist "!DESK_USER!\Steam Updater.lnk" set "SHORTCUT_EXISTS=1"

if "!SHORTCUT_EXISTS!"=="1" (
    echo [+] Desktop shortcut already exists.
) else (
    echo [*] Creating Desktop shortcut...
    set "TARGET_DESK=!DESK_USER!"
    if exist "!DESK_PUBLIC!" (
        2>nul (>>"!DESK_PUBLIC!\.chk" echo 1) && (
            del "!DESK_PUBLIC!\.chk" >nul 2>&1
            set "TARGET_DESK=!DESK_PUBLIC!"
        )
    )
    set "SHORTCUT_FILE=!TARGET_DESK!\Steam Auto-Updater.lnk"
    powershell -NoProfile -ExecutionPolicy Bypass -Command ^
        "$ws = New-Object -ComObject WScript.Shell;" ^
        "$s = $ws.CreateShortcut('!SHORTCUT_FILE!');" ^
        "$s.TargetPath = '!CLIENT_EXE!';" ^
        "$s.WorkingDirectory = '!TARGET_DIR!';" ^
        "$s.Description = 'Steam Auto-Updater';" ^
        "$s.Save();"
    
    if exist "!SHORTCUT_FILE!" (
        echo [+] Desktop shortcut created: !SHORTCUT_FILE!
    ) else (
        rem Fallback to user desktop if public failed
        set "SHORTCUT_FILE=!DESK_USER!\Steam Auto-Updater.lnk"
        powershell -NoProfile -ExecutionPolicy Bypass -Command ^
            "$ws = New-Object -ComObject WScript.Shell;" ^
            "$s = $ws.CreateShortcut('!SHORTCUT_FILE!');" ^
            "$s.TargetPath = '!CLIENT_EXE!';" ^
            "$s.WorkingDirectory = '!TARGET_DIR!';" ^
            "$s.Description = 'Steam Auto-Updater';" ^
            "$s.Save();"
        if exist "!SHORTCUT_FILE!" echo [+] Desktop shortcut created: !SHORTCUT_FILE!
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

