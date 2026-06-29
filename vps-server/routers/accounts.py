"""
routers/accounts.py — эндпоинты /accounts/*
"""
from __future__ import annotations

import logging
from typing import Literal, Optional

from fastapi import APIRouter, Depends, HTTPException
from pydantic import BaseModel, Field

from crypto import decrypt, load_key, DecryptionError
from db.database import get_conn
from routers.deps import verify_api_key

logger = logging.getLogger(__name__)
router = APIRouter(prefix="/accounts")


def get_enc_key() -> bytes:
    return load_key()

# --- Schemas ---

class AcquireRequest(BaseModel):
    pc_id: str = Field(min_length=1)
    app_id: str
    steam_id64: Optional[str] = None
    idempotency_key: str = Field(min_length=1)


class HeartbeatRequest(BaseModel):
    lease_token: str
    pc_id: str


class ReleaseRequest(BaseModel):
    lease_token: str
    pc_id: str
    result: Literal["success", "failed", "interrupted"]
    app_id: str
    error_msg: Optional[str] = None
    build_id_before: Optional[str] = None
    build_id_after: Optional[str] = None


# --- Helpers ---

def _acquire_one(conn: sqlite3.Connection, body: AcquireRequest) -> Optional[sqlite3.Row]:
    """Атомарный захват свободного аккаунта с retry на race condition."""
    for _ in range(5):
        if body.steam_id64:
            row = conn.execute(
                "SELECT id FROM accounts WHERE steam_id64=? AND status='free' LIMIT 1",
                (body.steam_id64,)
            ).fetchone()
        else:
            row = conn.execute(
                "SELECT id FROM accounts WHERE is_f2p=1 AND status='free' "
                "ORDER BY (last_heartbeat IS NULL) DESC, last_heartbeat ASC LIMIT 1"
            ).fetchone()

        if not row:
            return None  # реально свободных нет

        updated = conn.execute(
            """
            UPDATE accounts
            SET status='busy',
                lease_token=lower(hex(randomblob(16))),
                leased_to_pc=?,
                last_heartbeat=unixepoch()
            WHERE id=? AND status='free'
            RETURNING id, login, password_enc, lease_token
            """,
            (body.pc_id, row["id"])
        ).fetchone()

        if updated:
            return updated
        # кто-то опередил — пробуем следующий

    return None


def _replay_idempotent(conn: sqlite3.Connection, body: AcquireRequest) -> dict:
    """Вернуть результат предыдущего запроса с тем же idempotency_key."""
    rec = conn.execute(
        "SELECT status, account_id, lease_token, http_status "
        "FROM idempotency_keys WHERE key=? AND pc_id=?",
        (body.idempotency_key, body.pc_id)
    ).fetchone()

    if rec["status"] == "in_progress":
        raise HTTPException(
            status_code=409,
            detail="Запрос с этим ключом ещё обрабатывается",
            headers={"Retry-After": "1"}
        )

    if rec["status"] != "completed":
        raise HTTPException(
            status_code=rec["http_status"] or 409,
            detail="Предыдущая попытка завершилась ошибкой"
        )

    acc = conn.execute(
        "SELECT login, password_enc, lease_token FROM accounts WHERE id=?",
        (rec["account_id"],)
    ).fetchone()

    if not acc or acc["lease_token"] != rec["lease_token"]:
        raise HTTPException(status_code=410, detail="Лиз истёк, запросите новый аккаунт")

    try:
        password = decrypt(acc["password_enc"], get_enc_key())
    except DecryptionError as e:
        raise HTTPException(status_code=500, detail=f"Ошибка расшифровки: {e}")

    return {
        "account_id": rec["account_id"],
        "login": acc["login"],
        "password": password,
        "lease_token": rec["lease_token"],
    }


# --- Endpoints ---

