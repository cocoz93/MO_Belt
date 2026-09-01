#!/bin/sh
# 무결성 검증 3종 — 셋 다 padErr·orderErr 가 0 이어야 통과.
#   ① 정상        : 60Hz, 크기 20~256B 랜덤 — 링버퍼 랩·코얼레싱 경계를 크기가 섞인 채로 때린다
#   ② 과부하 창   : 최대 속도 + 미응답 100 허용 — 송신 정체를 만들어 부분 전송·보류 경로를 때린다
#   ③ 재접속 반복 : 부하 한가운데서 세션이 나가고 들어온다 — 수명 관리와 슬롯 재사용을 때린다
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
    if [ -f /tmp/belt-echo-integrity.log ]; then
        echo "  ★ 위반 덤프:"
        head -3 /tmp/belt-echo-integrity.log
    fi
    pkill -x belt_server 2>/dev/null
    sleep 0.5
}

run "① 정상 60Hz 가변 20~256B"
run "② 과부하 창 100 (최대 속도)" --input-hz 1000 --over-send 100
run "③ 재접속 반복 (churn 2s)" --churn-ms 2000
