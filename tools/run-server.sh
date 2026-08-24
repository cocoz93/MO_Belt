#!/bin/sh
# 서버(release 빌드) 기동. 인자: [listen포트] [metrics포트] [run초]
d="$(dirname "$0")/../server/build/release"
exec "$d/belt_server" --listen "${1:-15400}" --port "${2:-19100}" --run-secs "${3:--1}"
