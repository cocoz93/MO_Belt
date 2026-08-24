'use strict';
// Belt 와이어 규약의 JS 거울 — server/src/Protocol.h 와 값·레이아웃이 짝이다.
// enum 값은 손으로 박는다(서버와 동일 원칙). 브라우저·node 공용.
(function (root, factory) {
    if (typeof module === 'object' && module.exports) module.exports = factory();
    else root.BeltProto = factory();
})(typeof self !== 'undefined' ? self : this, function () {

    const MSG = {
        ECHO: 1,
        C2S_JOIN: 10, C2S_INPUT: 11, C2S_PING: 12,
        S2C_JOIN_OK: 100, S2C_JOIN_FAIL: 101, S2C_SNAPSHOT: 102, S2C_PONG: 103, S2C_ROOM_EVENT: 104,
    };
    const PROTOCOL_VERSION = 1;
    const POS_QUANT = 32;             // 1/32px — 서버 kPosQuantScale 과 짝
    const SNAP_ACTORS = 14;

    function mkHeader(size, type) {
        const b = new ArrayBuffer(size);
        const d = new DataView(b);
        d.setUint16(0, size, true);
        d.setUint16(2, type, true);
        return { b, d };
    }

    const enc = {
        join() {
            const { b, d } = mkHeader(6, MSG.C2S_JOIN);
            d.setUint16(4, PROTOCOL_VERSION, true);
            return b;
        },
        input(seq, mx, my, attack, skill) {
            const { b, d } = mkHeader(12, MSG.C2S_INPUT);
            d.setUint32(4, seq >>> 0, true);
            d.setInt8(8, mx); d.setInt8(9, my);
            d.setUint8(10, attack ? 1 : 0); d.setUint8(11, skill ? 1 : 0);
            return b;
        },
        ping(timeUs) {
            const { b, d } = mkHeader(12, MSG.C2S_PING);
            d.setBigInt64(4, BigInt(timeUs), true);
            return b;
        },
    };

    // pkt: DataView (한 프레임 전체)
    function decode(d) {
        const type = d.getUint16(2, true);
        switch (type) {
            case MSG.S2C_JOIN_OK:
                return { type, actorId: d.getUint8(4), roomId: d.getUint32(5, true),
                         serverTick: d.getUint32(9, true) };
            case MSG.S2C_JOIN_FAIL:
                return { type, reason: d.getUint8(4) };
            case MSG.S2C_PONG:
                return { type, clientTimeUs: d.getBigInt64(4, true),
                         serverTick: d.getUint32(12, true), tickRemainUs: d.getUint32(16, true) };
            case MSG.S2C_ROOM_EVENT:
                return { type, kind: d.getUint8(4), actorId: d.getUint8(5),
                         roomId: d.getUint32(6, true),
                         spawnQX: d.getUint16(10, true), spawnQY: d.getUint16(12, true) };
            case MSG.S2C_SNAPSHOT: {
                const out = { type, serverTick: d.getUint32(4, true),
                              actorCount: d.getUint8(8), lastInputSeq: [], actors: [] };
                for (let i = 0; i < 4; ++i) out.lastInputSeq.push(d.getUint32(9 + i * 4, true));
                let off = 9 + 16;
                for (let i = 0; i < SNAP_ACTORS; ++i, off += 10) {
                    out.actors.push({
                        id: d.getUint8(off), kind: d.getUint8(off + 1), state: d.getUint8(off + 2),
                        face: d.getInt8(off + 3),
                        x: d.getUint16(off + 4, true) / POS_QUANT,
                        y: d.getUint16(off + 6, true) / POS_QUANT,
                        hp: d.getInt16(off + 8, true),
                    });
                }
                return out;
            }
            default:
                return { type };
        }
    }

    // 길이 프리픽스 재조립 — onPacket(DataView)
    function Reassembler(onPacket) {
        let acc = new Uint8Array(0);
        this.push = (chunk) => {
            const u = chunk instanceof Uint8Array ? chunk : new Uint8Array(chunk);
            const merged = new Uint8Array(acc.length + u.length);
            merged.set(acc, 0); merged.set(u, acc.length);
            acc = merged;
            for (;;) {
                if (acc.length < 4) return;
                const size = acc[0] | (acc[1] << 8);
                if (size < 4 || size > 4096) { acc = new Uint8Array(0); return; }   // 오염 — 통째 버림
                if (acc.length < size) return;
                onPacket(new DataView(acc.buffer, acc.byteOffset, size));
                acc = acc.subarray(size);
            }
        };
    }

    return { MSG, PROTOCOL_VERSION, POS_QUANT, SNAP_ACTORS, enc, decode, Reassembler };
});
