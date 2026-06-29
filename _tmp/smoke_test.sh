#!/bin/bash
API_KEY=$(grep STEAM_API_KEY /root/steam-club/.env | cut -d= -f2-)
echo "=== /health ==="
curl -s http://localhost:8000/health
echo ""
echo ""
echo "=== /accounts/status ==="
curl -s -H "Authorization: Bearer $API_KEY" http://localhost:8000/accounts/status | python3 -m json.tool
echo ""
echo "=== API_KEY for client config ==="
echo "$API_KEY"
