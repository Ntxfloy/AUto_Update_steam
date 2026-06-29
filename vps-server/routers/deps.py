"""
routers/deps.py — общие зависимости (авторизация).
"""
import os
import secrets

from fastapi import HTTPException, Security
from fastapi.security import HTTPAuthorizationCredentials, HTTPBearer

bearer_scheme = HTTPBearer()


def verify_api_key(credentials: HTTPAuthorizationCredentials = Security(bearer_scheme)):
    api_key = os.environ.get("STEAM_API_KEY", "")
    # constant-time сравнение — защита от timing-атаки
    if not api_key or not secrets.compare_digest(credentials.credentials, api_key):
        raise HTTPException(status_code=401, detail="Неверный API ключ")
