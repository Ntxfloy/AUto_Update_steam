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
from routers.clientlogs import router as client_logs_router
from routers.log import router as log_router

# Через сколько секунд без heartbeat аренда считается брошенной.
#
# Было 90 секунд, и это оказалось слишком жёстко: heartbeat идёт раз в 15 с, а
# во время закачки на полной скорости канал клуба легко глотает несколько
# запросов подряд (плюс перезапуск сервера). Шесть потерянных ударов — и
# аккаунт уходил в пул, хотя steamcmd на клиенте им ещё пользовался: второй ПК
# получал тот же логин и ловил Login Failure. 240 секунд = 16 пропущенных
# ударов, при этом реально повисшая машина освобождает аккаунт за 4 минуты.
LEASE_TIMEOUT_SEC = int(os.environ.get("STEAM_LEASE_TIMEOUT_SEC", "240"))

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


def _cleanup_once() -> list[tuple[int, str, int]]:
    """Синхронная функция очистки — запускается в потоке.

    Возвращает список (account_id, pc_id, секунд_без_heartbeat) — чтобы в логе
    было видно, КАКОЙ ПК отвалился, а не просто счётчик.
    """
    conn = get_conn()
    try:
        rows = conn.execute(
            "SELECT id, leased_to_pc, (unixepoch() - last_heartbeat) AS silent "
            "FROM accounts "
            "WHERE status='busy' AND last_heartbeat IS NOT NULL "
            "AND (unixepoch() - last_heartbeat) > ?",
            (LEASE_TIMEOUT_SEC,)
        ).fetchall()

        if rows:
            conn.execute(
                "UPDATE accounts SET status='free', lease_token=NULL, leased_to_pc=NULL "
                "WHERE status='busy' AND last_heartbeat IS NOT NULL "
                "AND (unixepoch() - last_heartbeat) > ?",
                (LEASE_TIMEOUT_SEC,)
            )

        # Чистим устаревшие idempotency_keys старше суток
        conn.execute(
            "DELETE FROM idempotency_keys WHERE unixepoch() - created_at > 86400"
        )
        conn.commit()
        return [(r["id"], r["leased_to_pc"] or "?", int(r["silent"] or 0)) for r in rows]
    finally:
        conn.close()


async def cleanup_stale_loop():
    """Фоновый таск: освобождает зависшие аренды каждые 60 секунд."""
    while True:
        await asyncio.sleep(60)
        try:
            freed = await asyncio.to_thread(_cleanup_once)
            for account_id, pc_id, silent in freed:
                logger.warning(
                    "Аренда освобождена: account_id=%s pc_id=%s (нет heartbeat %d с, лимит %d с)",
                    account_id, pc_id, silent, LEASE_TIMEOUT_SEC
                )
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
    logger.info("  Логи клиентов: POST /logs/ingest -> %s",
                os.path.abspath(os.environ.get("STEAM_CLIENT_LOG_DIR", "logs")))
    logger.info("  Таймаут аренды: %d с без heartbeat", LEASE_TIMEOUT_SEC)
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
app.include_router(client_logs_router)
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


if __name__ == "__main__":
    import multiprocessing
    import uvicorn

    multiprocessing.freeze_support()
    uvicorn.run(app, host="0.0.0.0", port=8000, log_config=None)
