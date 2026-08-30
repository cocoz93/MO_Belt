#!/bin/sh
# 왕복 지연 원인 가르기 — 4,000세션에서 p99 가 40ms 버킷에 걸린 게 무엇 때문인지 본다.
#   A 60Hz/스레드8  : 기준 (문제 재현)
#   B 30Hz/스레드8  : 부하만 절반 — 지연이 같이 반토막이면 처리량 한계
#   C 60Hz/스레드11 : 더미 코어만 더 — 지연이 좋아지면 측정 도구가 범인
# 사용: tools/echo-probe-lag.sh
here="$(dirname "$0")"
for a in "4000 20 8 60" "4000 20 8 30" "4000 20 11 60"; do
    # shellcheck disable=SC2086
    "$here/echo-once.sh" $a 2>&1 | grep -E '^ECHO|rtt p99'
    sleep 3
done
