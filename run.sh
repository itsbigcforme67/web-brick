#!/usr/bin/env bash
# Serve the app from this computer:  bash run.sh [port]
# Then open http://localhost:8767 here, or http://<this computer's address>:8767 on a phone on the same Wi-Fi.
cd "$(dirname "$0")/web"
PORT="${1:-8767}"
echo "Web Brick: http://localhost:$PORT"
hostname -I 2>/dev/null | tr ' ' '\n' | grep -E '^[0-9]+\.' | sed "s#.*#           http://&:$PORT#"
exec python3 -m http.server "$PORT"
