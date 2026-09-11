"""
routers/clientlogs.py — приём и просмотр подробных логов клиентов.

Клиент (updater.exe) пачками шлёт сюда свои строки лога:
    POST /logs/ingest
    {"pc_id": "PC-07", "session_id": "A1B2C3",
     "lines": [{"ts": "...", "level": "INFO", "module": "worker",
                "app_id": "570", "thread": 1234, "text": "..."}]}

Храним в двух местах:
  * таблица client_logs в SQLite — для фильтров и поиска в панели;
  * плоский файл logs/<pc_id>/<дата>.log — чтобы можно было грепать руками
    и отдавать целиком кнопкой «Скачать».
"""
import logging
import os
import re
import sqlite3
from datetime import datetime
from typing import List, Optional

from fastapi import APIRouter, Depends, Query
from fastapi.responses import PlainTextResponse
from pydantic import BaseModel, Field

from db.database import get_conn
from routers.deps import verify_api_key

router = APIRouter()
logger = logging.getLogger(__name__)

LOG_DIR = os.environ.get("STEAM_CLIENT_LOG_DIR", "logs")
# Сколько строк держим в базе. Дальше режем самые старые, чтобы SQLite
# не разросся до гигабайтов за неделю тестов на 50 машинах.
MAX_ROWS = int(os.environ.get("STEAM_CLIENT_LOG_MAX_ROWS", "400000"))

_SAFE = re.compile(r"[^A-Za-z0-9_.-]+")
_schema_ready = False


def _safe_name(value: str) -> str:
    return _SAFE.sub("_", (value or "unknown"))[:64] or "unknown"


def _ensure_schema(conn: sqlite3.Connection) -> None:
    global _schema_ready
    if _schema_ready:
        return
    conn.executescript(
        """
        CREATE TABLE IF NOT EXISTS client_logs (
            id         INTEGER PRIMARY KEY AUTOINCREMENT,
            pc_id      TEXT NOT NULL,
            session_id TEXT,
            ts         TEXT,
            level      TEXT,
            module     TEXT,
            app_id     TEXT,
            thread     INTEGER,
            text       TEXT,
            received_at INTEGER DEFAULT (unixepoch())
        );
        CREATE INDEX IF NOT EXISTS idx_client_logs_pc      ON client_logs(pc_id, id DESC);
        CREATE INDEX IF NOT EXISTS idx_client_logs_session ON client_logs(session_id, id DESC);
        CREATE INDEX IF NOT EXISTS idx_client_logs_level   ON client_logs(level, id DESC);
        CREATE INDEX IF NOT EXISTS idx_client_logs_app     ON client_logs(app_id, id DESC);
        """
    )
    conn.commit()
    _schema_ready = True


class LogLine(BaseModel):
    ts: Optional[str] = None
    level: Optional[str] = "INFO"
    module: Optional[str] = "app"
    app_id: Optional[str] = ""
    thread: Optional[int] = 0
    text: str = ""


class LogBatch(BaseModel):
    pc_id: str = Field(..., max_length=64)
    session_id: Optional[str] = Field(None, max_length=64)
    lines: List[LogLine] = []


def _write_flat_file(batch: LogBatch) -> None:
    """Дублируем строки в logs/<pc_id>/<дата>.log."""
    day = datetime.now().strftime("%Y-%m-%d")
    folder = os.path.join(LOG_DIR, _safe_name(batch.pc_id))
    try:
        os.makedirs(folder, exist_ok=True)
        path = os.path.join(folder, f"{day}.log")
        with open(path, "a", encoding="utf-8", errors="replace") as fh:
            for ln in batch.lines:
                fh.write(
                    f"{ln.ts or ''} [{(ln.level or ''):5}] [{(ln.module or ''):8}] "
                    f"[sess:{batch.session_id or '-'}]"
                    f"{' app:' + ln.app_id if ln.app_id else ''} {ln.text}\n"
                )
    except OSError as exc:
        logger.warning("Не удалось записать файл лога клиента: %s", exc)


def _trim(conn: sqlite3.Connection) -> None:
    row = conn.execute("SELECT COUNT(*) AS c FROM client_logs").fetchone()
    if row and row["c"] > MAX_ROWS:
        conn.execute(
            "DELETE FROM client_logs WHERE id IN ("
            "  SELECT id FROM client_logs ORDER BY id ASC LIMIT ?"
            ")",
            (row["c"] - MAX_ROWS,),
        )
        conn.commit()


@router.post("/logs/ingest", dependencies=[Depends(verify_api_key)])
def ingest(batch: LogBatch):
    """Принять пачку строк от клиента."""
    if not batch.lines:
        return {"accepted": 0}

    conn: sqlite3.Connection = get_conn()
    try:
        _ensure_schema(conn)
        conn.executemany(
            "INSERT INTO client_logs (pc_id, session_id, ts, level, module, app_id, thread, text) "
            "VALUES (?,?,?,?,?,?,?,?)",
            [
                (
                    batch.pc_id,
                    batch.session_id,
                    ln.ts,
                    (ln.level or "INFO").upper()[:8],
                    (ln.module or "app")[:24],
                    (ln.app_id or "")[:16],
                    ln.thread or 0,
                    ln.text[:2000],
                )
                for ln in batch.lines
            ],
        )
        conn.commit()
        _trim(conn)
    finally:
        conn.close()

    _write_flat_file(batch)
    return {"accepted": len(batch.lines)}


