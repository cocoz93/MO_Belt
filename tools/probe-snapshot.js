'use strict';
// U2.3 프로브 — 4개 접속이 같은 방에 들어가 60Hz 입력을 보내며 3초간 스냅샷을 받는다.
// 검사: ① serverTick 단조 증가 ② actorCount 14 ③ 자기 lastInputSeq 가 보낸 seq 를 따라옴
//       ④ 스냅샷 수신율 ~20Hz(3초에 40장 이상) ⑤ PONG serverTick > 0
// 사용: node probe-snapshot.js [host] [port]
const net = require('net');

const HOST = process.argv[2] || '127.0.0.1';
const PORT = Number(process.argv[3]) || 15400;

const T = { JOIN: 10, INPUT: 11, PING: 12, JOIN_OK: 100, SNAPSHOT: 102, PONG: 103 };

function mkJoin() {
    const b = Buffer.alloc(6);
    b.writeUInt16LE(6, 0); b.writeUInt16LE(T.JOIN, 2); b.writeUInt16LE(1, 4);
    return b;
}
function mkInput(seq, mx) {
    const b = Buffer.alloc(12);
    b.writeUInt16LE(12, 0); b.writeUInt16LE(T.INPUT, 2);
    b.writeUInt32LE(seq, 4);
    b.writeInt8(mx, 8); b.writeInt8(0, 9); b.writeUInt8(0, 10); b.writeUInt8(0, 11);
    return b;
}
function mkPing(us) {
    const b = Buffer.alloc(12);
    b.writeUInt16LE(12, 0); b.writeUInt16LE(T.PING, 2);
    b.writeBigInt64LE(BigInt(us), 4);
    return b;
}
function reasm(onPacket) {
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

function client(i) {
    return new Promise((res) => {
        const st = {
            actorId: -1, roomId: -1, seq: 0, snaps: 0, lastTick: 0,
            monotonic: true, actorCountOk: true, seqSeen: 0, pongTick: 0,
        };
        const sock = net.connect({ host: HOST, port: PORT });
        const to = setTimeout(() => fin('timeout'), 12000);
        let inputTimer = null, pingTimer = null, endTimer = null;
        function fin(err) {
            clearTimeout(to);
            if (inputTimer) clearInterval(inputTimer);
            if (pingTimer) clearInterval(pingTimer);
            if (endTimer) clearTimeout(endTimer);
            sock.destroy();
            res({ err: err || null, st });
        }
        sock.on('error', (e) => fin('sock ' + e.code));
        sock.on('connect', () => sock.write(mkJoin()));
        sock.on('data', reasm((pkt) => {
            const type = pkt.readUInt16LE(2);
            if (type === T.JOIN_OK) {
                st.actorId = pkt.readUInt8(4);
                st.roomId  = pkt.readUInt32LE(5);
                // 60Hz 입력 (seq = 클라 틱)
                inputTimer = setInterval(() => sock.write(mkInput(++st.seq, 1)), 16);
                pingTimer  = setInterval(() => sock.write(mkPing(Date.now() * 1000)), 500);
                endTimer   = setTimeout(() => fin(null), 3000);
                return;
            }
            if (type === T.SNAPSHOT) {
                ++st.snaps;
                const tick = pkt.readUInt32LE(4);
                if (tick <= st.lastTick) st.monotonic = false;
                st.lastTick = tick;
                if (pkt.readUInt8(8) !== 14) st.actorCountOk = false;
                st.seqSeen = pkt.readUInt32LE(9 + st.actorId * 4);   // 내 슬롯의 lastInputSeq
                return;
            }
            if (type === T.PONG) {
                st.pongTick = pkt.readUInt32LE(12);
                return;
            }
        }));
    });
}

(async () => {
    const results = await Promise.all([client(0), client(1), client(2), client(3)]);
    let fail = false;
    const rooms = new Set();
    for (let i = 0; i < 4; ++i) {
        const { err, st } = results[i];
        rooms.add(st.roomId);
        const okSnaps = st.snaps >= 40;                       // 3초 × 20Hz = 60장 기대, 40장 하한
        const okSeq   = st.seqSeen > 0 && st.seq - st.seqSeen < 30;   // 최근 seq 를 따라오는가
        const okPong  = st.pongTick > 0;
        const ok = !err && st.monotonic && st.actorCountOk && okSnaps && okSeq && okPong;
        console.log(`conn${i}: actor ${st.actorId} room ${st.roomId} snaps ${st.snaps} ` +
                    `tick ${st.lastTick} seq ${st.seq}/에코 ${st.seqSeen} pongTick ${st.pongTick} — ` +
                    (ok ? 'OK' : `FAIL${err ? ' (' + err + ')' : ''}`));
        if (!ok) fail = true;
    }
    if (rooms.size !== 1) { console.error(`4명이 같은 방이 아님 (${rooms.size}개 방)`); fail = true; }
    if (fail) { console.error('PROBE FAIL'); process.exit(1); }
    console.log('snapshot probe OK');
    process.exit(0);
})();
