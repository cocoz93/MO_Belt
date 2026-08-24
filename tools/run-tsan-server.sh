#!/bin/sh
# TSan 빌드 서버를 setarch -R(ASLR 끔)로 기동. 출력은 /tmp/belt-tsan.log 에 남긴다 —
# rc=66 이중 의미 때문에 판정은 반드시 로그 내용(FATAL 유무·WARNING 유무)으로 한다.
d="$(dirname "$0")/../server/build/tsan"
exec setarch "$(uname -m)" -R "$d/belt_server" \
    --listen "${1:-15401}" --port "${2:-19135}" --run-secs "${3:-20}" \
    > /tmp/belt-tsan.log 2>&1
