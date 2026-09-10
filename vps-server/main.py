"""
main.py — FastAPI приложение для управления Steam-аккаунтами компьютерного клуба.

Запуск на Windows (локальная сеть клуба):
    run-windows.bat
Панель админа: http://<ip этой машины>:8000/
"""
import asyncio
import logging
import logging.handlers
import os
import socket
from contextlib import asynccontextmanager

from dotenv import load_dotenv
load_dotenv()

from fastapi import FastAPI
from fastapi.responses import JSONResponse

from db.database import init_db, get_conn
from routers.accounts import router as accounts_router
from routers.admin import router as admin_router
from routers.log import router as log_router

# --- Логирование: консоль + файл с ротацией -------------------------------
# Файл нужен, чтобы панель могла показывать хвост лога в браузере, а также чтобы
# при закрытии консоли на Windows история ошибок не терялась.
LOG_FILE = os.environ.get("STEAM_LOG_FILE", "server.log")
_handlers: list[logging.Handler] = [logging.StreamHandler()]
try:
    _handlers.append(logging.handlers.RotatingFileHandler(
        LOG_FILE, maxBytes=5 * 1024 * 1024, backupCount=3, encoding="utf-8"
    ))
except OSError:
    pass  # нет прав на запись — работаем только в консоль

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    handlers=_handlers,
)
logger = logging.getLogger(__name__)


def _lan_ip() -> str:
    """IP в локальной сети — чтобы сразу напечатать адрес панели."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("10.255.255.255", 1))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


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
    if not os.environ.get("STEAM_API_KEY"):
        logger.error("STEAM_API_KEY не задан! Клиенты не смогут авторизоваться.")
    if not os.environ.get("STEAM_ENC_KEY"):
        logger.error("STEAM_ENC_KEY не задан! Пароли нельзя будет расшифровать.")

    ip = _lan_ip()
    logger.info("=" * 62)
    logger.info("  Панель админа:  http://%s:8000/", ip)
    logger.info("  Для клиентов в updater.ini:  server_url=http://%s:8000", ip)
    logger.info("  Лог-файл: %s", os.path.abspath(LOG_FILE))
    logger.info("=" * 62)

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
app.include_router(admin_router)


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
