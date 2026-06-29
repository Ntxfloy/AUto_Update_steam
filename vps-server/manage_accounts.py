#!/usr/bin/env python3
"""
manage_accounts.py — CLI для управления Steam-аккаунтами компьютерного клуба.

Команды:
    add         добавить один аккаунт
    list        показать все аккаунты (без паролей)
    delete      удалить аккаунт по login
    set-status  сменить статус (free/disabled) по login
    bulk-add    массовое добавление из CSV (login,password,steam_id64,is_paid)
    gen-key     сгенерировать новый STEAM_ENC_KEY (hex)

Зависимости: stdlib + pycryptodome (через crypto.py).
Ключ шифрования берётся из env STEAM_ENC_KEY.
"""
from __future__ import annotations

import argparse
import csv
import os
import secrets
import sqlite3
import sys

from dotenv import load_dotenv
load_dotenv()

from crypto import load_key, encrypt
from db.database import get_conn, init_db

VALID_STATUSES = ("free", "disabled")


def require_key() -> bytes:
    try:
        return load_key()
    except Exception as e:
        sys.exit(f"Ошибка ключа шифрования: {e}\nЗадайте STEAM_ENC_KEY (см. команду gen-key).")


def ensure_table(conn: sqlite3.Connection) -> None:
    # Единый источник истины — db/schema.sql (все таблицы разом)
    init_db()


def cmd_add(args: argparse.Namespace) -> None:
    key = require_key()
    is_f2p = 0 if args.paid else 1
    steam_id64 = args.steam_id64 or None

    conn = get_conn()
    try:
        ensure_table(conn)
        enc = encrypt(args.password, key)
        try:
            conn.execute(
                "INSERT INTO accounts (login, password_enc, steam_id64, is_f2p, status) "
                "VALUES (?, ?, ?, ?, 'free')",
                (args.login, enc, steam_id64, is_f2p),
            )
            conn.commit()
        except sqlite3.IntegrityError as e:
            sys.exit(f"Не удалось добавить '{args.login}': {e}")
    finally:
        conn.close()

    kind = "платный" if args.paid else "F2P"
    print(f"✓ Аккаунт '{args.login}' добавлен ({kind}"
          f"{', steam_id64=' + steam_id64 if steam_id64 else ''}).")


def cmd_list(args: argparse.Namespace) -> None:
    conn = get_conn()
    try:
        ensure_table(conn)
        rows = conn.execute(
            "SELECT id, login, steam_id64, is_f2p, status FROM accounts ORDER BY id"
        ).fetchall()
    finally:
        conn.close()

    if not rows:
        print("Аккаунтов нет.")
        return

    table = [
        {
            "ID": str(r["id"]),
            "LOGIN": r["login"],
            "STEAM_ID64": r["steam_id64"] or "-",
            "TYPE": "F2P" if r["is_f2p"] else "PAID",
            "STATUS": r["status"],
        }
        for r in rows
    ]
    columns = ["ID", "LOGIN", "STEAM_ID64", "TYPE", "STATUS"]
    widths = {c: max(len(c), max(len(row[c]) for row in table)) for c in columns}

    def fmt_row(values: dict) -> str:
        return "  ".join(values[c].ljust(widths[c]) for c in columns)

    print(fmt_row({c: c for c in columns}))
    print("  ".join("-" * widths[c] for c in columns))
    for row in table:
        print(fmt_row(row))
    print(f"\nВсего: {len(rows)}")


def cmd_delete(args: argparse.Namespace) -> None:
    conn = get_conn()
    try:
        ensure_table(conn)
        acc = conn.execute(
            "SELECT id FROM accounts WHERE login=?", (args.login,)
        ).fetchone()
        if not acc:
            sys.exit(f"Аккаунт '{args.login}' не найден.")
        acc_id = acc["id"]
        # H2: снимаем внешние ссылки перед удалением (сохраняем историю логов)
        conn.execute("UPDATE update_log SET account_id=NULL WHERE account_id=?", (acc_id,))
        conn.execute("DELETE FROM idempotency_keys WHERE account_id=?", (acc_id,))
        conn.execute("DELETE FROM accounts WHERE id=?", (acc_id,))
        conn.commit()
    finally:
        conn.close()

    print(f"✓ Аккаунт '{args.login}' удалён.")


