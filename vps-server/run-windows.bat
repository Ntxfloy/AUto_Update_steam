@echo off
chcp 65001 >nul
setlocal
title Steam Club Manager - local server
cd /d "%~dp0"

echo ============================================================
echo   Steam Club Manager - local server (Windows)
echo ============================================================
echo.

where python >nul 2>&1
if errorlevel 1 (
    echo [ERROR] Python not found in PATH.
    echo         Install Python 3.10+ from python.org and tick "Add to PATH".
    pause
    exit /b 1
)

if not exist ".venv\Scripts\python.exe" (
    echo [1/4] Creating virtual environment .venv ...
    python -m venv .venv
    if errorlevel 1 ( echo [ERROR] venv creation failed. & pause & exit /b 1 )
) else (
    echo [1/4] Virtual environment found.
)

echo [2/4] Installing dependencies ...
".venv\Scripts\python.exe" -m pip install --disable-pip-version-check -q --upgrade pip
".venv\Scripts\python.exe" -m pip install --disable-pip-version-check -q -r requirements.txt
if errorlevel 1 ( echo [ERROR] pip install failed. & pause & exit /b 1 )

if not exist ".env" (
    echo.
    echo [3/4] No .env found. Generating keys ...
    rem Do NOT use "for /f" with a quoted python -c here: cmd mangles the inner
    rem quotes, the variables end up empty and .env is written as "ECHO is off".
    rem One direct python call writes the file itself and prints the key.
    ".venv\Scripts\python.exe" -c "import secrets; k1=secrets.token_urlsafe(32); k2=secrets.token_bytes(32).hex(); open('.env','w',encoding='utf-8').write('STEAM_API_KEY='+k1+'\nSTEAM_ENC_KEY='+k2+'\nSTEAM_LOG_FILE=server.log\n'); print(); print('  API KEY FOR CLIENTS AND THE ADMIN PANEL:'); print('  '+k1); print(); print('  Save it. Put it into updater.ini on every PC as api_key='); print('  Back up .env: without STEAM_ENC_KEY the stored passwords are unreadable.')"
    if errorlevel 1 ( echo [ERROR] could not generate .env & pause & exit /b 1 )
    echo.
    pause
) else (
    echo [3/4] .env found, keeping existing keys.
)

rem Verify the generated .env is actually usable before starting the server.
".venv\Scripts\python.exe" -c "import sys,pathlib; t=pathlib.Path('.env').read_text(encoding='utf-8',errors='ignore'); ok=all(any(l.startswith(k+'=') and len(l.split('=',1)[1].strip())>10 for l in t.splitlines()) for k in ('STEAM_API_KEY','STEAM_ENC_KEY')); sys.exit(0 if ok else 1)"
if errorlevel 1 (
    echo.
    echo [ERROR] .env is empty or broken. Delete vps-server\.env and run this file again.
    pause
    exit /b 1
)

echo [4/4] Opening firewall port 8000 (private networks) ...
netsh advfirewall firewall show rule name="SteamClubManager" >nul 2>&1
if errorlevel 1 (
    netsh advfirewall firewall add rule name="SteamClubManager" dir=in action=allow ^
        protocol=TCP localport=8000 profile=private,domain >nul 2>&1
    if errorlevel 1 echo    [warn] could not add the rule - run this file as administrator once.
)

echo.
echo Starting the server. Close this window to stop it.
echo The panel URL and the LAN address are printed below.
echo.
".venv\Scripts\python.exe" -m uvicorn main:app --host 0.0.0.0 --port 8000

echo.
echo Server stopped.
pause
