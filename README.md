# Steam Auto-Updater (для компьютерного клуба)

Обновляет и **устанавливает** free-to-play игры (CS2, PUBG, Apex, Dota 2, TF2 и т.д.)
на клубных ПК без ручного ввода логинов. Аккаунты выдаёт свой VPS-сервер по
системе аренды (lease), поэтому один аккаунт не выдаётся сразу на 50 машин и
сессии не выбивают друг друга.

```
[ПК клуба] steam_updater.exe ──HTTPS──> [VPS: FastAPI + SQLite]
      │                                   acquire → heartbeat → release
      └── steamcmd.exe ──> Steam CDN ──> C:\...\steamapps\common\<Game>
```

## Как это работает

1. Клиент находит корень Steam (`HKLM\SOFTWARE\WOW6432Node\Valve\Steam!InstallPath`)
   и все библиотеки из `libraryfolders.vdf`.
2. Если игра установлена — обновление идёт **на месте**: `force_install_dir` на
   существующую папку игры + `app_update <id> validate`. Скачивается только
   разница, а не вся игра.
3. Если игра не установлена — берётся библиотека с максимумом свободного места,
   создаётся `steamapps\common\<InstallDir>` и игра ставится с нуля.
4. Аккаунт берётся из пула: `POST /accounts/acquire` → lease-токен, heartbeat
   каждые 15 сек, `POST /accounts/release` в конце (в т.ч. при ошибке).
   Аварийное завершение подхватывается через `lease.json` при следующем старте.
5. После успеха `appmanifest_<appid>.acf` из папки steamcmd копируется в
   библиотеку клиента с исправленным `installdir`, чтобы Steam видел игру
   установленной и не предлагал качать заново.

## Сборка клиента

Нужен MinGW-w64 (gcc) в PATH:

```bat
cd client
mingw32-make
:: результат: client\dist\steam_updater.exe
```

Рядом с `.exe` должен лежать `updater.ini` (шаблон — `client_build/updater.ini`).

## Настройка клиента

`updater.ini`:

```ini
[AutoUpdater]
server_url=https://your-vps.example.com
api_key=PUT_YOUR_API_KEY_HERE
steamcmd_path=C:\ProgramData\svc-steam\steamcmd\steamcmd.exe
pc_id=PC-01
```

`pc_id` должен быть уникальным на каждой машине (удобно ставить имя ПК).

## Интерфейс

* Список = встроенный каталог F2P-игр + всё, что реально установлено на этом ПК.
* Галочки: установленные отмечены автоматически; отметь не установленную игру,
  чтобы поставить её с нуля.
* **Update ALL checked** — последовательная очередь по всем отмеченным играм
  (по одной за раз, чтобы не забивать канал и не плодить сессии).
* Прогресс-бар показывает этап: `logging in` → `preallocating` → `downloading`
  → `validating` → `committing`; в колонке Status пишется итог по каждой игре.

## Сервер

```bash
cd vps-server
cp .env.example .env      # STEAM_ENC_KEY, API_KEY
python manage_accounts.py gen-key
python manage_accounts.py bulk-add accounts.csv   # login,password,steam_id64,is_paid
uvicorn main:app --host 0.0.0.0 --port 8000
```

Обязательно поставь nginx + TLS перед uvicorn: пароли Steam ходят по этому
каналу.

## Почему пул аккаунтов, а не один логин на все ПК

Steam разрешает ограниченное число одновременных сессий на аккаунт (на практике
около четырёх), новая сессия выбивает старую. Поэтому сервер выдаёт каждому ПК
свободный аккаунт из пула и держит его занятым до конца обновления.

## Безопасность

* Держи `steamcmd` в `C:\ProgramData\svc-steam\` с ACL только для
  Administrators + SYSTEM.
* Пароли в базе шифруются (`crypto.py`, ключ `STEAM_ENC_KEY`).
* Runscript с паролем затирается сразу после старта steamcmd.
* Не коммить реальные `api_key` / пути в `updater.ini`.