def cmd_set_status(args: argparse.Namespace) -> None:
    conn = get_conn()
    try:
        ensure_table(conn)
        cur = conn.execute(
            "UPDATE accounts "
            "SET status=?, lease_token=NULL, leased_to_pc=NULL, last_heartbeat=NULL "
            "WHERE login=?",
            (args.status, args.login),
        )
        conn.commit()
    finally:
        conn.close()

    if cur.rowcount:
        print(f"✓ Статус '{args.login}' → '{args.status}'.")
    else:
        sys.exit(f"Аккаунт '{args.login}' не найден.")


def cmd_bulk_add(args: argparse.Namespace) -> None:
    key = require_key()

    if not os.path.isfile(args.csv_file):
        sys.exit(f"Файл не найден: {args.csv_file}")

    added, skipped, errors = 0, 0, 0
    conn = get_conn()
    try:
        ensure_table(conn)
        with open(args.csv_file, newline="", encoding="utf-8") as f:
            reader = csv.reader(f)
            for lineno, raw in enumerate(reader, start=1):
                if not raw or all(not c.strip() for c in raw):
                    continue
                if lineno == 1 and raw[0].strip().lower() == "login":
                    continue
                if len(raw) < 2:
                    print(f"  строка {lineno}: пропущена (нужны login,password)")
                    errors += 1
                    continue

                login = raw[0].strip()
                password = raw[1].strip()
                steam_id64 = raw[2].strip() if len(raw) > 2 and raw[2].strip() else None
                is_paid_raw = raw[3].strip().lower() if len(raw) > 3 else ""
                is_f2p = 0 if is_paid_raw in ("1", "true", "yes", "paid", "y") else 1

                if not login or not password:
                    print(f"  строка {lineno}: пропущена (пустой login или password)")
                    errors += 1
                    continue

                try:
                    enc = encrypt(password, key)
                    conn.execute(
                        "INSERT INTO accounts (login, password_enc, steam_id64, is_f2p, status) "
                        "VALUES (?, ?, ?, ?, 'free')",
                        (login, enc, steam_id64, is_f2p),
                    )
                    conn.commit()
                    added += 1
                except sqlite3.IntegrityError:
                    print(f"  строка {lineno}: '{login}' уже существует — пропущена")
                    skipped += 1
                except Exception as e:
                    print(f"  строка {lineno}: ошибка '{login}': {e}")
                    errors += 1
    finally:
        conn.close()

    print(f"\nИтог: добавлено {added}, пропущено {skipped}, ошибок {errors}.")


def cmd_gen_key(args: argparse.Namespace) -> None:
    key_hex = secrets.token_bytes(32).hex()
    print(key_hex)
    print("\nДобавьте в .env строку:", file=sys.stderr)
    print(f"STEAM_ENC_KEY={key_hex}", file=sys.stderr)
    print("\nВНИМАНИЕ: смена ключа делает нечитаемыми все ранее "
          "зашифрованные пароли. Меняйте только на пустой БД.", file=sys.stderr)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="manage_accounts.py",
        description="Управление Steam-аккаунтами компьютерного клуба.",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    p_add = sub.add_parser("add", help="Добавить один аккаунт")
    p_add.add_argument("--login", required=True)
    p_add.add_argument("--password", required=True)
    p_add.add_argument("--steam-id64", dest="steam_id64", default=None)
    p_add.add_argument("--paid", action="store_true", help="Платный аккаунт (is_f2p=0)")
    p_add.set_defaults(func=cmd_add)

    p_list = sub.add_parser("list", help="Показать все аккаунты (без паролей)")
    p_list.set_defaults(func=cmd_list)

    p_del = sub.add_parser("delete", help="Удалить аккаунт по login")
    p_del.add_argument("--login", required=True)
    p_del.set_defaults(func=cmd_delete)

    p_status = sub.add_parser("set-status", help="Сменить статус (free/disabled)")
    p_status.add_argument("--login", required=True)
    p_status.add_argument("--status", required=True, choices=VALID_STATUSES)
    p_status.set_defaults(func=cmd_set_status)

    p_bulk = sub.add_parser("bulk-add", help="Массовое добавление из CSV")
    p_bulk.add_argument("csv_file", help="Путь к CSV файлу")
    p_bulk.set_defaults(func=cmd_bulk_add)

    p_key = sub.add_parser("gen-key", help="Сгенерировать новый STEAM_ENC_KEY (hex)")
    p_key.set_defaults(func=cmd_gen_key)

    return parser


def main() -> None:
    parser = build_parser()
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
