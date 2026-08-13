'use strict';
// ─────────────────────────────────────────────────────────────
//  벨트스크롤 로직 — 좌표와 판정만 한다.
//
//  이 파일에는 브라우저 관련 코드가 한 줄도 없어야 한다.
//  (document / canvas / window / addEventListener / Math.random / Date)
//
//  나중에 이 파일을 C++로 옮기면 서버가 그대로 돌린다.
//  클라도 같은 계산을 미리 돌려서 화면에 먼저 보여준다(클라 예측).
//  둘이 조금이라도 다르면 화면이 계속 튄다.
//
//  C++로 옮길 때 주의: 실수형은 반드시 double. float를 쓰면 결과가 달라진다.
// ─────────────────────────────────────────────────────────────

// ── 틱 ──
// 프레임이 아니라 항상 이 간격으로만 계산한다.
const TICK_HZ = 60;
const TICK_DT = 1 / TICK_HZ;

// ── 무대 ──
// 벨트스크롤은 바닥이 띠(belt)다. x는 좌우로 길고, y는 앞뒤 깊이만 얕게 있다.
// 화면 높이가 아니라 "발이 놓이는 바닥의 앞뒤 폭"이 y다.
const WORLD_W = 1600;
const DEPTH_MIN = 0;     // 안쪽(위)
const DEPTH_MAX = 165;   // 앞쪽(아래)

// ── 이동 ──
// 깊이 이동이 가로보다 느리다. 벨트스크롤은 대개 이렇게 둔다 —
// 같은 속도면 비스듬히 걷는 게 이득이라 다들 대각선으로만 다닌다.
const P_SPEED_X = 215, P_SPEED_Y = 132;
const E_SPEED_X = 98,  E_SPEED_Y = 62;

// ── 몸 ──
// 서로 겹쳐 서지 못하게 밀어내는 크기. 그림 크기가 아니라 판정 크기다.
const BODY_HALF_W = 17;
const BODY_HALF_D = 9;

// ── 공격 ──
// 6프레임짜리 그림을 프레임당 5틱으로 돌려서 30틱(0.5초).
// 판정은 칼이 앞으로 나온 순간에만 한 번 — 누르자마자 맞으면 손맛이 죽는다.
const ATK_TICKS     = 30;
const ATK_HIT_TICK  = 11;
const ATK_RANGE     = 64;   // 바라보는 쪽으로 뻗는 거리
const ATK_BACK      = 8;    // 등 뒤로도 조금 (딱 붙었을 때 헛치는 것 방지)
const ATK_DEPTH     = 26;   // 깊이 허용 오차 — 좁으면 줄 맞추기가 짜증난다
const ATK_COOLDOWN  = 6;    // 다음 공격까지

// ── 피격 ──
const HURT_TICKS = 20;
// 넉백은 사거리(64)보다 훨씬 짧아야 한다. 크게 두면 한 대 때릴 때마다
// 상대가 사거리 밖으로 밀려나 다음 타가 헛친다 — 몰아붙이는 맛이 사라진다.
const KNOCK_X    = 12;
const DEATH_TICKS = 24;  // 다 돌면 그 자세로 멈춘다

// ── 적 ──
const E_ATK_RANGE  = 52;
const E_ATK_DEPTH  = 22;
const E_THINK_GAP  = 70;   // 이 간격으로만 공격 시도 (계속 때리면 빠져나갈 틈이 없다)

// ── 상태 ──
const ST_IDLE = 0, ST_WALK = 1, ST_ATTACK = 2, ST_HURT = 3, ST_DEAD = 4;

const KIND_PLAYER = 0, KIND_ORC = 1;

function clamp(v, lo, hi) { return v < lo ? lo : (v > hi ? hi : v); }

function makeActor(id, kind, x, y, hp) {
    return {
        id, kind,
        x, y,
        hp, maxHp: hp,
        face: 1,          // 1 = 오른쪽, -1 = 왼쪽
        state: ST_IDLE,
        stateTick: 0,     // 현재 상태에 들어온 뒤 지난 틱
        hitDone: false,   // 이번 공격에서 판정을 이미 냈는지
        cool: 0,          // 다음 공격까지 남은 틱
        think: 0          // 적 전용 — 다음 판단까지
    };
}

