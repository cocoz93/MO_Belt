#!/bin/sh
# U1.2 스모크: ① 자기시험(링버퍼 계약) ② /metrics 응답 확인 (release 빌드 기준)
set -e
d="$(dirname "$0")/../server/build/release"

echo "[1/2] selftest"
"$d/belt_server" --selftest

echo "[2/2] /metrics"
"$d/belt_server" --port 19133 --run-secs 3 &
pid=$!
sleep 1
out=$(curl -s http://127.0.0.1:19133/metrics)
echo "$out" | grep -q "^belt_up 1" || { echo "SMOKE FAIL: belt_up 미노출"; kill $pid 2>/dev/null; exit 1; }
echo "$out" | grep -q "belt_thread_cpu_ns" || { echo "SMOKE FAIL: 스레드 CPU 미노출"; kill $pid 2>/dev/null; exit 1; }
echo "$out"
wait $pid
echo "U1.2 smoke OK"
