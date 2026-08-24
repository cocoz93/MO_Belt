#!/bin/sh
# 비공식 방 개수 스윕 — 방 500 / 1,000 / 1,500 (세션 4배수). "이동만" 라벨.
# ⚠ 공식 베이스라인은 사용자 입회 + 측정 잡음 정리 후에 따로 돈다.
here="$(dirname "$0")"
for s in 2000 4000 6000; do
    echo "── sessions $s ──"
    sh "$here/sweep-once.sh" "$s" 20 || exit 1
    sleep 3
done
echo "SWEEP ALL OK"
