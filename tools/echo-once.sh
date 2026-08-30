#!/bin/sh
# 에코 기준선 1런 — 게임 워커를 끈 서버(--game 0)에 ECHO 만 왕복시켜 전송 계층만 잰다.
#   왜: 2차에서 전투가 붙으면 "느려진 게 전송 탓인가 게임 탓인가"를 가를 기준선이 필요하다.
#       sweep-once.sh 가 게임 워커 CPU 를 재는 자리에서, 이쪽은 epoll 워커 CPU 를 잰다.
#   ① 서버 0-5 / 더미 6-16 pin + Cpus_allowed_list 서로소 게이트
#      게임 모드(sweep-once.sh)는 서버 0-11 이지만, 에코 런은 게임 워커 7개가 아예 없으므로
#      서버는 epoll 4+main 1 로 6코어면 충분하다. 남는 몫을 더미에 준다 —
#      에코는 60Hz 왕복이라 더미 수신 이벤트가 게임 모드(20Hz 스냅샷)의 3배라서,
#      코어를 게임 런처럼 5개만 주면 더미가 먼저 포화해 서버 수치가 무의미해진다(1,000세션 실측).
#   ② 램프 종료 후 유지 구간의 epoll 워커 CPU 차분 → 워커당 평균 %
#   ③ 게이트: 더미 자가 게이트(rc) + 서버가 끊은 세션 0
# ⚠ 다른 belt_server 가 15400 에 떠 있으면 SO_REUSEPORT 로 연결이 나뉘어 측정이 오염된다 — 먼저 정리할 것.
# 사용: tools/echo-once.sh <sessions> [hold초] [더미스레드] [송신Hz]
set -e
S=${1:-2000}
HOLD=${2:-20}
DTH=${3:-8}
HZ=${4:-60}
d="$(dirname "$0")/../server/build/release"

taskset -c 0-5 "$d/belt_server" --listen 15400 --game 0 --port 19152 \
    --run-secs $((HOLD+30)) > /tmp/belt-echo-server.log 2>&1 &
SRV=$!
sleep 1
taskset -c 6-16 "$d/belt_dummy" --mode echo --server 127.0.0.1 --port 15400 --sessions "$S" \
    --threads "$DTH" --input-hz "$HZ" --run-secs $((HOLD+12)) --ramp 80 \
    --metrics-port 19160 > /tmp/belt-echo-dummy.log 2>&1 &
DUM=$!

A=$(grep Cpus_allowed_list "/proc/$SRV/status" | awk '{print $2}')
B=$(grep Cpus_allowed_list "/proc/$DUM/status" | awk '{print $2}')
if [ "$A" != "0-5" ] || [ "$B" != "6-16" ]; then
    echo "RUN GATE FAIL: cpus server=$A dummy=$B (서로소 아님)"
    kill "$SRV" "$DUM" 2>/dev/null || true
    exit 1
fi

sleep 8    # 램프 종료 대기 — 유지 구간 진입
CPU0=$(curl -s http://127.0.0.1:19152/metrics | awk '/thread_cpu_ns\{thread="epoll/ {s+=$2} END{print s}')
T0=$(date +%s%N)
sleep "$HOLD"
CPU1=$(curl -s http://127.0.0.1:19152/metrics | awk '/thread_cpu_ns\{thread="epoll/ {s+=$2} END{print s}')
T1=$(date +%s%N)

M=$(curl -s http://127.0.0.1:19152/metrics)
SESS=$(echo "$M" | awk '/^belt_sessions_active/ {print $2}')
ACC=$(echo "$M" | awk '/^belt_accepts_total/ {print $2}')
DIS=$(echo "$M" | awk '/^belt_disconnects_total/ {print $2}')
RB=$(echo "$M" | awk '/^belt_recv_bytes_total/ {print $2}')
SB=$(echo "$M" | awk '/^belt_send_bytes_total/ {print $2}')

wait "$DUM"; DUMRC=$?
tail -1 /tmp/belt-echo-dummy.log

ECPU=$(awk "BEGIN{printf \"%.2f\", ($CPU1-$CPU0)*100.0/($T1-$T0)/4}")
echo "ECHO sessions=$S hz=$HZ dth=$DTH sess_active=$SESS hold=${HOLD}s epollCpu/worker=${ECPU}% accepts=$ACC disconnects=$DIS recvMB=$((RB/1048576)) sendMB=$((SB/1048576)) dummyRC=$DUMRC"

kill "$SRV" 2>/dev/null || true
wait "$SRV" 2>/dev/null || true

if [ "$DUMRC" = "0" ] && [ "$DIS" = "0" ]; then
    echo "RUN GATE OK"
else
    echo "RUN GATE FAIL"
    exit 1
fi