@router.post("/acquire", dependencies=[Depends(verify_api_key)])
def acquire_account(body: AcquireRequest):
    conn: sqlite3.Connection = get_conn()
    try:
        # Атомарно застолбить idempotency_key
        cur = conn.execute(
            "INSERT OR IGNORE INTO idempotency_keys(key, pc_id, status) VALUES (?, ?, 'in_progress')",
            (body.idempotency_key, body.pc_id)
        )
        conn.commit()

        if cur.rowcount == 0:
            # Ключ уже существует — это ретрай
            return _replay_idempotent(conn, body)

        # Мы владельцы ключа — реально захватываем аккаунт
        try:
            updated = _acquire_one(conn, body)
            if not updated:
                conn.execute(
                    "UPDATE idempotency_keys SET status='failed', http_status=409 "
                    "WHERE key=? AND pc_id=?",
                    (body.idempotency_key, body.pc_id)
                )
                conn.commit()
                raise HTTPException(status_code=409, detail="Нет свободных аккаунтов")

            # Расшифровываем ДО коммита — при ошибке откатим
            try:
                password = decrypt(updated["password_enc"], get_enc_key())
            except DecryptionError as e:
                conn.rollback()
                logger.exception("Не удалось расшифровать пароль для account_id=%s", updated["id"])
                raise HTTPException(status_code=500, detail=f"Ошибка расшифровки: {e}")

            # Привязываем результат к ключу
            conn.execute(
                "UPDATE idempotency_keys "
                "SET status='completed', account_id=?, lease_token=?, http_status=200 "
                "WHERE key=? AND pc_id=?",
                (updated["id"], updated["lease_token"], body.idempotency_key, body.pc_id)
            )
            conn.commit()

            return {
                "account_id": updated["id"],
                "login": updated["login"],
                "password": password,
                "lease_token": updated["lease_token"],
            }

        except HTTPException:
            raise
        except Exception:
            conn.rollback()
            conn.execute(
                "UPDATE idempotency_keys SET status='failed', http_status=500 "
                "WHERE key=? AND pc_id=?",
                (body.idempotency_key, body.pc_id)
            )
            conn.commit()
            logger.exception("Ошибка захвата аккаунта")
            raise HTTPException(status_code=500, detail="Внутренняя ошибка сервера")

    finally:
        conn.close()


@router.post("/heartbeat", dependencies=[Depends(verify_api_key)])
def heartbeat(body: HeartbeatRequest):
    conn: sqlite3.Connection = get_conn()
    try:
        cur = conn.execute(
            """
            UPDATE accounts SET last_heartbeat=unixepoch()
            WHERE lease_token=? AND leased_to_pc=? AND status='busy'
            """,
            (body.lease_token, body.pc_id)
        )
        conn.commit()
        if cur.rowcount == 0:
            raise HTTPException(status_code=404, detail="Аренда не найдена")
        return {"ok": True}
    finally:
        conn.close()


@router.post("/release", dependencies=[Depends(verify_api_key)])
def release_account(body: ReleaseRequest):
    conn: sqlite3.Connection = get_conn()
    try:
        # C2: освобождаем ТОЛЬКО свою аренду (lease_token И leased_to_pc)
        row = conn.execute(
            "UPDATE accounts SET status='free', lease_token=NULL, leased_to_pc=NULL "
            "WHERE lease_token=? AND leased_to_pc=? RETURNING id",
            (body.lease_token, body.pc_id)
        ).fetchone()

        # Если аренда уже истекла (cleanup) — пробуем найти account_id по idempotency_keys
        if row:
            account_id = row["id"]
        else:
            idem = conn.execute(
                "SELECT account_id FROM idempotency_keys "
                "WHERE pc_id=? AND lease_token=? ORDER BY created_at DESC LIMIT 1",
                (body.pc_id, body.lease_token)
            ).fetchone()
            account_id = idem["account_id"] if idem else None

        started_at = None
        if account_id is not None:
            idem = conn.execute(
                "SELECT created_at FROM idempotency_keys WHERE account_id=? "
                "ORDER BY created_at DESC LIMIT 1",
                (account_id,)
            ).fetchone()
            started_at = idem["created_at"] if idem else None

        # C3: лог пишем ВСЕГДА, даже если аренда уже истекла
        if started_at is not None:
            conn.execute(
                "INSERT INTO update_log "
                "(pc_id, account_id, app_id, started_at, finished_at, result, error_msg, build_id_before, build_id_after) "
                "VALUES (?, ?, ?, ?, unixepoch(), ?, ?, ?, ?)",
                (body.pc_id, account_id, body.app_id, started_at,
                 body.result, body.error_msg, body.build_id_before, body.build_id_after)
            )
        else:
            conn.execute(
                "INSERT INTO update_log "
                "(pc_id, account_id, app_id, finished_at, result, error_msg, build_id_before, build_id_after) "
                "VALUES (?, ?, ?, unixepoch(), ?, ?, ?, ?)",
                (body.pc_id, account_id, body.app_id,
                 body.result, body.error_msg, body.build_id_before, body.build_id_after)
            )

        conn.commit()
        return {"ok": True, "released": bool(row)}
    finally:
        conn.close()


@router.get("/status", dependencies=[Depends(verify_api_key)])
def accounts_status():
    conn: sqlite3.Connection = get_conn()
    try:
        rows = conn.execute(
            "SELECT id, login, status, leased_to_pc, last_heartbeat, is_f2p FROM accounts"
        ).fetchall()
        return [
            {
                "id": r["id"],
                "login": r["login"],
                "status": r["status"],
                "leased_to_pc": r["leased_to_pc"],
                "last_heartbeat": r["last_heartbeat"],
                "is_f2p": bool(r["is_f2p"]),
            }
            for r in rows
        ]
    finally:
        conn.close()
