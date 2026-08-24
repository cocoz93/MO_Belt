#!/bin/sh
# 떠 있는 belt_server 의 열린 fd 개수 — 누수 판독용
pid=$(pgrep -f 'belt_server --listen' | head -1)
[ -z "$pid" ] && { echo "no-server"; exit 1; }
ls "/proc/$pid/fd" | wc -l
