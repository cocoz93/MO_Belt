'use strict';
// U2.1 판정기 — /metrics 의 oversleep 히스토그램에서 워커별 p99 를 구해 게이트와 대조.
// 사용: node check-oversleep.js [metricsUrl] [p99상한µs]
//   게이트(플랜): 무부하 oversleep p99 ≤ 2000µs. 넘으면 그 런은 환경 오염으로 폐기.
const http = require('http');

const URL_  = process.argv[2] || 'http://127.0.0.1:19100/metrics';
const LIMIT = Number.isFinite(Number(process.argv[3])) ? Number(process.argv[3]) : 2000;

http.get(URL_, (res) => {
    let body = '';
    res.on('data', (d) => (body += d));
    res.on('end', () => judge(body));
}).on('error', (e) => { console.error('metrics 접속 실패: ' + e.message); process.exit(1); });

function judge(text) {
    // belt_game_oversleep_us_bucket{worker="0",le="50"} 123
    const buckets = {};   // worker → [{le, cum}]
    const wakes   = {};
    const skipped = {};
    for (const line of text.split('\n')) {
        let m = line.match(/^belt_game_oversleep_us_bucket\{worker="(\d+)",le="([^"]+)"\} (\d+)$/);
        if (m) {
            (buckets[m[1]] = buckets[m[1]] || []).push({
                le: m[2] === '+Inf' ? Infinity : Number(m[2]),
                cum: Number(m[3]),
            });
            continue;
        }
        m = line.match(/^belt_game_wakes_total\{worker="(\d+)"\} (\d+)$/);
        if (m) { wakes[m[1]] = Number(m[2]); continue; }
        m = line.match(/^belt_game_skipped_ticks_total\{worker="(\d+)"\} (\d+)$/);
        if (m) { skipped[m[1]] = Number(m[2]); }
    }

    const workers = Object.keys(buckets).sort((a, b) => a - b);
    if (workers.length === 0) { console.error('게임 워커 지표가 없음'); process.exit(1); }

    let fail = false;
    for (const w of workers) {
        const total = wakes[w] || 0;
        if (total < 100) { console.error(`worker ${w}: 표본 ${total}개 — 판정 불가(최소 100)`); fail = true; continue; }
        const target = Math.ceil(total * 0.99);
        const bs = buckets[w].sort((a, b) => a.le - b.le);
        let p99 = Infinity;
        for (const b of bs) { if (b.cum >= target) { p99 = b.le; break; } }
        const ok = p99 <= LIMIT;
        console.log(`worker ${w}: 표본 ${total} / p99 ≤ ${p99}µs / skipped ${skipped[w] || 0} — ${ok ? 'OK' : 'FAIL'}`);
        if (!ok) fail = true;
    }
    process.exit(fail ? 1 : 0);
}
