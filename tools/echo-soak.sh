#!/bin/sh
# 장시간 소크 — 짧은 런으로는 못 보는 것을 본다.
#   ① 링 랩을 충분히 많이 지나고도 무결한가 (15초 런은 세션당 랩 15 바퀴뿐이었다)
#   ② 시간이 가며 새는 것이 있나 — 메모리(RSS)·fd·세션 수
#   ③ 지연이 서서히 나빠지나 (드리프트)
#   압박 세션을 조금 섞어 보류·링가득 경로도 내내 밟게 한다.
# 사용: tools/echo-soak.sh [sessions] [분] [압박세션수] [churn ms]
#   ⚠ churn 을 0 으로 두면 세션이 끝까지 살아 있어 슬롯 재사용(세대 대조) 경로가 안 밟히고,
#     압박 세션도 초반에 절단된 뒤로는 다시 안 붙어 보류·링가득이 30초 만에 멈춘다(첫 소크 실측).
S=${1:-2000}
M=${2:-10}
A=${3:-10}
C=${4:-5000}
R=$((M * 60))
d="$(dirname "$0")/../server/build/release"
LOG=/tmp/belt-soak.log

pkill -x belt_server 2>/dev/null
pkill -x belt_dummy 2>/dev/null
rm -f /tmp/belt-echo-integrity.log "$LOG"
sleep 0.5

taskset -c 0-5 "$d/belt_server" --listen 15400 --game 0 --port 19100 \
    --run-secs $((R + 60)) > /tmp/belt-soak-server.log 2>&1 &
SRV=$!
sleep 1
taskset -c 6-16 "$d/belt_dummy" --mode echo --sessions "$S" --threads 8 \
    --pkt-min 20 --pkt-max 256 --attack-sendq "$A" --slow-recv-ms 20 --churn-ms "$C" \
    --run-secs "$R" --ramp 80 --metrics-port 19160 > /tmp/belt-soak-dummy.log 2>&1 &
DUM=$!

echo "soak 시작: 세션 $S / ${M}분 / 압박 $A / churn ${C}ms — 30초마다 기록 ($LOG)"
i=0
while kill -0 "$DUM" 2>/dev/null; do
    sleep 30
    i=$((i + 30))
    RSS=$(awk '/VmRSS/{print $2}' "/proc/$SRV/status" 2>/dev/null)
    FD=$(ls "/proc/$SRV/fd" 2>/dev/null | wc -l)
    M1=$(curl -s http://127.0.0.1:19100/metrics 2>/dev/null)
    D1=$(curl -s http://127.0.0.1:19160/metrics 2>/dev/null)
    SESS=$(echo "$M1" | awk '/^belt_sessions_active/{print $2}')
    WRAP=$(echo "$M1" | awk '/^belt_ring_split_read_total/{print $2}')
    ARM=$(echo "$M1" | awk '/^belt_epollout_arm_total/{print $2}')
    FULL=$(echo "$M1" | awk '/^belt_send_ring_full_total/{print $2}')
    ACC=$(echo "$M1" | awk '/^belt_accepts_total/{print $2}')
    PAD=$(echo "$D1" | awk '/^dummy_pad_error_total/{print $2}')
    ORD=$(echo "$D1" | awk '/^dummy_order_error_total/{print $2}')
    RECV=$(echo "$D1" | awk '/^dummy_echo_recv_total/{print $2}')
    printf "%4ds  세션 %s  RSS %sKB  fd %s  재사용 %s  랩 %s  보류 %s  링가득 %s  왕복 %s  padErr %s  ordErr %s\n" \
           "$i" "$SESS" "$RSS" "$FD" "$ACC" "$WRAP" "$ARM" "$FULL" "$RECV" "$PAD" "$ORD" | tee -a "$LOG"
done

echo "── 소크 종료 ──" | tee -a "$LOG"
tail -1 /tmp/belt-soak-dummy.log | tee -a "$LOG"
if [ -f /tmp/belt-echo-integrity.log ]; then
    echo "★ 위반 덤프:" | tee -a "$LOG"
    head -5 /tmp/belt-echo-integrity.log | tee -a "$LOG"
fi
if pgrep -x belt_server > /dev/null; then echo "서버 생존 OK" | tee -a "$LOG"; else echo "★ 서버가 죽었다" | tee -a "$LOG"; fi
pkill -x belt_server 2>/dev/null