// 적 배치는 고정이다. 난수를 쓰면 서버와 클라가 갈린다.
const SPAWNS = [
    [520,  40], [700, 120], [980,  70],
    [1180, 140], [1330, 55]
];

function createWorld() {
    const w = { tick: 0, actors: [], nextId: 1, cleared: false };
    w.player = makeActor(w.nextId++, KIND_PLAYER, 180, 100, 10);
    w.actors.push(w.player);
    for (const s of SPAWNS) {
        w.actors.push(makeActor(w.nextId++, KIND_ORC, s[0], s[1], 3));
    }
    return w;
}

function setState(a, st) {
    if (a.state === st) return;
    a.state = st;
    a.stateTick = 0;
    a.hitDone = false;
}

// 움직일 수 있는 상태인가 (공격·피격·사망 중엔 조작이 안 먹는다)
function canAct(a) { return a.state === ST_IDLE || a.state === ST_WALK; }

// 몸끼리 겹치지 않게 밀어낸다. 벨트스크롤에서 이게 없으면 다 한 점에 모인다.
function separate(w) {
    for (let i = 0; i < w.actors.length; i++) {
        const a = w.actors[i];
        if (a.state === ST_DEAD) continue;
        for (let j = i + 1; j < w.actors.length; j++) {
            const b = w.actors[j];
            if (b.state === ST_DEAD) continue;
            const dx = b.x - a.x, dy = b.y - a.y;
            const ox = BODY_HALF_W * 2 - Math.abs(dx);
            const oy = BODY_HALF_D * 2 - Math.abs(dy);
            if (ox <= 0 || oy <= 0) continue;
            // 덜 파고든 축으로만 밀어낸다 — 두 축 다 밀면 캐릭터가 튄다
            if (ox < oy) {
                const push = (dx >= 0 ? 1 : -1) * ox * 0.5;
                a.x -= push; b.x += push;
            } else {
                const push = (dy >= 0 ? 1 : -1) * oy * 0.5;
                a.y -= push; b.y += push;
            }
        }
    }
    for (const a of w.actors) {
        a.x = clamp(a.x, 20, WORLD_W - 20);
        a.y = clamp(a.y, DEPTH_MIN, DEPTH_MAX);
    }
}

// 공격 판정 — 때린 쪽 기준으로 앞쪽 상자를 만들고 그 안에 든 상대를 친다.
function resolveHit(w, attacker, range, depth, dmg) {
    const x0 = attacker.face > 0 ? attacker.x - ATK_BACK : attacker.x - range;
    const x1 = attacker.face > 0 ? attacker.x + range   : attacker.x + ATK_BACK;
    let hit = 0;
    for (const t of w.actors) {
        if (t === attacker || t.state === ST_DEAD) continue;
        if (t.kind === attacker.kind) continue;          // 같은 편은 안 맞는다
        // 맞고 비틀거리는 동안은 다시 안 맞는다. 이게 없으면 적 여럿에게 둘러싸였을 때
        // 경직에서 못 빠져나오고 그대로 죽는다(다구리 즉사).
        if (t.state === ST_HURT) continue;
        if (t.x < x0 || t.x > x1) continue;
        if (Math.abs(t.y - attacker.y) > depth) continue;

        t.hp -= dmg;
        if (t.hp <= 0) {
            t.hp = 0;
            setState(t, ST_DEAD);
        } else {
            setState(t, ST_HURT);
            t.x = clamp(t.x + attacker.face * KNOCK_X, 20, WORLD_W - 20);  // 때린 쪽 반대로 밀린다
            t.face = -attacker.face;                      // 맞으면 때린 쪽을 본다
        }
        hit++;
    }
    return hit;
}

