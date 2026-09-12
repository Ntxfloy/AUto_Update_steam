PRAGMA journal_mode=WAL;
PRAGMA foreign_keys=ON;

CREATE TABLE IF NOT EXISTS accounts (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    login         TEXT    UNIQUE NOT NULL,
    password_enc  TEXT    NOT NULL,
    steam_id64    TEXT    UNIQUE,
    is_f2p        INTEGER NOT NULL DEFAULT 1,
    status        TEXT    NOT NULL DEFAULT 'free',
    lease_token   TEXT,
    leased_to_pc  TEXT,
    last_heartbeat INTEGER,
    created_at    INTEGER NOT NULL DEFAULT (unixepoch())
);

CREATE TABLE IF NOT EXISTS update_log (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    pc_id      TEXT NOT NULL,
    account_id INTEGER REFERENCES accounts(id),
    app_id     TEXT NOT NULL,
    started_at INTEGER NOT NULL DEFAULT (unixepoch()),
    finished_at INTEGER,
    result     TEXT,
    error_msg  TEXT,
    build_id_before TEXT,
    build_id_after  TEXT
);

CREATE TABLE IF NOT EXISTS idempotency_keys (
    key         TEXT NOT NULL,
    pc_id       TEXT NOT NULL,
    status      TEXT NOT NULL DEFAULT 'in_progress',
    account_id  INTEGER REFERENCES accounts(id),
    lease_token TEXT,
    http_status INTEGER,
    created_at  INTEGER NOT NULL DEFAULT (unixepoch()),
    PRIMARY KEY (key, pc_id)
);

-- "Account has no license for this app" — это свойство ПАРЫ (аккаунт, игра),
-- а не признак битого аккаунта. Раньше такая ошибка помечала аккаунт 'bad',
-- и одна игра без лицензии (например Call of Duty HQ) выжигала весь пул.
-- Теперь запоминаем только то, что этому аккаунту не надо больше выдавать
-- эту игру; для всех остальных игр аккаунт остаётся полностью рабочим.
CREATE TABLE IF NOT EXISTS account_app_denied (
    account_id INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    app_id     TEXT    NOT NULL,
    reason     TEXT,
    created_at INTEGER NOT NULL DEFAULT (unixepoch()),
    PRIMARY KEY (account_id, app_id)
);

CREATE INDEX IF NOT EXISTS idx_idem_created ON idempotency_keys(created_at);
CREATE INDEX IF NOT EXISTS idx_accounts_status ON accounts(status, is_f2p);
CREATE INDEX IF NOT EXISTS idx_denied_app ON account_app_denied(app_id);
