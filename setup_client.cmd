@echo off
chcp 65001 >nul
setlocal EnableDelayedExpansion

echo ===================================================
echo   Steam Auto-Updater - Client Auto-Setup
echo ===================================================

:: 1. Choose Target Drive (Prefer D:, fallback to C:)
if exist "D:\" (
    set "TARGET_DIR=D:\SteamUpdater"
) else (
    set "TARGET_DIR=C:\SteamUpdater"
)

if not exist "!TARGET_DIR!" mkdir "!TARGET_DIR!"
echo [*] Target folder: !TARGET_DIR!

:: 2. Find or Install SteamCMD
set "STEAMCMD_EXE="

:: Check local folder first
if exist "!TARGET_DIR!\steamcmd\steamcmd.exe" (
    set "STEAMCMD_EXE=!TARGET_DIR!\steamcmd\steamcmd.exe"
) else if exist "D:\SteamCMD\steamcmd.exe" (
    set "STEAMCMD_EXE=D:\SteamCMD\steamcmd.exe"
) else if exist "C:\SteamCMD\steamcmd.exe" (
    set "STEAMCMD_EXE=C:\SteamCMD\steamcmd.exe"
) else if exist "d:\Ayder_dontdelete\steamcmd\steamcmd.exe" (
    set "STEAMCMD_EXE=d:\Ayder_dontdelete\steamcmd\steamcmd.exe"
)

:: Check WinGet package paths
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

:: Check PATH
if not defined STEAMCMD_EXE (
    for /f "delims=" %%I in ('where steamcmd.exe 2^>nul') do (
        if exist "%%I" set "STEAMCMD_EXE=%%I"
    )
)

:: If not found anywhere, download official portable SteamCMD (770 KB)
if not defined STEAMCMD_EXE (
    echo [*] steamcmd.exe not found. Downloading official SteamCMD...
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

:: 3. Download or update steam_updater.exe
set "CLIENT_EXE=!TARGET_DIR!\steam_updater.exe"
set "FORCE_UPDATE=0"
if /i "%1"=="--update" set "FORCE_UPDATE=1"
if /i "%2"=="--update" set "FORCE_UPDATE=1"

if exist "!CLIENT_EXE!" (
    if "!FORCE_UPDATE!"=="1" (
        echo [*] Force update requested. Re-downloading steam_updater.exe...
        powershell -NoProfile -ExecutionPolicy Bypass -Command ^
            "$ProgressPreference = 'SilentlyContinue';" ^
            "[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12;" ^
            "Invoke-WebRequest -Uri 'https://raw.githubusercontent.com/Ntxfloy/AUto_Update_steam/feat/dark-ui-and-layout-fixes/client_build/steam_updater.exe' -OutFile '!CLIENT_EXE!' -UseBasicParsing;"
        echo [+] steam_updater.exe updated.
    ) else (
        echo [+] steam_updater.exe already installed.
    )
) else (
    echo [*] Downloading steam_updater.exe...
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

:: 4. Generate updater.ini if not present
set "INI_FILE=!TARGET_DIR!\updater.ini"
if exist "!INI_FILE!" (
    echo [+] updater.ini already exists.
) else (
    echo [*] Generating updater.ini...
    (
        echo ; Steam Auto-Updater client configuration
        echo [AutoUpdater]
        echo server_url=http://192.168.20.149:8000
        echo api_key=zOTHOQY4gcdx6xSg5RzT-G-u9xJJABivqXjrgz_IlLA
        echo steamcmd_path=!STEAMCMD_EXE!
        echo pc_id=%COMPUTERNAME%
    ) > "!INI_FILE!"
    echo [+] updater.ini generated for PC: %COMPUTERNAME%.
)

echo ===================================================
echo   Setup Complete! Everything is ready in:
echo   !TARGET_DIR!
echo ===================================================

:: Optional launch flag (--run or /run)
if /i "%1"=="--run" (
    echo [*] Launching Steam Auto-Updater...
    start "" /d "!TARGET_DIR!" "!CLIENT_EXE!"
) else if /i "%2"=="--run" (
    echo [*] Launching Steam Auto-Updater...
    start "" /d "!TARGET_DIR!" "!CLIENT_EXE!"
)
