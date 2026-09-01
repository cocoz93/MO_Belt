#!/bin/sh
# 무결성 검증 — 셋째 줄까지는 padErr·orderErr 가 0 이어야 하고,
# 넷째·다섯째는 "서버가 그 세션만 끊고 살아남는가"를 본다.
#
# ★ 함께 찍는 경로 계측이 이 검증의 뼈대다.
#   "위반 0" 은 그 코드 길을 밟고도 멀쩡했을 때만 뜻이 있다. 계측이 0 인 경로는
#   검증된 게 아니라 안 지나간 것 — 실제로 ①~③ 만 돌렸을 때 보류·링가득·부분전송이
#   전부 0 이었고, 그래서 ④⑤(sendQ 압박)를 따로 만들었다.
#
#   ① 정상        : 60Hz, 크기 20~256B 랜덤 — 링 랩·코얼레싱 경계를 크기가 섞인 채로 때린다
#   ② 과부하 창   : 최대 속도 + 미응답 100 허용 — 송신 정체
#   ③ 재접속 반복 : 부하 한가운데서 세션이 나가고 들어온다 — 수명 관리·슬롯 재사용
#   ④ sendQ 압박  : 앞 N 개가 아예 안 읽는다 → 서버가 그 세션만 끊어야 한다(링 가득 경로)
#   ⑤ 느린 소비자 : 수신창을 좁히고 조금씩만 읽는다 → 보류·부분 전송 경로
#
# 검증기 자체가 제대로 잡는지는 tools/echo-faultcheck.sh 로 따로 확인한다.
# 사용: tools/echo-verify.sh [sessions] [run초]
S=${1:-500}
R=${2:-15}
d="$(dirname "$0")/../server/build/release"

run() {
    label=$1
    shift
    pkill -x belt_server 2>/dev/null
    rm -f /tmp/belt-echo-integrity.log
    sleep 0.4
    "$d/belt_server" --listen 15400 --port 19100 --game 0 --run-secs $((R + 15)) > /tmp/s.log 2>&1 &
    sleep 1
    echo "── $label ──"
    "$d/belt_dummy" --mode echo --sessions "$S" --run-secs "$R" --metrics-port 19160 "$@" 2>&1 | tail -1
    curl -s http://127.0.0.1:19100/metrics 2>/dev/null | awk '
        /^belt_ring_split_read_total/  {a=$2}
        /^belt_ring_split_send_total/  {b=$2}
        /^belt_frame_wait_total/       {c=$2}
        /^belt_partial_send_total/     {e=$2}
        /^belt_epollout_arm_total/     {f=$2}
        /^belt_send_ring_full_total/   {g=$2}
        END{printf "   경로: 랩읽기 %s · 랩송신 %s · 부분수신 %s · 부분전송 %s · 보류 %s · 링가득 %s\n",a,b,c,e,f,g}'
    if pgrep -x belt_server > /dev/null; then echo "   서버 생존 OK"; else echo "   ★ 서버가 죽었다"; fi
    if [ -f /tmp/belt-echo-integrity.log ]; then
        echo "   ★ 위반 덤프:"
        head -3 /tmp/belt-echo-integrity.log
    fi
    pkill -x belt_server 2>/dev/null
    sleep 0.5
}

run "① 정상 60Hz 가변 20~256B"
run "② 과부하 창 100 (최대 속도)" --input-hz 1000 --over-send 100
run "③ 재접속 반복 (churn 2s)" --churn-ms 2000
run "④ sendQ 압박 (안 읽는 세션 10)" --pkt-min 512 --pkt-max 1024 --attack-sendq 10
run "⑤ 느린 소비자 (창 4KB·20ms)"   --pkt-min 512 --pkt-max 1024 --attack-sendq 20 --slow-recv-ms 20