@router.get("/admin/api/client-logs", dependencies=[Depends(verify_api_key)])
def list_client_logs(
    pc_id: Optional[str] = None,
    session_id: Optional[str] = None,
    level: Optional[str] = None,
    app_id: Optional[str] = None,
    q: Optional[str] = None,
    after_id: int = 0,
    limit: int = Query(300, ge=1, le=2000),
):
    """Лента логов с фильтрами. after_id позволяет делать живой хвост."""
    where, params = [], []
    if pc_id:
        where.append("pc_id = ?")
        params.append(pc_id)
    if session_id:
        where.append("session_id = ?")
        params.append(session_id)
    if app_id:
        where.append("app_id = ?")
        params.append(app_id)
    if level:
        levels = {"DEBUG": 0, "INFO": 1, "WARN": 2, "ERROR": 3}
        wanted = [k for k, v in levels.items() if v >= levels.get(level.upper(), 0)]
        where.append("level IN (%s)" % ",".join("?" * len(wanted)))
        params.extend(wanted)
    if q:
        where.append("text LIKE ?")
        params.append(f"%{q}%")
    if after_id:
        where.append("id > ?")
        params.append(after_id)

    sql = "SELECT * FROM client_logs"
    if where:
        sql += " WHERE " + " AND ".join(where)
    # after_id — это хвост, его читаем по возрастанию; иначе показываем
    # последние строки.
    sql += " ORDER BY id ASC LIMIT ?" if after_id else " ORDER BY id DESC LIMIT ?"
    params.append(limit)

    conn: sqlite3.Connection = get_conn()
    try:
        _ensure_schema(conn)
        rows = [dict(r) for r in conn.execute(sql, params).fetchall()]
    finally:
        conn.close()

    if not after_id:
        rows.reverse()
    return rows


@router.get("/admin/api/client-logs/sessions", dependencies=[Depends(verify_api_key)])
def list_sessions(limit: int = Query(50, ge=1, le=500)):
    """Последние сессии клиентов: какой ПК, когда, сколько строк, сколько ошибок."""
    conn: sqlite3.Connection = get_conn()
    try:
        _ensure_schema(conn)
        rows = conn.execute(
            """
            SELECT session_id, pc_id,
                   COUNT(*) AS lines,
                   SUM(CASE WHEN level='ERROR' THEN 1 ELSE 0 END) AS errors,
                   SUM(CASE WHEN level='WARN'  THEN 1 ELSE 0 END) AS warnings,
                   MIN(ts) AS started_at, MAX(ts) AS last_at,
                   MAX(id) AS last_id
            FROM client_logs
            GROUP BY session_id, pc_id
            ORDER BY last_id DESC
            LIMIT ?
            """,
            (limit,),
        ).fetchall()
        return [dict(r) for r in rows]
    finally:
        conn.close()


@router.get("/admin/api/client-logs/pcs", dependencies=[Depends(verify_api_key)])
def list_pcs():
    conn: sqlite3.Connection = get_conn()
    try:
        _ensure_schema(conn)
        rows = conn.execute(
            "SELECT pc_id, COUNT(*) AS lines, MAX(id) AS last_id "
            "FROM client_logs GROUP BY pc_id ORDER BY last_id DESC"
        ).fetchall()
        return [dict(r) for r in rows]
    finally:
        conn.close()


@router.get(
    "/admin/api/client-logs/download",
    dependencies=[Depends(verify_api_key)],
    response_class=PlainTextResponse,
)
def download(session_id: Optional[str] = None, pc_id: Optional[str] = None):
    """Отдать всю сессию (или весь ПК) одним текстовым файлом."""
    where, params = [], []
    if session_id:
        where.append("session_id = ?")
        params.append(session_id)
    if pc_id:
        where.append("pc_id = ?")
        params.append(pc_id)
    sql = "SELECT * FROM client_logs"
    if where:
        sql += " WHERE " + " AND ".join(where)
    sql += " ORDER BY id ASC LIMIT 200000"

    conn: sqlite3.Connection = get_conn()
    try:
        _ensure_schema(conn)
        rows = conn.execute(sql, params).fetchall()
    finally:
        conn.close()

    body = "\n".join(
        f"{r['ts']} [{r['level']:5}] [{r['module']:8}] [{r['pc_id']}]"
        f"{' app:' + r['app_id'] if r['app_id'] else ''} {r['text']}"
        for r in rows
    )
    return PlainTextResponse(body or "(пусто)")


@router.delete("/admin/api/client-logs", dependencies=[Depends(verify_api_key)])
def purge(pc_id: Optional[str] = None, session_id: Optional[str] = None):
    """Очистка: весь лог, один ПК или одна сессия."""
    where, params = [], []
    if pc_id:
        where.append("pc_id = ?")
        params.append(pc_id)
    if session_id:
        where.append("session_id = ?")
        params.append(session_id)
    sql = "DELETE FROM client_logs"
    if where:
        sql += " WHERE " + " AND ".join(where)

    conn: sqlite3.Connection = get_conn()
    try:
        _ensure_schema(conn)
        cur = conn.execute(sql, params)
        conn.commit()
        return {"deleted": cur.rowcount}
    finally:
        conn.close()
