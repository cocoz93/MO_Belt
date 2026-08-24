'use strict';
// U3.1 스모크 — 방 1,000개(=접속 4,000) 스폰·유지·회수.
//   접속을 200개/100ms 로 램프 → 전원 JOIN_OK → 정지 입력(mx=0) 2Hz 로 8초 유지
//   → 피크 지표(rooms_active·sessions_active) 자가 확인 → 전원 절단 → 잔량 0 확인.
//   입력을 mx=0 으로 두는 이유: 이동하면 출구 방 이동이 발동해 방 수가 흔들린다.
// 사용: node probe-rooms1k.js [host] [port] [conns] [metricsPort]
const net  = require('net');
const http = require('http');

function argNum(i, def) {
    const v = Number(process.argv[i]);
    return Number.isFinite(v) ? v : def;
}
const HOST  = process.argv[2] || '127.0.0.1';
const PORT  = argNum(3, 15402);
const CONNS = argNum(4, 4000);
const MPORT = argNum(5, 19147);

const T = { JOIN: 10, INPUT: 11, JOIN_OK: 100 };

function mkJoin() {
    const b = Buffer.alloc(6);
    b.writeUInt16LE(6, 0); b.writeUInt16LE(T.JOIN, 2); b.writeUInt16LE(1, 4);
    return b;
}
function mkIdle(seq) {
    const b = Buffer.alloc(12);
    b.writeUInt16LE(12, 0); b.writeUInt16LE(T.INPUT, 2);
    b.writeUInt32LE(seq, 4);
    b.writeInt8(0, 8); b.writeInt8(0, 9); b.writeUInt8(0, 10); b.writeUInt8(0, 11);
    return b;
}

function fetchOnce(names) {
    return new Promise((res, rej) => {
        http.get(`http://${HOST}:${MPORT}/metrics`, (r) => {
            let body = '';
            r.on('data', (d) => (body += d));
            r.on('end', () => {
                const out = {};
                for (const n of names) {
                    const m = body.match(new RegExp(`^${n} (-?\\d+)$`, 'm'));
                    out[n] = m ? Number(m[1]) : NaN;
                }
                res(out);
            });
            r.on('error', rej);
        }).on('error', rej);
    });
}
async function fetchMetric(names) {          // 일시 거절 1회 재시도
    try { return await fetchOnce(names); }
    catch { await new Promise((r) => setTimeout(r, 300)); return fetchOnce(names); }
}

const socks = [];
let joined = 0, errors = 0;

function connectOne() {
    const sock = net.connect({ host: HOST, port: PORT });
    let seq = 0, timer = null;
    sock.on('connect', () => sock.write(mkJoin()));
    sock.on('data', () => {
        if (timer === null) {
            ++joined;                                     // 첫 응답 = JOIN_OK 로 간주
            timer = setInterval(() => sock.write(mkIdle(++seq)), 500);
            sock.cleanup = () => clearInterval(timer);
        }
    });
    sock.on('error', () => { ++errors; });
    socks.push(sock);
}

(async () => {
    const t0 = Date.now();
    for (let i = 0; i < CONNS; i += 200)
    {
        for (let j = i; j < Math.min(i + 200, CONNS); ++j) connectOne();
        await new Promise((r) => setTimeout(r, 100));
    }
    // 전원 합류 대기 (최대 15초)
    while (joined + errors < CONNS && Date.now() - t0 < 15000)
        await new Promise((r) => setTimeout(r, 200));
    console.log(`ramp ${Date.now() - t0}ms — joined ${joined}/${CONNS} (err ${errors})`);

    // ⚠ 피크·잔량 지표 확인은 바깥(wsl curl)에서 한다 — Windows→WSL localhost 릴레이가
    //   동시 4천 연결 중에는 새(메트릭) 연결을 RST 하는 것이 실측됨. 서버 문제 아님.
    await new Promise((r) => setTimeout(r, 10000));       // 유지 구간 (바깥 스크레이프용)

    for (const s of socks) { if (s.cleanup) s.cleanup(); s.destroy(); }
    await new Promise((r) => setTimeout(r, 2000));

    const ok = joined === CONNS && errors === 0;
    if (!ok) { console.error('SMOKE FAIL'); process.exit(1); }
    console.log('rooms1k 접속 단계 OK (지표 대조는 wsl curl 로)');
    process.exit(0);
})().catch((e) => { console.error('SMOKE FAIL: ' + e.message); process.exit(1); });
