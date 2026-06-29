"""
routers/log.py — /log и /admin/*
"""
import sqlite3

from fastapi import APIRouter, Depends

from db.database import get_conn
from routers.deps import verify_api_key

router = APIRouter()


@router.get("/log", dependencies=[Depends(verify_api_key)])
def get_log():
    conn: sqlite3.Connection = get_conn()
    try:
        rows = conn.execute(
            """
            SELECT id, pc_id, app_id, result, started_at, finished_at,
                   build_id_before, build_id_after, error_msg
            FROM update_log
            ORDER BY id DESC
            LIMIT 200
            """
        ).fetchall()
        return [dict(r) for r in rows]
    finally:
        conn.close()


@router.post("/admin/cleanup-stale", dependencies=[Depends(verify_api_key)])
def cleanup_stale():
    conn: sqlite3.Connection = get_conn()
    try:
        cur = conn.execute(
            """
            UPDATE accounts SET status='free', lease_token=NULL, leased_to_pc=NULL
            WHERE status='busy' AND (unixepoch() - last_heartbeat) > 90
            """
        )
        conn.commit()
        return {"freed": cur.rowcount}
    finally:
        conn.close()
