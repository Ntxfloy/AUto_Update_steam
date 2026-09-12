"""
routers/accounts.py — эндпоинты /accounts/*
"""
from __future__ import annotations

import logging
import sqlite3
from typing import Literal, Optional

from fastapi import APIRouter, Depends, HTTPException
from pydantic import BaseModel, Field

from crypto import decrypt, load_key, DecryptionError
from db.database import get_conn
from routers.deps import verify_api_key

logger = logging.getLogger(__name__)
router = APIRouter(prefix="/accounts")

# Клиент присылает человекочитаемый error_msg из steamcmd_result_name().
# Эти ошибки означают, что непригоден САМ АККАУНТ (неверный пароль,
# включён Steam Guard, бан) — его нельзя выдавать следующему ПК.
#
# ВАЖНО: "no license" здесь быть НЕ ДОЛЖНО. Это не поломка аккаунта,
# а факт о конкретной игре: аккаунт ею просто не владеет. Раньше один
# такой тайтл (Call of Duty HQ) проходил по всему пулу и выжигал его целиком.
BAD_ACCOUNT_MARKERS = (
    "authentication failure",
    "login failure",
    "invalid password",
    "steam guard",
)

# Ошибки вида «этот аккаунт не владеет этой игрой».
# Аккаунт остаётся рабочим, запоминаем только пару (аккаунт, app_id).
NO_LICENSE_MARKERS = (
    "no license",
    "no subscription",
)


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

def _is_bad_account_error(error_msg: Optional[str]) -> bool:
    if not error_msg:
        return False
    low = error_msg.lower()
    return any(m in low for m in BAD_ACCOUNT_MARKERS)


def _is_no_license_error(error_msg: Optional[str]) -> bool:
    if not error_msg:
        return False
    low = error_msg.lower()
    return any(m in low for m in NO_LICENSE_MARKERS)


def _acquire_one(conn: sqlite3.Connection, body: AcquireRequest) -> Optional[sqlite3.Row]:
    """Атомарный захват свободного аккаунта с retry на race condition.

    Аккаунты, про которые уже известно, что у них нет лицензии на эту
    игру, в выборку не попадают — но остаются доступны для всех остальных игр.
    """
    for _ in range(5):
        if body.steam_id64:
            row = conn.execute(
                "SELECT id FROM accounts "
                "WHERE steam_id64=? AND status='free' "
                "  AND id NOT IN (SELECT account_id FROM account_app_denied WHERE app_id=?) "
                "LIMIT 1",
                (body.steam_id64, body.app_id)
            ).fetchone()
        else:
            row = conn.execute(
                "SELECT id FROM accounts "
                "WHERE is_f2p=1 AND status='free' "
                "  AND id NOT IN (SELECT account_id FROM account_app_denied WHERE app_id=?) "
                "ORDER BY (last_heartbeat IS NULL) DESC, last_heartbeat ASC LIMIT 1",
                (body.app_id,)
            ).fetchone()

        if not row:
            return None  # реально свободных (и пригодных для этой игры) нет

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


def _no_license_pool_exhausted(conn: sqlite3.Connection, app_id: str) -> bool:
    """True, если свободные аккаунты есть, но все они без лицензии на эту игру.

    В этом случае ретраить бессмысленно: игрой не владеет никто в пуле.
    """
    free_total = conn.execute(
        "SELECT COUNT(*) AS n FROM accounts WHERE status='free' AND is_f2p=1"
    ).fetchone()["n"]
    if free_total == 0:
        return False
    denied = conn.execute(
        "SELECT COUNT(*) AS n FROM accounts a "
        "WHERE a.status='free' AND a.is_f2p=1 "
        "  AND a.id IN (SELECT account_id FROM account_app_denied WHERE app_id=?)",
        (app_id,)
    ).fetchone()["n"]
    return denied >= free_total


