"""
routers/admin.py — встроенная веб-панель админа.

Сама страница — один статический HTML-файл (static/admin.html), всё общение —
через /admin/api/*. Никакого отдельного приложения ставить не надо: сервер уже
запущен, панель открывается в браузере по http://<ip-машины>:8000/

Авторизация — тот же STEAM_API_KEY (Bearer). Сам HTML отдаётся без ключа (в нём нет
данных), ключ вводится в браузере один раз и хранится в localStorage.
"""
from __future__ import annotations

import logging
import os
import sqlite3
from pathlib import Path
from typing import List, Optional

from fastapi import APIRouter, Depends, HTTPException
from fastapi.responses import FileResponse, PlainTextResponse
from pydantic import BaseModel, Field

from crypto import encrypt, load_key
from db.database import get_conn
from routers.deps import verify_api_key

logger = logging.getLogger(__name__)
router = APIRouter()

STATIC_DIR = Path(__file__).resolve().parent.parent / "static"
LOG_FILE = Path(os.environ.get("STEAM_LOG_FILE", "server.log"))

VALID_STATUSES = ("free", "busy", "bad", "disabled")


# --- Страница -------------------------------------------------------------

@router.get("/", include_in_schema=False)
@router.get("/admin", include_in_schema=False)
def admin_page():
    path = STATIC_DIR / "admin.html"
    if not path.exists():
        raise HTTPException(status_code=404, detail="admin.html не найден")
    return FileResponse(path, media_type="text/html; charset=utf-8")


# --- Схемы -----------------------------------------------------------------

class BulkAddRequest(BaseModel):
    # Сырой текст: по строке на аккаунт.
    # Поддерживается login:pass, login;pass, login,pass, login<tab>pass,
    # опционально третий столбец с steam_id64.
    text: str = Field(min_length=1)
    is_paid: bool = False
    skip_duplicates: bool = True


class StatusRequest(BaseModel):
    status: str


# --- Парсер пачки -----------------------------------------------------------

def _split_line(line: str) -> Optional[List[str]]:
    line = line.strip()
    if not line or line.startswith("#") or line.startswith(";"):
        return None
    # Отбрасываем шапку CSV, если кто-то вставил вместе с заголовком
    low = line.lower().replace(" ", "")
    if low.startswith("login,") or low.startswith("login:") or low.startswith("login;"):
        return None

    for sep in ("\t", ";", ",", ":", "|", " "):
        if sep in line:
            parts = [p.strip() for p in line.split(sep)]
            parts = [p for p in parts if p != ""]
            if len(parts) >= 2:
                return parts
    return None


# --- Эндпоинты -------------------------------------------------------------

@router.get("/admin/api/overview", dependencies=[Depends(verify_api_key)])
def overview():
    conn: sqlite3.Connection = get_conn()
    try:
        counts = {r["status"]: r["n"] for r in conn.execute(
            "SELECT status, COUNT(*) AS n FROM accounts GROUP BY status"
        ).fetchall()}

        accounts = [
            {
                "id": r["id"],
                "login": r["login"],
                "status": r["status"],
                "leased_to_pc": r["leased_to_pc"],
                "is_f2p": bool(r["is_f2p"]),
                "idle_sec": r["idle_sec"],
            }
            for r in conn.execute(
                "SELECT id, login, status, leased_to_pc, is_f2p, "
                "CASE WHEN last_heartbeat IS NULL THEN NULL "
                "     ELSE unixepoch() - last_heartbeat END AS idle_sec "
                "FROM accounts ORDER BY id"
            ).fetchall()
        ]

        # Активные ПК: кто сейчас держит аренду и как давно был heartbeat
        pcs = [
            {
                "pc_id": r["leased_to_pc"],
                "account_id": r["id"],
                "login": r["login"],
                "idle_sec": r["idle_sec"],
                "stale": (r["idle_sec"] is not None and r["idle_sec"] > 45),
            }
            for r in conn.execute(
                "SELECT id, login, leased_to_pc, "
                "CASE WHEN last_heartbeat IS NULL THEN NULL "
                "     ELSE unixepoch() - last_heartbeat END AS idle_sec "
                "FROM accounts WHERE status='busy' ORDER BY leased_to_pc"
            ).fetchall()
        ]

        # Сводка за сутки
        day = conn.execute(
            "SELECT result, COUNT(*) AS n FROM update_log "
            "WHERE unixepoch() - finished_at < 86400 GROUP BY result"
        ).fetchall()

        return {
            "counts": {
                "total": sum(counts.values()),
                "free": counts.get("free", 0),
                "busy": counts.get("busy", 0),
                "bad": counts.get("bad", 0),
                "disabled": counts.get("disabled", 0),
            },
            "accounts": accounts,
            "pcs": pcs,
            "last_24h": {r["result"]: r["n"] for r in day},
        }
    finally:
        conn.close()


@router.get("/admin/api/log", dependencies=[Depends(verify_api_key)])
def update_log(limit: int = 100, result: Optional[str] = None, pc_id: Optional[str] = None):
    limit = max(1, min(limit, 500))
    conn: sqlite3.Connection = get_conn()
    try:
        sql = (
            "SELECT l.id, l.pc_id, l.app_id, l.result, l.error_msg, "
            "l.build_id_before, l.build_id_after, l.started_at, l.finished_at, a.login "
            "FROM update_log l LEFT JOIN accounts a ON a.id = l.account_id WHERE 1=1"
        )
        params: list = []
        if result:
            sql += " AND l.result=?"
            params.append(result)
        if pc_id:
            sql += " AND l.pc_id=?"
            params.append(pc_id)
        sql += " ORDER BY l.id DESC LIMIT ?"
        params.append(limit)

        return [dict(r) for r in conn.execute(sql, params).fetchall()]
    finally:
        conn.close()


