# Steam Auto-Updater — Client Build Guide

## Requirements

- **MinGW-w64** (gcc for Windows)
  - Download: https://winlibs.com/ (choose Win64, UCRT, latest)
  - Or via MSYS2: `pacman -S mingw-w64-ucrt-x86_64-gcc`
- **Windows SDK** is NOT required — we use only Win32 headers bundled with MinGW

## Build

```cmd
cd client
make
```

Output: `client\steam_updater.exe`

Debug build:
```cmd
make debug
```

## First Run

1. Copy `steam_updater.exe` to the PC (e.g. `C:\steamcmd\updater\`)
2. Also copy `steamcmd.exe` to the same machine (e.g. `C:\steamcmd\steamcmd.exe`)
3. Run `steam_updater.exe` — it creates `updater.ini` in the same folder
4. Click **Settings** and fill in:
   - **Server URL**: `https://your-vps.com`
   - **API Key**: the Bearer token from VPS `.env`
   - **steamcmd.exe path**: full path to steamcmd.exe
   - **PC ID**: auto-detected from hostname, can override
5. Click **Refresh List** — all installed Steam games appear
6. Select a game → **Update Selected**

## Config file (`updater.ini`)

```ini
[AutoUpdater]
server_url=https://your-vps.com
api_key=YOUR_API_KEY_HERE
steamcmd_path=C:\steamcmd\steamcmd.exe
pc_id=PC-04
```

## Crash Recovery

If the updater crashes mid-update, `lease.json` is left on disk.
On next startup, the updater automatically sends `/release` to the VPS
and shows a warning dialog. No manual cleanup needed.

## VPS Setup (quick reference)

```bash
cd vps-server
pip install -r requirements.txt
cp .env.example .env
# Edit .env: set STEAM_API_KEY and STEAM_ENC_KEY
uvicorn main:app --host 0.0.0.0 --port 8000

# Add accounts:
python manage_accounts.py gen-key          # generate STEAM_ENC_KEY
python manage_accounts.py add login pass steam_id64 --paid
python manage_accounts.py list

# Deploy as systemd service:
sudo cp deploy/steam-club.service /etc/systemd/system/
sudo systemctl enable --now steam-club
```

## Architecture

```
[PC-01..49]  ─── HTTPS ───>  [VPS: FastAPI + SQLite WAL]
     │                              │
     │  1. acquire (lease_token)    │  atomic UPDATE → busy
     │  2. steamcmd runs            │
     │  3. heartbeat every 30s  ────>  last_heartbeat refresh
     │  4. release (result)     ────>  status=free + log entry
     │
  lease.json (crash safety)
  Job Object  (kill SteamCMD on parent crash)
  Jitter 1-15s (prevent 49 PCs hitting server simultaneously)
```
