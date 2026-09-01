#!/bin/sh
# 서버 사망 재현 — 10분 소크에서 서버가 약 120초 시점에 사라졌다(로그는 /tmp 와 함께 유실).
#   이번엔 로그를 Windows 쪽(/mnt/c)에 남겨 WSL 이 통째로 재시작돼도 보존되게 하고,
#   30초마다 생존을 확인해 죽은 시점을 좁힌다.
# 사용: tools/echo-repro-death.sh [sessions] [run초] [churn ms]
S=${1:-2000}
R=${2:-180}
C=${3:-5000}
d="$(dirname "$0")/../server/build/release"
OUT=/mnt/c/Users/USER/Desktop/MyGit/Belt/_repro
mkdir -p "$OUT"

pkill -x belt_server 2>/dev/null
pkill -x belt_dummy 2>/dev/null
sleep 0.5
ulimit -c unlimited

taskset -c 0-5 "$d/belt_server" --listen 15400 --game 0 --port 19100 \
    --run-secs $((R + 60)) > "$OUT/server.log" 2>&1 &
SRV=$!
sleep 1
taskset -c 6-16 "$d/belt_dummy" --mode echo --sessions "$S" --threads 8 \
    --pkt-min 20 --pkt-max 256 --attack-sendq 10 --slow-recv-ms 20 --churn-ms "$C" \
    --run-secs "$R" --ramp 80 --metrics-port 19160 > "$OUT/dummy.log" 2>&1 &
DUM=$!

echo "재현 시작: 세션 $S / ${R}초 / churn ${C}ms  (로그 $OUT)"
t=0
while [ "$t" -lt "$R" ]; do
    sleep 15
    t=$((t + 15))
    if kill -0 "$SRV" 2>/dev/null; then
        SESS=$(curl -s http://127.0.0.1:19100/metrics 2>/dev/null | awk '/^belt_sessions_active/{print $2}')
        ACC=$(curl -s http://127.0.0.1:19100/metrics 2>/dev/null | awk '/^belt_accepts_total/{print $2}')
        echo "  ${t}초: 생존 (세션 $SESS · 누적 accept $ACC)"
    else
        echo "  ★ ${t}초 이전에 서버 사망"
        wait "$SRV" 2>/dev/null
        echo "  종료 코드: $?"
        break
    fi
done

pkill -x belt_dummy 2>/dev/null
sleep 1
echo "── 서버 로그 끝부분 ──"
tail -20 "$OUT/server.log"
pkill -x belt_server 2>/dev/null
