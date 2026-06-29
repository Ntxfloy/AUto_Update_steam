"""
crypto.py — AES-256-GCM шифрование паролей.
Формат: base64url( nonce[12] || tag[16] || ciphertext )
"""
from __future__ import annotations

import base64
import binascii
import os

from Crypto.Cipher import AES
from Crypto.Random import get_random_bytes

NONCE_LEN = 12
TAG_LEN = 16
KEY_LEN = 32


class CryptoError(Exception):
    pass


class DecryptionError(CryptoError):
    pass


def _b64url_encode(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode("ascii")


def _b64url_decode(s: str) -> bytes:
    pad = "=" * (-len(s) % 4)
    return base64.urlsafe_b64decode(s + pad)


def load_key(raw: str | None = None) -> bytes:
    raw = raw if raw is not None else os.environ.get("STEAM_ENC_KEY")
    if not raw:
        raise CryptoError("STEAM_ENC_KEY не задан в окружении")
    raw = raw.strip()
    key: bytes | None = None
    try:
        candidate = binascii.unhexlify(raw)
        if len(candidate) == KEY_LEN:
            key = candidate
    except (binascii.Error, ValueError):
        pass
    if key is None:
        for decoder in (base64.b64decode, base64.urlsafe_b64decode):
            try:
                candidate = decoder(raw + "=" * (-len(raw) % 4))
                if len(candidate) == KEY_LEN:
                    key = candidate
                    break
            except (binascii.Error, ValueError):
                continue
    if key is None:
        raise CryptoError("STEAM_ENC_KEY должен декодироваться в 32 байта (AES-256)")
    return key


def encrypt(password: str, key: bytes) -> str:
    if len(key) != KEY_LEN:
        raise CryptoError(f"Ключ должен быть {KEY_LEN} байт")
    nonce = get_random_bytes(NONCE_LEN)
    cipher = AES.new(key, AES.MODE_GCM, nonce=nonce, mac_len=TAG_LEN)
    ciphertext, tag = cipher.encrypt_and_digest(password.encode("utf-8"))
    return _b64url_encode(nonce + tag + ciphertext)


def decrypt(enc: str, key: bytes) -> str:
    if len(key) != KEY_LEN:
        raise CryptoError(f"Ключ должен быть {KEY_LEN} байт")
    try:
        blob = _b64url_decode(enc)
    except (binascii.Error, ValueError) as e:
        raise DecryptionError("Некорректная base64-строка") from e
    if len(blob) < NONCE_LEN + TAG_LEN:
        raise DecryptionError("Данные слишком короткие")
    nonce = blob[:NONCE_LEN]
    tag = blob[NONCE_LEN:NONCE_LEN + TAG_LEN]
    ciphertext = blob[NONCE_LEN + TAG_LEN:]
    cipher = AES.new(key, AES.MODE_GCM, nonce=nonce, mac_len=TAG_LEN)
    try:
        plaintext = cipher.decrypt_and_verify(ciphertext, tag)
    except ValueError as e:
        raise DecryptionError("Проверка целостности не пройдена") from e
    return plaintext.decode("utf-8")