def _replay_idempotent(conn: sqlite3.Connection, body: AcquireRequest) -> dict:
    """Вернуть результат предыдущего запроса с тем же idempotency_key."""
    rec = conn.execute(
        "SELECT status, account_id, lease_token, http_status "
        "FROM idempotency_keys WHERE key=? AND pc_id=?",
        (body.idempotency_key, body.pc_id)
    ).fetchone()

    # Запись мог удалить сборщик мусора (>24ч) между INSERT OR IGNORE и чтением.
    # Без этой проверки здесь был TypeError → 500 вместо внятного ответа.
    if rec is None:
        raise HTTPException(status_code=409, detail="Ключ идемпотентности не найден, повторите запрос")

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
                # Различаем две разные ситуации:
                #  409 — все заняты, имеет смысл подождать и повторить;
                #  410 — игрой не владеет ни один аккаунт пула, ретраить бессмысленно.
                no_license_everywhere = _no_license_pool_exhausted(conn, body.app_id)
                http_status = 410 if no_license_everywhere else 409
                detail = (
                    f"Ни один аккаунт пула не владеет игрой {body.app_id} — повторы бессмысленны"
                    if no_license_everywhere else "Нет свободных аккаунтов"
                )
                if no_license_everywhere:
                    logger.warning(
                        "app_id=%s: в пуле нет ни одного аккаунта с лицензией (pc_id=%s)",
                        body.app_id, body.pc_id
                    )
                conn.execute(
                    "UPDATE idempotency_keys SET status='failed', http_status=? "
                    "WHERE key=? AND pc_id=?",
                    (http_status, body.idempotency_key, body.pc_id)
                )
                conn.commit()
                raise HTTPException(status_code=http_status, detail=detail)

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
        failed = (body.result == "failed")
        no_license = failed and _is_no_license_error(body.error_msg)
        # Нет лицензии — аккаунт исправен, проблема только в этой игре.
        bad_credentials = failed and not no_license and _is_bad_account_error(body.error_msg)

        # Освобождаем ТОЛЬКО свою аренду (lease_token И leased_to_pc).
        # Если аккаунт не пустил по самой авторизации — он не возвращается
        # в пул, а помечается 'bad', чтобы следующий acquire выдал другой логин.
        new_status = "bad" if bad_credentials else "free"
        row = conn.execute(
            "UPDATE accounts SET status=?, lease_token=NULL, leased_to_pc=NULL "
            "WHERE lease_token=? AND leased_to_pc=? RETURNING id",
            (new_status, body.lease_token, body.pc_id)
        ).fetchone()

        # Если аренда уже истекла (cleanup) — ищем account_id по idempotency_keys
        if row:
            account_id = row["id"]
        else:
            idem = conn.execute(
                "SELECT account_id FROM idempotency_keys "
                "WHERE pc_id=? AND lease_token=? ORDER BY created_at DESC LIMIT 1",
                (body.pc_id, body.lease_token)
            ).fetchone()
            account_id = idem["account_id"] if idem else None

            # Аренда истекла, но аккаунт всё равно битый — помечаем его,
            # но только если его уже не забрал другой ПК.
            if bad_credentials and account_id is not None:
                conn.execute(
                    "UPDATE accounts SET status='bad' WHERE id=? AND status='free'",
                    (account_id,)
                )

        # Запоминаем пару (аккаунт, игра): больше не выдаём этот аккаунт
        # под эту игру, но сам аккаунт остаётся в пуле для всего остального.
        if no_license and account_id is not None:
            conn.execute(
                "INSERT OR IGNORE INTO account_app_denied(account_id, app_id, reason) "
                "VALUES (?, ?, ?)",
                (account_id, body.app_id, (body.error_msg or "no license")[:200])
            )
            logger.info(
                "Аккаунт id=%s не владеет app_id=%s — исключён только для этой игры "
                "(pc_id=%s, статус остался free)",
                account_id, body.app_id, body.pc_id
            )

        if bad_credentials and account_id is not None:
            logger.warning(
                "Аккаунт id=%s помечен 'bad' (pc_id=%s): %s",
                account_id, body.pc_id, body.error_msg
            )

        started_at = None
        if account_id is not None:
            idem = conn.execute(
                "SELECT created_at FROM idempotency_keys WHERE account_id=? "
                "ORDER BY created_at DESC LIMIT 1",
                (account_id,)
            ).fetchone()
            started_at = idem["created_at"] if idem else None

        # Лог пишем ВСЕГДА, даже если аренда уже истекла
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
        return {
            "ok": True,
            "released": bool(row),
            "account_disabled": bad_credentials,
            "app_denied": no_license,
        }
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
                "status": r["status"],
                "login": r["login"],
                "leased_to_pc": r["leased_to_pc"],
                "last_heartbeat": r["last_heartbeat"],
                "is_f2p": bool(r["is_f2p"]),
            }
            for r in rows
        ]
    finally:
        conn.close()
