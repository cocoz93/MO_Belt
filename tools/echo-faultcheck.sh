#!/bin/sh
# 검증기 자체 시험 — 서버가 일부러 틀리게 되돌릴 때 더미가 정말 잡아내는가.
#   "위반 0" 이 깨끗한 건지 검증기가 눈뜬장님인지 가르는 절차다.
#   ⚠ EpollWorker.cpp 에 BELT_ECHO_FAULT 임시 패치가 들어간 상태에서만 의미가 있다(원복 대상).
#     mode 1 = 5000번째 응답의 마지막 바이트 뒤집기  → padErr 로 잡혀야 한다
#     mode 2 = 헤더의 크기 필드만 4 줄이기           → padErr(크기 불일치)로 잡혀야 한다
#     mode 3 = 5000번째 응답을 통째로 버리기          → orderErr 로 잡혀야 한다
# 사용: tools/echo-faultcheck.sh
d="$(dirname "$0")/../server/build/release"
for m in 1 2 3; do
    pkill -x belt_server 2>/dev/null
    rm -f /tmp/belt-echo-integrity.log
    sleep 0.4
    BELT_ECHO_FAULT=$m "$d/belt_server" --listen 15400 --port 19100 --game 0 --run-secs 30 >/tmp/s.log 2>&1 &
    sleep 1
    echo "── fault mode $m ──"
    "$d/belt_dummy" --mode echo --sessions 300 --run-secs 15 --metrics-port 19160 2>&1 | grep -E 'padErr|GATE'
    head -2 /tmp/belt-echo-integrity.log 2>/dev/null || echo "  ✗ 덤프 없음 — 검출 실패"
    pkill -x belt_server 2>/dev/null
    sleep 0.5
done
