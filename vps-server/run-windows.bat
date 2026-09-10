@echo off
REM ===================================================================
REM  Steam Club Account Manager - запуск на Windows (локальная сеть клуба)
REM  Запускать из папки vps-server. Первый запуск сам создаст venv.
REM ===================================================================
chcp 65001 >nul
setlocal
cd /d "%~dp0"
title Steam Club Account Manager

where python >nul 2>&1
if errorlevel 1 (
    echo [ОШИБКА] Python не найден в PATH.
    echo Установите Python 3.11+ с python.org и отметьте "Add python.exe to PATH".
    pause & exit /b 1
)

if not exist ".venv\Scripts\python.exe" (
    echo [1/3] Создаю виртуальное окружение...
    python -m venv .venv || (echo Не удалось создать venv & pause & exit /b 1)
    echo [2/3] Устанавливаю зависимости...
    ".venv\Scripts\python.exe" -m pip install --upgrade pip >nul
    ".venv\Scripts\python.exe" -m pip install -r requirements.txt || (echo Ошибка pip & pause & exit /b 1)
)

if not exist ".env" (
    echo.
    echo [ВНИМАНИЕ] Файла .env нет. Генерирую ключи...
    for /f "delims=" %%i in ('".venv\Scripts\python.exe" -c "import secrets;print(secrets.token_urlsafe(32))"') do set "NEWAPI=%%i"
    for /f "delims=" %%i in ('".venv\Scripts\python.exe" -c "import secrets;print(secrets.token_bytes(32).hex())"') do set "NEWENC=%%i"
    >  .env echo STEAM_API_KEY=%NEWAPI%
    >> .env echo STEAM_ENC_KEY=%NEWENC%
    >> .env echo STEAM_LOG_FILE=server.log
    echo.
    echo   Создан .env. КЛЮЧ ДЛЯ КЛИЕНТОВ И ПАНЕЛИ АДМИНА:
    echo   %NEWAPI%
    echo.
    echo   Сохраните его! Впишите в updater.ini на каждом ПК как api_key=
    echo   БЕЗ STEAM_ENC_KEY из .env пароли в БД будут нечитаемы - сделайте бэкап .env
    echo.
    pause
)

REM Правило брандмауэра для локальной сети (тихо падает без админа)
netsh advfirewall firewall show rule name="SteamClubManager" >nul 2>&1
if errorlevel 1 (
    netsh advfirewall firewall add rule name="SteamClubManager" dir=in action=allow ^
        protocol=TCP localport=8000 profile=private,domain >nul 2>&1
    if errorlevel 1 (
        echo [ИНФО] Не смог добавить правило брандмауэра ^(нужен запуск от админа^).
        echo        Если клиенты не видят сервер - разрешите порт 8000 вручную.
    ) else (
        echo [ОК] Порт 8000 открыт для локальной сети.
    )
)

echo.
echo [3/3] Запускаю сервер. Не закрывайте это окно.
echo Адрес панели будет напечатан ниже. Остановка - Ctrl+C.
echo.
":venv" 2>nul
".venv\Scripts\python.exe" -m uvicorn main:app --host 0.0.0.0 --port 8000

echo.
echo Сервер остановлен. Лог сохранён в server.log
pause
