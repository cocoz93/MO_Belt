#!/bin/sh
# TSan 아래서 압박·재접속 경로를 밟는다.
#   왜: 무결성 대조는 "바이트가 틀렸나"를 보고, TSan 은 "자물쇠를 안 나눠 가졌나"를 본다.
#       뒤엣것은 실제로 겹치는 순간이 안 와도 잡히므로, 확률에 기대지 않는다.
#   ⚠ 서버만 TSan 빌드로 띄우고 더미는 release 를 쓴다 — 더미까지 느려지면 부하가 안 실린다.
#   ⚠ TSan 은 5~15배 느리다. 세션을 줄일 수밖에 없는데, 그러다 압박이 안 걸리면 헛돈 것이므로
#     아래 경로 계측(보류·링가득)이 0 이 아닌지 반드시 확인할 것.
#   ⚠ rc=66 은 "못 떴다"와 "경합을 찾았다"를 둘 다 뜻한다 — 로그 첫 줄로 가른다.
# 사용: tools/echo-tsan.sh [sessions] [run초]
S=${1:-200}
R=${2:-45}
here="$(dirname "$0")"
tsanBin="$here/../server/build/tsan/belt_server"
relBin="$here/../server/build/release/belt_dummy"
LOG=/tmp/belt-tsan-server.log

pkill -x belt_server 2>/dev/null
pkill -x belt_dummy 2>/dev/null
rm -f "$LOG" /tmp/belt-echo-integrity.log
sleep 0.5

TSAN_OPTIONS="halt_on_error=0 second_deadlock_stack=1" \
    "$here/tsan-run.sh" "$tsanBin" --listen 15400 --game 0 --port 19100 \
    --run-secs $((R + 20)) > "$LOG" 2>&1 &
sleep 3

if ! pgrep -x belt_server > /dev/null; then
    echo "★ 서버가 안 떴다 — 로그 첫 줄:"
    head -3 "$LOG"
    exit 1
fi

echo "── TSan 서버 + 압박·재접속 부하 (${S}세션 / ${R}초) ──"
"$relBin" --mode echo --sessions "$S" --threads 4 --run-secs "$R" \
    --pkt-min 20 --pkt-max 1024 --attack-sendq 10 --slow-recv-ms 20 \
    --churn-ms 3000 --metrics-port 19160 2>&1 | tail -1

curl -s http://127.0.0.1:19100/metrics 2>/dev/null | awk '
    /^belt_ring_split_read_total/ {a=$2}
    /^belt_ring_split_send_total/ {b=$2}
    /^belt_frame_wait_total/      {c=$2}
    /^belt_partial_send_total/    {e=$2}
    /^belt_epollout_arm_total/    {f=$2}
    /^belt_send_ring_full_total/  {g=$2}
    /^belt_accepts_total/         {h=$2}
    /^belt_disconnects_total/     {i=$2}
    END{printf "   경로: 랩읽기 %s · 랩송신 %s · 부분수신 %s · 부분전송 %s · 보류 %s · 링가득 %s\n   수명: accept %s · disconnect %s (슬롯 재사용량)\n",a,b,c,e,f,g,h,i}'

pkill -x belt_server 2>/dev/null
sleep 2

echo "── TSan 판정 ──"
if head -1 "$LOG" | grep -q "FATAL"; then
    echo "★ TSan 이 못 떴다(ASLR) — 이 런은 무효:"
    head -2 "$LOG"
elif grep -q "WARNING: ThreadSanitizer" "$LOG"; then
    echo "★ 경합 보고 $(grep -c 'WARNING: ThreadSanitizer' "$LOG") 건:"
    grep -A 12 "WARNING: ThreadSanitizer" "$LOG" | head -40
else
    echo "TSan 무보고 (로그 $(wc -l < "$LOG") 줄)"
fi
