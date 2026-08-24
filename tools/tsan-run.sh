#!/bin/sh
# TSan 실행 래퍼 — WSL2 는 ASLR 충돌로 TSan 이 대개 시작조차 못 한다(20회 중 16회 실측).
# setarch -R 로 ASLR 을 끄고 띄운다.
#
# ⚠ 판정 주의: rc=66 은 "못 떴다"와 "떴는데 경합을 찾았다"를 둘 다 가리킨다.
#   반드시 출력 첫 줄(FATAL: unexpected memory mapping 인지)까지 봐야 한다.
set -e
exe="$1"; shift
setarch "$(uname -m)" -R "$exe" "$@"