@router.get("/admin/api/server-log", response_class=PlainTextResponse,
            dependencies=[Depends(verify_api_key)])
def server_log(lines: int = 200):
    """Хвост файлового лога — чтобы видеть ошибки без доступа к консоли."""
    lines = max(10, min(lines, 2000))
    if not LOG_FILE.exists():
        return "Файловый лог пуст или не настроен (STEAM_LOG_FILE)."
    try:
        with LOG_FILE.open("r", encoding="utf-8", errors="replace") as f:
            tail = f.readlines()[-lines:]
        return "".join(tail)
    except OSError as e:
        return f"Не удалось прочитать лог: {e}"


@router.post("/admin/api/accounts/bulk", dependencies=[Depends(verify_api_key)])
def bulk_add(body: BulkAddRequest):
    """Пачковое добавление: вставил список — получил отчёт по каждой строке."""
    try:
        key = load_key()
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"Нет ключа шифрования: {e}")

    added = 0
    skipped: List[dict] = []
    conn: sqlite3.Connection = get_conn()
    try:
        for lineno, raw in enumerate(body.text.splitlines(), start=1):
            parts = _split_line(raw)
            if parts is None:
                if raw.strip():
                    skipped.append({"line": lineno, "text": raw.strip()[:60],
                                    "reason": "не разобралась (нужно login и пароль)"})
                continue

            login, password = parts[0], parts[1]
            steam_id64 = None
            if len(parts) >= 3 and parts[2].isdigit() and len(parts[2]) >= 15:
                steam_id64 = parts[2]

            if len(login) > 64 or len(password) > 128:
                skipped.append({"line": lineno, "text": login[:60],
                                "reason": "слишком длинный логин/пароль"})
                continue

            exists = conn.execute(
                "SELECT id FROM accounts WHERE login=? COLLATE NOCASE", (login,)
            ).fetchone()
            if exists:
                if body.skip_duplicates:
                    skipped.append({"line": lineno, "text": login,
                                    "reason": f"уже есть в базе (id={exists['id']})"})
                    continue
                conn.execute(
                    "UPDATE accounts SET password_enc=?, status='free', "
                    "lease_token=NULL, leased_to_pc=NULL WHERE id=?",
                    (encrypt(password, key), exists["id"])
                )
                added += 1
                continue

            try:
                conn.execute(
                    "INSERT INTO accounts(login, password_enc, steam_id64, is_f2p, status) "
                    "VALUES (?, ?, ?, ?, 'free')",
                    (login, encrypt(password, key), steam_id64, 0 if body.is_paid else 1)
                )
                added += 1
            except sqlite3.IntegrityError as e:
                skipped.append({"line": lineno, "text": login,
                                "reason": f"конфликт в БД: {e}"})

        conn.commit()
    finally:
        conn.close()

    logger.info("Пачковое добавление: +%d, пропущено %d", added, len(skipped))
    return {"added": added, "skipped": skipped}


@router.post("/admin/api/accounts/{account_id}/status", dependencies=[Depends(verify_api_key)])
def set_status(account_id: int, body: StatusRequest):
    if body.status not in VALID_STATUSES:
        raise HTTPException(status_code=400,
                            detail=f"Статус должен быть одним из: {', '.join(VALID_STATUSES)}")
    conn: sqlite3.Connection = get_conn()
    try:
        cur = conn.execute(
            "UPDATE accounts SET status=?, lease_token=NULL, leased_to_pc=NULL WHERE id=?",
            (body.status, account_id)
        )
        conn.commit()
        if cur.rowcount == 0:
            raise HTTPException(status_code=404, detail="Аккаунт не найден")
        return {"ok": True}
    finally:
        conn.close()


@router.post("/admin/api/accounts/revive-bad", dependencies=[Depends(verify_api_key)])
def revive_bad():
    """Вернуть в пул все аккаунты, помеченные 'bad' (после исправления паролей)."""
    conn: sqlite3.Connection = get_conn()
    try:
        cur = conn.execute(
            "UPDATE accounts SET status='free', lease_token=NULL, leased_to_pc=NULL "
            "WHERE status='bad'"
        )
        conn.commit()
        return {"revived": cur.rowcount}
    finally:
        conn.close()


@router.post("/admin/api/accounts/force-release", dependencies=[Depends(verify_api_key)])
def force_release():
    """Аварийно снять все аренды (если ПК вырубили из розетки и ждать 90с лень)."""
    conn: sqlite3.Connection = get_conn()
    try:
        cur = conn.execute(
            "UPDATE accounts SET status='free', lease_token=NULL, leased_to_pc=NULL "
            "WHERE status='busy'"
        )
        conn.commit()
        logger.warning("Админ принудительно снял %d аренд", cur.rowcount)
        return {"released": cur.rowcount}
    finally:
        conn.close()


@router.delete("/admin/api/accounts/{account_id}", dependencies=[Depends(verify_api_key)])
def delete_account(account_id: int):
    conn: sqlite3.Connection = get_conn()
    try:
        cur = conn.execute("DELETE FROM accounts WHERE id=?", (account_id,))
        conn.commit()
        if cur.rowcount == 0:
            raise HTTPException(status_code=404, detail="Аккаунт не найден")
        return {"ok": True}
    finally:
        conn.close()
