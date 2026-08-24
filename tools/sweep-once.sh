#!/bin/sh
# 비공식 방 개수 스윕 1런 — "이동만(전투 없음)" 조건 라벨. WSL 안에서 실행.
#   ① 서버를 0-11, 더미를 12-16 코어에 pin (OS 여유 17-19 — 20코어 전소 사고 방지)
#   ② /proc Cpus_allowed_list 서로소 게이트
#   ③ 램프 후 유지 구간의 게임 워커 CPU 차분 → 워커당 평균 % (비공식 참고치)
#   ④ 게이트: 더미 자가 게이트(rc) + 서버 skipped=0
# 사용: tools/sweep-once.sh <sessions> [hold초]
set -e
S=${1:-2000}
HOLD=${2:-20}
d="$(dirname "$0")/../server/build/release"

taskset -c 0-11 "$d/belt_server" --listen 15400 --game 7 --port 19152 \
    --run-secs $((HOLD+30)) > /tmp/belt-sweep-server.log 2>&1 &
SRV=$!
sleep 1
taskset -c 12-16 "$d/belt_dummy" --server 127.0.0.1 --port 15400 --sessions "$S" \
    --run-secs $((HOLD+12)) --ramp 80 --metrics-port 19160 > /tmp/belt-sweep-dummy.log 2>&1 &
DUM=$!

A=$(grep Cpus_allowed_list "/proc/$SRV/status" | awk '{print $2}')
B=$(grep Cpus_allowed_list "/proc/$DUM/status" | awk '{print $2}')
if [ "$A" != "0-11" ] || [ "$B" != "12-16" ]; then
    echo "RUN GATE FAIL: cpus server=$A dummy=$B (서로소 아님)"
    kill "$SRV" "$DUM" 2>/dev/null || true
    exit 1
fi

sleep 8    # 램프 종료 대기 — 유지 구간 진입
CPU0=$(curl -s http://127.0.0.1:19152/metrics | awk '/thread_cpu_ns\{thread="game/ {s+=$2} END{print s}')
T0=$(date +%s%N)
sleep "$HOLD"
CPU1=$(curl -s http://127.0.0.1:19152/metrics | awk '/thread_cpu_ns\{thread="game/ {s+=$2} END{print s}')
T1=$(date +%s%N)

M=$(curl -s http://127.0.0.1:19152/metrics)
ROOMS=$(echo "$M" | awk '/^belt_rooms_active/ {print $2}')
SESS=$(echo "$M" | awk '/^belt_sessions_active/ {print $2}')
SKIP=$(echo "$M" | awk -F' ' '/belt_game_skipped_ticks_total/ {s+=$2} END{print s+0}')
DROP=$(echo "$M" | awk '/^belt_snapshots_dropped_total/ {print $2}')
RDROP=$(echo "$M" | awk '/^belt_input_ring_drops_total/ {print $2}')

wait "$DUM"; DUMRC=$?
tail -1 /tmp/belt-sweep-dummy.log

GCPU=$(awk "BEGIN{printf \"%.2f\", ($CPU1-$CPU0)*100.0/($T1-$T0)/7}")
echo "SWEEP sessions=$S rooms=$ROOMS sess_active=$SESS hold=${HOLD}s gameCpu/worker=${GCPU}% skipped=$SKIP snapDrop=$DROP ringDrop=$RDROP dummyRC=$DUMRC"

kill "$SRV" 2>/dev/null || true
wait "$SRV" 2>/dev/null || true

if [ "$DUMRC" = "0" ] && [ "$SKIP" = "0" ]; then
    echo "RUN GATE OK"
else
    echo "RUN GATE FAIL"
    exit 1
fi
