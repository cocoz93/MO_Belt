'use strict';
// U1.4 프로브 — 사용: node probe-churn.js [host] [port] [churnN] [concurrency] [floodMB]
//   ① churn: 접속→ECHO 1회→절단을 N회 (N > 풀 8192 로 잡아 슬롯 재사용·세대 증가를 강제)
//   ② burst: 1KB×64 를 몰아 보내고 읽기를 300ms 멈췄다 재개 — 바이트 전량·순서 무결
//            (커널 버퍼가 차면 서버의 EPOLLOUT 경로가 잔여를 잇는다)
//   ③ flood: 읽기를 영영 멈춘 채 수 MB — 서버가 송신링 가득에서 절단하는가
const net = require('net');

// 0 도 유효한 값("건너뜀")이라 || 기본값을 쓰면 안 된다 — 실측에서 0이 10000으로 둔갑했다
function argNum(i, def) {
    const v = Number(process.argv[i]);
    return Number.isFinite(v) ? v : def;
}
const HOST     = process.argv[2] || '127.0.0.1';
const PORT     = argNum(3, 15400);
const N        = argNum(4, 10000);
const C        = argNum(5, 100);
const FLOOD_MB = argNum(6, 48);    // WSL 은 tcp_rmem 상한 32MB — 그보다 커야 절단 조건에 닿는다

function mkEcho(payload) {
    const size = 4 + payload.length;
    const b = Buffer.alloc(size);
    b.writeUInt16LE(size, 0);
    b.writeUInt16LE(1, 2);        // ECHO
    payload.copy(b, 4);
    return b;
}

// ① churn 1회 — ECHO 왕복 확인 후 클라가 끊는다
function churnOne(i) {
    return new Promise((res) => {
        let acc = Buffer.alloc(0);
        const payload = Buffer.from('c' + i);
        const want = 4 + payload.length;
        const sock = net.connect({ host: HOST, port: PORT });
        const to = setTimeout(() => fin('churn timeout'), 8000);
        function fin(err) { clearTimeout(to); sock.destroy(); res(err || null); }
        sock.on('connect', () => sock.write(mkEcho(payload)));
        sock.on('data', (d) => {
            acc = Buffer.concat([acc, d]);
            if (acc.length >= want) fin(acc.length === want ? null : 'churn 크기 불일치');
        });
        sock.on('error', (e) => fin('churn ' + e.code));
    });
}

async function churn() {
    let next = 0, fail = 0;
    async function worker() {
        for (;;) {
            const i = next++;
            if (i >= N) return;
            const e = await churnOne(i);
            if (e) ++fail;
        }
    }
    await Promise.all(Array.from({ length: C }, worker));
    return fail;
}

// ② burst 무결성
function burstIntegrity() {
    return new Promise((res) => {
        const payload = Buffer.alloc(996, 0x42);
        const msg = mkEcho(payload);              // 1000B
        const count = 64;
        const expect = count * msg.length;
        let rcv = 0;
        const sock = net.connect({ host: HOST, port: PORT });
        const to = setTimeout(() => fin(`burst timeout rcv=${rcv}/${expect}`), 10000);
        function fin(err) { clearTimeout(to); sock.destroy(); res(err || null); }
        sock.on('connect', () => {
            for (let i = 0; i < count; ++i) sock.write(msg);
            sock.pause();                          // 읽기 정지 — 서버 쪽 버퍼를 채운다
            setTimeout(() => sock.resume(), 300);
        });
        sock.on('data', (d) => {
            rcv += d.length;
            if (rcv === expect) fin(null);
            else if (rcv > expect) fin('burst 바이트 초과');
        });
        sock.on('error', (e) => fin('burst ' + e.code));
    });
}

// ③ flood 절단
function floodCut() {
    return new Promise((res) => {
        const payload = Buffer.alloc(1016, 0x7a);
        const msg = mkEcho(payload);              // 1020B
        const total = Math.floor((FLOOD_MB * 1024 * 1024) / msg.length);
        const sock = net.connect({ host: HOST, port: PORT });
        const to = setTimeout(() => fin('flood: 서버가 20초 내 안 끊음'), 20000);
        function fin(err) { clearTimeout(to); sock.destroy(); res(err || null); }
        sock.on('connect', () => {
            sock.pause();                          // 영영 안 읽는다
            let i = 0;
            (function pump() {
                while (i < total) {
                    ++i;
                    if (!sock.write(msg)) { sock.once('drain', pump); return; }
                }
            })();
        });
        sock.on('error', () => {});                // 서버 절단이면 EPIPE/ECONNRESET — 정상
        sock.on('close', () => fin(null));         // 서버가 끊어 close 가 오면 통과
    });
}

(async () => {
    let fails = 0, b = null, f = null;

    if (N > 0) {
        const t0 = Date.now();
        fails = await churn();
        console.log(`churn ${N}회 (동시 ${C}) — 실패 ${fails}건, ${Date.now() - t0}ms`);
    } else {
        console.log('churn — 건너뜀');
    }

    b = await burstIntegrity();
    console.log(`burst — ${b ? b : 'OK (64KB 전량 수신)'}`);

    if (FLOOD_MB > 0) {
        f = await floodCut();
        console.log(`flood — ${f ? f : 'OK (서버가 절단)'}`);
    } else {
        console.log('flood — 건너뜀');
    }

    if (fails || b || f) { console.error('PROBE FAIL'); process.exit(1); }
    console.log('churn probe OK');
    process.exit(0);
})();
