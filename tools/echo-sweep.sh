#!/bin/sh
# 에코 기준선 스윕 — 세션 수를 올려가며 echo-once.sh 를 반복한다.
#   인자를 그대로 목록으로 받는다(PowerShell 에서 인라인 변수를 쓰면 조용히 밀린다 — 반드시 이 파일로).
# 사용: tools/echo-sweep.sh 1000 2000 4000
HOLD=20
DTH=8
here="$(dirname "$0")"
for S in "$@"; do
    "$here/echo-once.sh" "$S" "$HOLD" "$DTH" 2>&1 | grep -E '^ECHO|^belt_dummy: connected|^RUN GATE' || echo "ECHO sessions=$S — 런 실패"
    sleep 3
done
