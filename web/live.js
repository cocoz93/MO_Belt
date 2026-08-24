'use strict';
// 서버 추종 웹 클라 (육안·시연 전용 — 측정은 더미 몫).
//   · 스냅샷(20Hz)을 보간 버퍼 100ms 로 그린다 — 보간 없이 그리면 20fps 계단이 "랙"으로 보인다.
//   · 내 캐릭터도 서버 추종(1차). 예측·재조정은 2차에서 logic.js 재사용으로 들어온다.
//   · ?probe: 헤드리스 검증 모드 — rAF 가 안 도는 환경이라 고정 스텝(setInterval)으로 그리고,
//     자동으로 오른쪽 입력을 보내 "접속 + 이동"을 title/data-status 로 노출한다.
/* global BeltProto */
(() => {
    const P = BeltProto;
    const q = new URLSearchParams(location.search);
    const WS_URL = q.get('ws') || 'ws://127.0.0.1:9010';
    const PROBE  = q.has('probe');

    const cv  = document.getElementById('cv');
    const ctx = cv.getContext('2d');
    const hud = document.getElementById('hud');

    const INTERP_MS = 100;          // 스냅샷 2장(20Hz)만큼 뒤에서 그린다
    const FLOOR_TOP = 300;          // game.js 와 같은 무대 매핑
    const WORLD_W   = 1600;

    // ── 상태 ──
    let ws = null;
    let myActorId = -1, roomId = -1;
    let seq = 0;
    let snaps = [];                 // {at(수신 ms), serverTick, actors[]}
    let snapCount = 0, moveEvents = 0;
    let rttMs = 0, lastServerTick = 0;
    let firstMyX = -1, lastMyX = -1;

    const KEY = {};
    if (!PROBE) {
        addEventListener('keydown', (e) => {
            KEY[e.code] = true;
            if (['ArrowLeft','ArrowRight','ArrowUp','ArrowDown','Space'].includes(e.code)) e.preventDefault();
        });
        addEventListener('keyup', (e) => { KEY[e.code] = false; });
    }
    function readInput() {
        if (PROBE)   // 검증 모드: 접속 2초간 오른쪽으로 걷는다 (출구 1400 에는 한참 못 미침)
            return { mx: snapCount < 40 ? 1 : 0, my: 0, attack: 0, skill: 0 };
        let mx = 0, my = 0;
        if (KEY.ArrowLeft || KEY.KeyA) mx = -1;
        if (KEY.ArrowRight || KEY.KeyD) mx = 1;
        if (KEY.ArrowUp || KEY.KeyW) my = -1;
        if (KEY.ArrowDown || KEY.KeyS) my = 1;
        return { mx, my, attack: (KEY.KeyJ || KEY.Space) ? 1 : 0, skill: KEY.KeyK ? 1 : 0 };
    }

    // ── 접속 ──
    function connect() {
        ws = new WebSocket(WS_URL);
        ws.binaryType = 'arraybuffer';
        const re = new P.Reassembler(onPacket);
        ws.onopen    = () => ws.send(P.enc.join());
        ws.onmessage = (ev) => re.push(new Uint8Array(ev.data));
        ws.onclose   = () => { hud.textContent = '연결 끊김 — 1.5초 뒤 재접속'; setTimeout(connect, 1500); };
    }

    function onPacket(dv) {
        const m = P.decode(dv);
        if (m.type === P.MSG.S2C_JOIN_OK) {
            myActorId = m.actorId; roomId = m.roomId;
            setInterval(() => { const i = readInput(); ws.send(P.enc.input(++seq, i.mx, i.my, i.attack, i.skill)); }, 16);
            setInterval(() => ws.send(P.enc.ping(Date.now() * 1000)), 500);
            return;
        }
        if (m.type === P.MSG.S2C_JOIN_FAIL) { hud.textContent = `입장 거절 (reason ${m.reason})`; return; }
        if (m.type === P.MSG.S2C_PONG) {
            rttMs = Date.now() - Number(m.clientTimeUs / 1000n);
            return;
        }
        if (m.type === P.MSG.S2C_ROOM_EVENT) {
            if (m.kind === 3) { roomId = m.roomId; snaps = []; ++moveEvents; }   // 새 방 — 보간 버퍼 리셋
            return;
        }
        if (m.type === P.MSG.S2C_SNAPSHOT) {
            ++snapCount;
            lastServerTick = m.serverTick;
            snaps.push({ at: Date.now(), serverTick: m.serverTick, actors: m.actors });
            if (snaps.length > 30) snaps.shift();
            if (myActorId >= 0) {
                const me = m.actors[myActorId];
                if (firstMyX < 0) firstMyX = me.x;
                lastMyX = me.x;
            }
        }
    }

    // ── 보간 ──
    function sampleActors() {
        if (snaps.length === 0) return null;
        const t = Date.now() - INTERP_MS;
        let a = snaps[0], b = snaps[snaps.length - 1];
        for (let i = 0; i < snaps.length - 1; ++i)
            if (snaps[i].at <= t && t <= snaps[i + 1].at) { a = snaps[i]; b = snaps[i + 1]; break; }
        const span = b.at - a.at;
        const r = span > 0 ? Math.min(1, Math.max(0, (t - a.at) / span)) : 1;
        return a.actors.map((av, i) => {
            const bv = b.actors[i];
            if (av.state === 255 || bv.state === 255) return bv;
            return { ...bv, x: av.x + (bv.x - av.x) * r, y: av.y + (bv.y - av.y) * r };
        });
    }

    // ── 렌더 (스프라이트는 assets/ 가 있으면 쓰고, 없으면 사각형 폴백) ──
    const sprites = { soldier: null, orc: null };
    function tryLoad(name, file) {
        const img = new Image();
        img.onload = () => { sprites[name] = img; };
        img.src = `../assets/${name}/${file}`;
    }
    tryLoad('soldier', 'Soldier_Idle.png');
    tryLoad('orc', 'Orc_Idle.png');

    function draw() {
        ctx.fillStyle = '#23262e';
        ctx.fillRect(0, 0, cv.width, cv.height);

        const actors = sampleActors();
        // 바닥 띠
        ctx.fillStyle = '#2d3140';
        ctx.fillRect(0, FLOOR_TOP - 20, cv.width, 165 + 80);

        let camX = 0;
        if (actors && myActorId >= 0 && actors[myActorId].state !== 255)
            camX = Math.min(Math.max(actors[myActorId].x - 480, 0), WORLD_W - 960);

        // 출구 표시
        const exitX = 1400 - camX;
        if (exitX > -40 && exitX < 1000) {
            ctx.fillStyle = 'rgba(120,220,120,0.15)';
            ctx.fillRect(exitX, FLOOR_TOP - 20, 960 - exitX, 245);
        }

        if (actors) {
            const order = actors.map((a, i) => ({ a, i })).filter(o => o.a.state !== 255)
                                .sort((p, q2) => p.a.y - q2.a.y);
            for (const { a, i } of order) {
                const sx = a.x - camX, sy = FLOOR_TOP + a.y;
                const img = a.kind === 0 ? sprites.soldier : sprites.orc;
                if (img) {
                    ctx.save();
                    ctx.translate(sx, sy);
                    if (a.face < 0) ctx.scale(-1, 1);
                    ctx.drawImage(img, 0, 0, 100, 100, -49 * 1.4, -59 * 1.4, 140, 140);
                    ctx.restore();
                } else {
                    ctx.fillStyle = a.kind === 1 ? '#c05050' : (i === myActorId ? '#f5d04a' : '#4a8bf5');
                    ctx.fillRect(sx - 17, sy - 48, 34, 48);
                }
                // hp 핍
                ctx.fillStyle = '#3c3';
                for (let h = 0; h < a.hp; ++h) ctx.fillRect(sx - 17 + h * 4, sy - 56, 3, 4);
                if (i === myActorId) { ctx.fillStyle = '#fd6'; ctx.fillRect(sx - 2, sy - 64, 4, 4); }
            }
        }

        hud.innerHTML = `방 <b>${roomId}</b> · 내 슬롯 <b>${myActorId}</b> · tick <b>${lastServerTick}</b>` +
                        ` · 스냅샷 <b>${snapCount}</b> · RTT <b>${rttMs}ms</b>` +
                        (moveEvents ? ` · 방이동 <b>${moveEvents}</b>회` : '') +
                        `<br>이동 방향키/WASD (전투는 2차) — 전원이 오른쪽 초록 구역에 서면 다음 방`;

        if (PROBE) {
            const moved = firstMyX >= 0 && (lastMyX - firstMyX) > 50;
            const status = (snapCount >= 60 && moved) ? 'ok' : 'wait';
            document.title = `belt-live ${status} snaps=${snapCount} dx=${(lastMyX - firstMyX).toFixed(1)} rtt=${rttMs}`;
            document.body.setAttribute('data-status', status);
        }
    }

    if (PROBE)
        setInterval(draw, 100);            // 헤드리스는 rAF 가 안 돈다 — 고정 스텝
    else
        (function loop() { draw(); requestAnimationFrame(loop); })();

    connect();
})();
