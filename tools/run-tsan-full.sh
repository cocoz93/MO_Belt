#!/bin/sh
# TSan 빌드 서버(전송+게임 워커)를 setarch -R 로 기동. 로그는 /tmp/belt-tsan.log.
# 판정은 rc 가 아니라 로그 내용(FATAL 유무·WARNING 유무)으로 한다.
d="$(dirname "$0")/../server/build/tsan"
exec setarch "$(uname -m)" -R "$d/belt_server" \
    --listen "${1:-15401}" --game "${2:-3}" --port "${3:-19141}" --run-secs "${4:-40}" \
    > /tmp/belt-tsan.log 2>&1
