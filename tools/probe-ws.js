'use strict';
// U4.1 프로브 — 웹 경로(WS → 릴레이 → TCP 서버) 전체를 node 로 검증.
//   브라우저 대신 ws 모듈로 같은 belt-proto 를 태워 JOIN → 이동 입력 → 스냅샷 수신·qx 증가 확인.
// 사용: node probe-ws.js [wsUrl]
const path = require('path');
const WebSocket = require(path.join(__dirname, '..', 'web', 'node_modules', 'ws'));
const P = require(path.join(__dirname, '..', 'web', 'belt-proto.js'));

const WS_URL = process.argv[2] || 'ws://127.0.0.1:9010';

const ws = new WebSocket(WS_URL);
ws.binaryType = 'arraybuffer';

let actorId = -1, seq = 0, snapCount = 0, firstX = -1, lastX = -1, gotPong = false;
const to = setTimeout(() => fin('timeout'), 10000);
let inputTimer = null;

function fin(err) {
    clearTimeout(to);
    if (inputTimer) clearInterval(inputTimer);
    ws.close();
    if (err) { console.error('WS PROBE FAIL: ' + err); process.exit(1); }
    console.log(`ws probe OK — snaps ${snapCount}, x ${firstX.toFixed(1)}→${lastX.toFixed(1)}, pong ${gotPong}`);
    process.exit(0);
}

const re = new P.Reassembler((dv) => {
    const m = P.decode(dv);
    if (m.type === P.MSG.S2C_JOIN_OK) {
        actorId = m.actorId;
        inputTimer = setInterval(() => ws.send(P.enc.input(++seq, 1, 0, 0, 0)), 16);
        ws.send(P.enc.ping(Date.now() * 1000));
        return;
    }
    if (m.type === P.MSG.S2C_PONG) { gotPong = true; return; }
    if (m.type === P.MSG.S2C_SNAPSHOT) {
        ++snapCount;
        if (actorId >= 0) {
            const me = m.actors[actorId];
            if (firstX < 0) firstX = me.x;
            lastX = me.x;
        }
        if (snapCount >= 30) {
            if (!(lastX - firstX > 30)) return fin(`qx 증가 없음 (${firstX}→${lastX})`);
            if (!gotPong) return fin('PONG 미수신');
            fin(null);
        }
    }
});

ws.on('open', () => ws.send(P.enc.join()));
ws.on('message', (data) => re.push(new Uint8Array(data)));
ws.on('error', (e) => fin('ws ' + e.message));
