'use strict';
// U1.3 프로브 — 사용: node probe-echo.js [host] [port] [conns]
//   ① ECHO 왕복(바이트 일치) × conns개 연결 (SO_REUSEPORT 배분 확인 겸)
//   ② PING → PONG 필드(clientTimeUs 에코·serverTick·tickRemainUs 존재)
//   ③ 파서 하드닝: 깨진 헤더(size=9999) → 서버가 끊는가
//   ④ 모르는 타입 → 서버가 끊는가
// 전부 통과 exit 0, 하나라도 실패 exit 1.
const net = require('net');

const HOST  = process.argv[2] || '127.0.0.1';
const PORT  = Number(process.argv[3]) || 15400;
const CONNS = Number(process.argv[4]) || 8;

const T = { ECHO: 1, PING: 12, PONG: 103 };

function mkMsg(type, payload) {
    const size = 4 + payload.length;
    const b = Buffer.alloc(size);
    b.writeUInt16LE(size, 0);
    b.writeUInt16LE(type, 2);
    payload.copy(b, 4);
    return b;
}

// 길이 프리픽스 재조립기
function makeReassembler(onPacket) {
    let acc = Buffer.alloc(0);
    return (chunk) => {
        acc = Buffer.concat([acc, chunk]);
        for (;;) {
            if (acc.length < 4) return;
            const size = acc.readUInt16LE(0);
            if (acc.length < size) return;
            onPacket(acc.subarray(0, size));
            acc = acc.subarray(size);
        }
    };
}

function withConn(name, fn) {
    return new Promise((resolve) => {
        const sock = net.connect({ host: HOST, port: PORT }, () => fn(sock, done));
        const timer = setTimeout(() => done(`${name}: 5초 타임아웃`), 5000);
        let finished = false;
        function done(err) {
            if (finished) return;
            finished = true;
            clearTimeout(timer);
            sock.destroy();
            resolve(err || null);
        }
        sock.on('error', (e) => done(`${name}: 소켓 에러 ${e.code}`));
        sock.dummyDone = done;
    });
}

// ① + ② 정상 왕복
function normalCase(i) {
    const name = `conn${i}`;
    return withConn(name, (sock, done) => {
        const payload = Buffer.from(`belt-probe-${i}-0123456789`);
        const echo = mkMsg(T.ECHO, payload);
        const pingPayload = Buffer.alloc(8);
        const stamp = BigInt(1000000 + i);
        pingPayload.writeBigInt64LE(stamp, 0);
        const ping = mkMsg(T.PING, pingPayload);

        let gotEcho = false;
        sock.on('data', makeReassembler((pkt) => {
            const type = pkt.readUInt16LE(2);
            if (!gotEcho) {
                if (type !== T.ECHO) return done(`${name}: ECHO 대신 type=${type}`);
                if (!pkt.equals(echo)) return done(`${name}: ECHO 바이트 불일치`);
                gotEcho = true;
                sock.write(ping);
                return;
            }
            if (type !== T.PONG) return done(`${name}: PONG 대신 type=${type}`);
            if (pkt.length !== 20) return done(`${name}: PONG 크기 ${pkt.length} != 20`);
            if (pkt.readBigInt64LE(4) !== stamp) return done(`${name}: clientTimeUs 에코 불일치`);
            // serverTick(12)·tickRemainUs(16)는 U2.1 전까지 0 — 읽히기만 하면 된다
            pkt.readUInt32LE(12);
            pkt.readUInt32LE(16);
            done(null);
        }));
        sock.write(echo);
    });
}

// ③ 깨진 헤더 → 서버가 끊어야 함
function badHeaderCase() {
    return withConn('bad-header', (sock, done) => {
        sock.on('close', () => done(null));
        sock.on('data', () => done('bad-header: 응답이 오면 안 됨'));
        const b = Buffer.alloc(8);
        b.writeUInt16LE(9999, 0);     // MAX_PACKET_SIZE(1024) 초과
        b.writeUInt16LE(1, 2);
        sock.write(b);
    });
}

// ④ 모르는 타입 → 서버가 끊어야 함
function unknownTypeCase() {
    return withConn('unknown-type', (sock, done) => {
        sock.on('close', () => done(null));
        sock.on('data', () => done('unknown-type: 응답이 오면 안 됨'));
        sock.write(mkMsg(777, Buffer.from('x')));
    });
}

(async () => {
    const tasks = [];
    for (let i = 0; i < CONNS; ++i) tasks.push(normalCase(i));
    tasks.push(badHeaderCase());
    tasks.push(unknownTypeCase());

    const results = await Promise.all(tasks);
    const errors = results.filter(Boolean);
    if (errors.length) {
        console.error('PROBE FAIL:');
        for (const e of errors) console.error('  ' + e);
        process.exit(1);
    }
    console.log(`probe OK — 정상 왕복 ${CONNS}건 + 하드닝 2건`);
    process.exit(0);
})();