// input = { mx:-1|0|1, my:-1|0|1, attack:bool }
function step(w, input) {
    w.tick++;

    // ── 플레이어 ──
    const p = w.player;
    if (p.state !== ST_DEAD) {
        if (p.cool > 0) p.cool--;

        if (canAct(p) && input.attack && p.cool === 0) {
            setState(p, ST_ATTACK);
            p.cool = ATK_TICKS + ATK_COOLDOWN;
        } else if (canAct(p)) {
            const mx = input.mx, my = input.my;
            if (mx !== 0 || my !== 0) {
                // 대각선이라고 빨라지지 않게 — 두 축을 같이 누르면 각 축을 줄인다
                const k = (mx !== 0 && my !== 0) ? 0.7071067811865476 : 1;
                p.x += mx * P_SPEED_X * TICK_DT * k;
                p.y += my * P_SPEED_Y * TICK_DT * k;
                if (mx !== 0) p.face = mx > 0 ? 1 : -1;
                setState(p, ST_WALK);
            } else {
                setState(p, ST_IDLE);
            }
        }
    }

    // ── 적 ──
    for (const e of w.actors) {
        if (e.kind !== KIND_ORC || e.state === ST_DEAD) continue;
        if (e.cool > 0) e.cool--;
        if (e.think > 0) e.think--;

        if (!canAct(e)) continue;
        if (p.state === ST_DEAD) { setState(e, ST_IDLE); continue; }

        const dx = p.x - e.x, dy = p.y - e.y;
        const inRange = Math.abs(dx) <= E_ATK_RANGE && Math.abs(dy) <= E_ATK_DEPTH;

        if (inRange) {
            e.face = dx >= 0 ? 1 : -1;
            if (e.cool === 0 && e.think === 0) {
                setState(e, ST_ATTACK);
                e.cool = ATK_TICKS + ATK_COOLDOWN;
                e.think = E_THINK_GAP;
            } else {
                setState(e, ST_IDLE);
            }
        } else {
            // 먼저 깊이를 맞추고 다가간다 — 안 그러면 비스듬히 붙어서 계속 헛친다
            let mx = 0, my = 0;
            if (Math.abs(dy) > 6) my = dy > 0 ? 1 : -1;
            if (Math.abs(dx) > E_ATK_RANGE - 12) mx = dx > 0 ? 1 : -1;
            const k = (mx !== 0 && my !== 0) ? 0.7071067811865476 : 1;
            e.x += mx * E_SPEED_X * TICK_DT * k;
            e.y += my * E_SPEED_Y * TICK_DT * k;
            if (mx !== 0) e.face = mx > 0 ? 1 : -1;
            setState(e, (mx !== 0 || my !== 0) ? ST_WALK : ST_IDLE);
        }
    }

    // ── 상태 진행 · 판정 ──
    for (const a of w.actors) {
        a.stateTick++;

        if (a.state === ST_ATTACK) {
            if (!a.hitDone && a.stateTick >= ATK_HIT_TICK) {
                a.hitDone = true;
                if (a.kind === KIND_PLAYER) resolveHit(w, a, ATK_RANGE, ATK_DEPTH, 1);
                else                        resolveHit(w, a, E_ATK_RANGE, E_ATK_DEPTH, 1);
            }
            if (a.stateTick >= ATK_TICKS) setState(a, ST_IDLE);
        } else if (a.state === ST_HURT) {
            if (a.stateTick >= HURT_TICKS) setState(a, ST_IDLE);
        }
        // ST_DEAD 는 풀리지 않는다 — 마지막 자세로 남는다
    }

    separate(w);

    w.cleared = w.actors.every(a => a.kind !== KIND_ORC || a.state === ST_DEAD);
    return w;
}

// 브라우저에서는 전역으로, 나중에 노드 검사에서는 module로 쓴다.
if (typeof module !== 'undefined') {
    module.exports = { createWorld, step, TICK_HZ, TICK_DT, WORLD_W,
                       DEPTH_MIN, DEPTH_MAX, ST_IDLE, ST_WALK, ST_ATTACK, ST_HURT, ST_DEAD,
                       KIND_PLAYER, KIND_ORC, ATK_TICKS, HURT_TICKS, DEATH_TICKS };
}
