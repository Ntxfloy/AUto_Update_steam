"""
setup_env.py — one-time VPS setup:
  1. Generate STEAM_API_KEY and STEAM_ENC_KEY, write .env
  2. Initialize the SQLite DB (run schema.sql)
  3. Load Steam accounts from ACCOUNTS list
"""
import os, sys, secrets, base64, sqlite3

# ── Config ──────────────────────────────────────────────────────────────────
DB_PATH    = "/root/steam-club/steam_club.db"
SCHEMA_SQL = "/root/steam-club/db/schema.sql"
ENV_FILE   = "/root/steam-club/.env"

ACCOUNTS = [
    ("localvip1", "MIhvG5267"),
    ("localvip2", "MIhvG5267"),
    ("localvip3", "MIhvG5267"),
    ("localvip4", "MIhvG5267"),
    ("localvip5", "MIhvG5267"),
]

# ── Step 1: generate keys if .env missing ───────────────────────────────────
if not os.path.exists(ENV_FILE):
    api_key = secrets.token_urlsafe(32)
    enc_key = base64.b64encode(secrets.token_bytes(32)).decode()
    with open(ENV_FILE, "w") as f:
        f.write(f"STEAM_API_KEY={api_key}\n")
        f.write(f"STEAM_ENC_KEY={enc_key}\n")
        f.write(f"DB_PATH={DB_PATH}\n")
    print(f"[setup] .env created")
    print(f"[setup] STEAM_API_KEY={api_key}")
    print(f"[setup] STEAM_ENC_KEY={enc_key}")
else:
    print("[setup] .env already exists — skipping key generation")

# ── Load env ────────────────────────────────────────────────────────────────
from dotenv import load_dotenv
load_dotenv(ENV_FILE)
sys.path.insert(0, "/root/steam-club")

from crypto import encrypt, load_key

key = load_key()
db_path = os.environ["DB_PATH"]

# ── Step 2: init DB ─────────────────────────────────────────────────────────
with open(SCHEMA_SQL) as f:
    schema = f.read()

conn = sqlite3.connect(db_path)
conn.execute("PRAGMA journal_mode=WAL")
conn.execute("PRAGMA foreign_keys=ON")
conn.executescript(schema)
conn.commit()
print(f"[setup] DB initialized: {db_path}")

# ── Step 3: load accounts ───────────────────────────────────────────────────
loaded = 0
skipped = 0
for login, password in ACCOUNTS:
    enc = encrypt(password, key)
    try:
        conn.execute(
            "INSERT INTO accounts (login, password_enc, is_f2p, status) VALUES (?, ?, 0, 'free')",
            (login, enc)
        )
        loaded += 1
        print(f"[setup] Added account: {login}")
    except sqlite3.IntegrityError:
        skipped += 1
        print(f"[setup] Skipped (already exists): {login}")

conn.commit()
conn.close()
print(f"\n[setup] Done. Loaded: {loaded}, Skipped: {skipped}")
print("[setup] Run server: cd /root/steam-club && venv/bin/uvicorn main:app --host 0.0.0.0 --port 8000")
