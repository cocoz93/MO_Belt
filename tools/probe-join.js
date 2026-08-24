'use strict';
// U2.2 프로브 — 사용: node probe-join.js [host] [port] [N] [concurrency]
//   1/3 은 JOIN 을 쏘자마자 끊는다(즉끊 — 넣을큐/뺄큐 인터리브 표적).
//   나머지는 JOIN_OK(actorId<4·roomId·serverTick)를 확인하고 끊는다.
//   버전 불일치 1건 → JOIN_FAIL(VersionMismatch) / 이중 JOIN 1건 → JOIN_FAIL(AlreadyJoined).
const net = require('net');

function argNum(i, def) {
    const v = Number(process.argv[i]);
    return Number.isFinite(v) ? v : def;
}
const HOST = process.argv[2] || '127.0.0.1';
const PORT = argNum(3, 15400);
const N    = argNum(4, 1000);
const C    = argNum(5, 50);

const T = { JOIN: 10, JOIN_OK: 100, JOIN_FAIL: 101 };

function mkJoin(ver) {
    const b = Buffer.alloc(6);
    b.writeUInt16LE(6, 0);
    b.writeUInt16LE(T.JOIN, 2);
    b.writeUInt16LE(ver, 4);
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

function joinOnce(i) {
    return new Promise((res) => {
        const dropEarly = i % 3 === 0;
        const sock = net.connect({ host: HOST, port: PORT });
        const to = setTimeout(() => fin('join timeout'), 8000);
        function fin(err) { clearTimeout(to); sock.destroy(); res(err || null); }
        sock.on('error', (e) => fin('join ' + e.code));
        sock.on('connect', () => {
            sock.write(mkJoin(1));
            if (dropEarly) fin(null);          // JOIN 직후 즉끊 — 응답을 안 기다린다
        });
        if (!dropEarly)
        {
            sock.on('data', reasm((pkt) => {
                const type = pkt.readUInt16LE(2);
                if (type !== T.JOIN_OK) return fin(`JOIN_OK 대신 type=${type}`);
                if (pkt.length !== 13) return fin(`JOIN_OK 크기 ${pkt.length}`);
                const actorId = pkt.readUInt8(4);
                if (actorId > 3) return fin(`actorId ${actorId} > 3`);
                pkt.readUInt32LE(5);   // roomId
                pkt.readUInt32LE(9);   // serverTick
                fin(null);
            }));
        }
    });
}

function expectFail(ver, expectReason, preJoin) {
    return new Promise((res) => {
        const sock = net.connect({ host: HOST, port: PORT });
        const to = setTimeout(() => fin('fail-case timeout'), 8000);
        function fin(err) { clearTimeout(to); sock.destroy(); res(err || null); }
        sock.on('error', (e) => fin('fail-case ' + e.code));
        let stage = 0;
        sock.on('connect', () => sock.write(mkJoin(preJoin ? 1 : ver)));
        sock.on('data', reasm((pkt) => {
            const type = pkt.readUInt16LE(2);
            if (preJoin && stage === 0) {
                if (type !== T.JOIN_OK) return fin(`선행 JOIN 실패 type=${type}`);
                stage = 1;
                sock.write(mkJoin(ver));       // 두 번째 JOIN → AlreadyJoined 기대
                return;
            }
            if (type !== T.JOIN_FAIL) return fin(`JOIN_FAIL 대신 type=${type}`);
            const reason = pkt.readUInt8(4);
            if (reason !== expectReason) return fin(`reason ${reason} != ${expectReason}`);
            fin(null);
        }));
    });
}

(async () => {
    let next = 0, fail = 0;
    const t0 = Date.now();
    async function worker() {
        for (;;) {
            const i = next++;
            if (i >= N) return;
            const e = await joinOnce(i);
            if (e) { ++fail; if (fail <= 5) console.error('  ' + e); }
        }
    }
    await Promise.all(Array.from({ length: C }, worker));
    console.log(`join ${N}회 (1/3 즉끊, 동시 ${C}) — 실패 ${fail}건, ${Date.now() - t0}ms`);

    const v = await expectFail(999, 1, false);   // VersionMismatch
    console.log(`version-mismatch — ${v ? v : 'OK'}`);
    const d = await expectFail(1, 3, true);      // AlreadyJoined
    console.log(`double-join — ${d ? d : 'OK'}`);

    if (fail || v || d) { console.error('PROBE FAIL'); process.exit(1); }
    console.log('join probe OK');
    process.exit(0);
})();
