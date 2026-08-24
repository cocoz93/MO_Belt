'use strict';
// U3.1 프로브 — 4클라 한 방에서 오른쪽으로 계속 이동:
//   ① 내 qx 가 증가한다 (이동 수식)  ② 오크가 밀린다 (separate 작동)
//   ③ 전원이 출구(x ≥ 1520)에 닿으면 ROOM_EVENT(RoomMove) + 새 방에서 스폰 위치로 복귀
//   ④ serverTick 은 방 이동 후에도 이어 센다 (되감기지 않음)
// 사용: node probe-move.js [host] [port]
const net = require('net');

const HOST = process.argv[2] || '127.0.0.1';
const PORT = Number(process.argv[3]) || 15400;

const T = { JOIN: 10, INPUT: 11, JOIN_OK: 100, SNAPSHOT: 102, ROOM_EVENT: 104 };
const Q = 32;                       // 좌표 눈금 1/32px
const ORC_SPAWN_QX = [520, 700, 980, 1180, 1330, 620, 840, 1060, 1240, 1450].map(x => x * Q);

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
            actorId: -1, room0: -1, seq: 0, firstQx: -1, lastQx: -1, maxTickBefore: 0,
            qxIncreased: false, orcPushed: false,
            moved: false, newRoom: -1, spawnQX: -1, afterSnapOk: false, tickContinued: false,
        };
        const sock = net.connect({ host: HOST, port: PORT });
        const to = setTimeout(() => fin('timeout(이동이 30초 내 안 끝남)'), 30000);
        let inputTimer = null;
        function fin(err) {
            clearTimeout(to);
            if (inputTimer) clearInterval(inputTimer);
            sock.destroy();
            res({ err: err || null, st });
        }
        sock.on('error', (e) => fin('sock ' + e.code));
        sock.on('connect', () => sock.write(mkJoin()));
        sock.on('data', reasm((pkt) => {
            const type = pkt.readUInt16LE(2);
            if (type === T.JOIN_OK) {
                st.actorId = pkt.readUInt8(4);
                st.room0   = pkt.readUInt32LE(5);
                inputTimer = setInterval(() => sock.write(mkInput(++st.seq, 1)), 16);  // 오른쪽 유지
                return;
            }
            if (type === T.SNAPSHOT) {
                const tick = pkt.readUInt32LE(4);
                const base = 9 + 16;                       // actors 시작
                const me   = base + st.actorId * 10;
                const qx   = pkt.readUInt16LE(me + 4);
                if (!st.moved) {
                    if (st.firstQx < 0) st.firstQx = qx;
                    if (qx > st.lastQx) st.qxIncreased = true;
                    st.lastQx = qx;
                    st.maxTickBefore = tick;
                    // 오크 10기 전체를 훑어 하나라도 스폰에서 1px 이상 밀렸으면 separate 작동
                    for (let o = 0; o < 10 && !st.orcPushed; ++o) {
                        const oQx = pkt.readUInt16LE(base + (4 + o) * 10 + 4);
                        if (Math.abs(oQx - ORC_SPAWN_QX[o]) > Q) st.orcPushed = true;
                    }
                } else {
                    // 이동 후 첫 스냅샷: 스폰 근처 + 틱 이어짐.
                    // 입력(mx=1)을 계속 보내는 중이라 스냅샷까지 최대 3틱(≈11px)+밀어내기만큼
                    // 이미 걸어 있다 — 허용오차 ±20px.
                    if (!st.afterSnapOk) {
                        st.afterSnapOk = Math.abs(qx - st.spawnQX) <= 20 * Q;
                        st.tickContinued = tick > st.maxTickBefore;
                        fin(null);                          // 검사 끝
                    }
                }
                return;
            }
            if (type === T.ROOM_EVENT) {
                const kind = pkt.readUInt8(4);
                if (kind !== 3) return;
                st.moved   = true;
                st.newRoom = pkt.readUInt32LE(6);
                st.spawnQX = pkt.readUInt16LE(10);
                return;
            }
        }));
    });
}

(async () => {
    const results = await Promise.all([client(0), client(1), client(2), client(3)]);
    let fail = false;
    for (let i = 0; i < 4; ++i) {
        const { err, st } = results[i];
        const ok = !err && st.qxIncreased && st.orcPushed && st.moved &&
                   st.newRoom !== st.room0 && st.afterSnapOk && st.tickContinued;
        console.log(`conn${i}: actor ${st.actorId} 방 ${st.room0}→${st.newRoom} ` +
                    `qx ${st.firstQx}→${st.lastQx} 오크밀림 ${st.orcPushed} ` +
                    `스폰복귀 ${st.afterSnapOk} 틱연속 ${st.tickContinued} — ` +
                    (ok ? 'OK' : `FAIL${err ? ' (' + err + ')' : ''}`));
        if (!ok) fail = true;
    }
    if (fail) { console.error('PROBE FAIL'); process.exit(1); }
    console.log('move probe OK');
    process.exit(0);
})();
