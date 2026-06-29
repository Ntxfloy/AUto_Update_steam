"""
main.py — FastAPI приложение для управления Steam-аккаунтами компьютерного клуба.
"""
import asyncio
import logging
from contextlib import asynccontextmanager

from dotenv import load_dotenv
load_dotenv()

from fastapi import FastAPI
from fastapi.responses import JSONResponse

from db.database import init_db, get_conn
from routers.accounts import router as accounts_router
from routers.log import router as log_router

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
logger = logging.getLogger(__name__)


def _cleanup_once() -> int:
    """Синхронная функция очистки — запускается в потоке."""
    conn = get_conn()
    try:
        cur = conn.execute(
            "UPDATE accounts SET status='free', lease_token=NULL, leased_to_pc=NULL "
            "WHERE status='busy' AND last_heartbeat IS NOT NULL "
            "AND (unixepoch() - last_heartbeat) > 90"
        )
        freed = cur.rowcount
        # Чистим устаревшие idempotency_keys старше суток
        conn.execute(
            "DELETE FROM idempotency_keys WHERE unixepoch() - created_at > 86400"
        )
        conn.commit()
        return freed
    finally:
        conn.close()


async def cleanup_stale_loop():
    """Фоновый таск: освобождает зависшие аренды каждые 60 секунд."""
    while True:
        await asyncio.sleep(60)
        try:
            freed = await asyncio.to_thread(_cleanup_once)
            if freed:
                logger.info("Освобождено %d зависших аккаунтов", freed)
        except Exception:
            logger.exception("[cleanup] Ошибка")


@asynccontextmanager
async def lifespan(app: FastAPI):
    init_db()
    task = asyncio.create_task(cleanup_stale_loop())
    yield
    task.cancel()
    try:
        await task
    except asyncio.CancelledError:
        pass


app = FastAPI(title="Steam Club Account Manager", lifespan=lifespan)

app.include_router(accounts_router)
app.include_router(log_router)


@app.get("/health")
def health():
    try:
        conn = get_conn()
        try:
            conn.execute("SELECT 1").fetchone()
        finally:
            conn.close()
        return {"status": "ok"}
    except Exception:
        return JSONResponse(status_code=503, content={"status": "error"})
