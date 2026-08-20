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
// 6프레임짜리 그림을 프레임당 3틱으로 돌려서 18틱(0.3초).
// 판정은 칼이 앞으로 나온 순간에만 한 번 — 누르자마자 맞으면 손맛이 죽는다.
const ATK_TICKS     = 18;
const ATK_HIT_TICK  = 7;
const ATK_RANGE     = 64;   // 바라보는 쪽으로 뻗는 거리
const ATK_BACK      = 8;    // 등 뒤로도 조금 (딱 붙었을 때 헛치는 것 방지)
const ATK_DEPTH     = 26;   // 깊이 허용 오차 — 좁으면 줄 맞추기가 짜증난다
const ATK_COOLDOWN  = 4;    // 다음 공격까지

// ── 어설트 ──
// 바라보는 쪽에서 **가장 먼** 적을 골라, 그를 지나쳐서까지 파고들며 지나치는 적을 전부 벤다.
// 같은 적은 한 번만 — 일렬로 선 무리를 관통하는 기술이다.
// 속도로 미는 게 아니라 **도착 지점을 먼저 정하고 보간**한다. 몇 틱이 밀려도 도착지가 안 흔들린다.
// 값을 상수 대신 객체로 묶은 건 시험 중 URL 로 바꿔보기 위해서다.
// 서버로 옮길 땐 상수로 굳힌다 — 실행 중에 바뀌면 클라·서버가 갈린다.
const SKILL = {
    ticks:       24,  // 모션 길이 (6프레임 × 4틱)
    blinkTicks:  10,  // 이 틱 안에 목표까지 도착한다 — 달리기가 아니라 파고들기다
    searchRange:720,  // 바라보는 쪽 이 거리 안에서 목표를 고른다 (화면 폭 960 의 3/4)
    searchDepth: 40,  // 깊이가 이만큼 어긋나도 목표로 삼는다
    passBy:      90,  // 목표를 이만큼 **지나쳐서** 선다. 앞에서 멈추면 이동이 짧고 답답하다
                      // (몸통 지름 34 보다 커야 끝난 뒤 겹쳐서 밀려나지 않는다)
    minDist:    340,  // 목표가 코앞이어도 최소 이만큼은 나아간다
    defaultDist:340,  // 목표가 아예 없을 때 가는 거리
    range:       50,  // 지나가며 베는 앞쪽 거리
    depth:       30,  // 깊이 허용 오차 — 일반 공격(26)보다 조금 넉넉하게
    dmg:          2,
    cooldown:     0,  // ⚠ 시험 중이라 0 이다. 원래 값은 240(4초) — 감을 잡으면 되돌린다
    knock:       20   // 지나간 쪽으로 밀어낸다
};

// ── 피격 ──
const HURT_TICKS = 12;
// 넉백은 사거리(64)보다 훨씬 짧아야 한다. 크게 두면 한 대 때릴 때마다
// 상대가 사거리 밖으로 밀려나 다음 타가 헛친다 — 몰아붙이는 맛이 사라진다.
const KNOCK_X    = 12;
const DEATH_TICKS = 20;  // 다 돌면 그 자세로 멈춘다

// ── 적 ──
const E_ATK_RANGE  = 52;
const E_ATK_DEPTH  = 22;
const E_THINK_GAP  = 70;   // 이 간격으로만 공격 시도 (계속 때리면 빠져나갈 틈이 없다)

// ── 상태 ──
const ST_IDLE = 0, ST_WALK = 1, ST_ATTACK = 2, ST_HURT = 3, ST_DEAD = 4, ST_SKILL = 5;

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
        hitIds: [],       // 대쉬 한 번에 이미 벤 상대 — 같은 적을 여러 틱에 걸쳐 다시 베지 않게
        cool: 0,          // 다음 공격까지 남은 틱
        skillCool: 0,     // 대쉬 쿨 — 일반 공격 쿨과 따로 센다
        lastSkillHit: 0,  // 지난 어설트에서 벤 마릿수
        dashFrom: 0,      // 파고들기 시작점
        dashTo: 0,        // 파고들어 도착할 자리 — 시작할 때 한 번만 정한다
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
    a.hitIds.length = 0;
}

// 움직일 수 있는 상태인가 (공격·피격·사망 중엔 조작이 안 먹는다)
function canAct(a) { return a.state === ST_IDLE || a.state === ST_WALK; }

// 몸끼리 겹치지 않게 밀어낸다. 벨트스크롤에서 이게 없으면 다 한 점에 모인다.
function separate(w) {
    for (let i = 0; i < w.actors.length; i++) {
        const a = w.actors[i];
        if (a.state === ST_DEAD || a.state === ST_SKILL) continue;
        for (let j = i + 1; j < w.actors.length; j++) {
            const b = w.actors[j];
            // ⚠ 파고드는 중(ST_SKILL)에는 밀어내기에서 뺀다.
            //    몸 한가운데를 통과하면 가로 겹침이 최대가 되어 '덜 파고든 축'이 깊이가 되고,
            //    그래서 뚫을수록 위아래로 튕긴다(실측 10.7px = 화면 43px). 통과하는 기술이니 겹쳐도 된다.
            if (b.state === ST_DEAD || b.state === ST_SKILL) continue;
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

// 파고들 목표를 고른다 — 바라보는 쪽에서 **가장 먼** 적이다.
// 가까운 놈을 고르면 코앞에서 멈춰 관통이 안 된다. 먼 놈을 고르면 그 사이를 전부 훑는다.
// ⚠ 거리가 같으면 id 가 작은 쪽. 이 규칙이 없으면 배열 순서에 따라 클라와 서버가
//    다른 목표를 골라 위치가 통째로 어긋난다 — 검증에서 제일 먼저 깨질 자리다.
function findAssaultTarget(w, caster) {
    let best = null, bestD = -1;
    for (const t of w.actors) {
        if (t === caster || t.state === ST_DEAD || t.kind === caster.kind) continue;
        const d = (t.x - caster.x) * caster.face;       // 바라보는 쪽으로 얼마나 떨어졌나
        if (d <= 0 || d > SKILL.searchRange) continue;
        if (Math.abs(t.y - caster.y) > SKILL.searchDepth) continue;
        if (d > bestD || (d === bestD && t.id < best.id)) { best = t; bestD = d; }
    }
    return best;
}

// 파고드는 동안 매 틱 부르는 판정. 지금 자리에서 앞쪽 좁은 사각 안의 적을 벤다.
// 한 번 벤 적은 hitIds 에 넣어 다음 틱에 또 베지 않게 한다 — 없으면 한 명에게 열 대가 들어간다.
// 일반 공격과 달리 경직 중인 상대도 벤다. 지나가며 훑는 기술이라 그래야 관통이 성립한다.
// fromX = 이번 틱에 출발한 자리. 지나온 구간 전체를 훑는다.
// ⚠ 지금 자리만 보면 안 된다 — 한 틱에 38px 씩 건너뛰므로 몸통(반지름 17)을 통째로
//    뛰어넘어 바로 앞의 적이 안 맞는다. 빠른 이동에서 흔한 '뚫고 지나가기'다.
function resolveDash(w, caster, fromX) {
    const tip = caster.x + caster.face * SKILL.range;   // 칼끝
    const x0 = Math.min(fromX, tip);
    const x1 = Math.max(fromX, tip);
    let hit = 0;
    for (const t of w.actors) {
        if (t === caster || t.state === ST_DEAD) continue;
        if (t.kind === caster.kind) continue;
        if (caster.hitIds.indexOf(t.id) !== -1) continue;   // 이미 벤 적
        if (t.x < x0 || t.x > x1) continue;
        if (Math.abs(t.y - caster.y) > SKILL.depth) continue;

        caster.hitIds.push(t.id);
        t.hp -= SKILL.dmg;
        if (t.hp <= 0) {
            t.hp = 0;
            setState(t, ST_DEAD);
        } else {
            setState(t, ST_HURT);
            t.x = clamp(t.x + caster.face * SKILL.knock, 20, WORLD_W - 20);
            t.face = -caster.face;
        }
        hit++;
    }
    return hit;
}

// input = { mx:-1|0|1, my:-1|0|1, attack:bool, skill:bool }
function step(w, input) {
    w.tick++;

    // ── 플레이어 ──
    const p = w.player;
    if (p.state !== ST_DEAD) {
        if (p.cool > 0) p.cool--;
        if (p.skillCool > 0) p.skillCool--;

        if (canAct(p) && input.skill && p.skillCool === 0) {
            setState(p, ST_SKILL);
            p.skillCool = SKILL.cooldown;
            p.cool = SKILL.ticks;      // 스킬이 도는 동안엔 일반 공격도 안 나가게
            p.lastSkillHit = 0;        // 이번에 몇 마리 벨지 새로 센다
            // 어디까지 파고들지를 시작할 때 한 번만 정한다.
            // 도중에 다시 고르면 목표가 죽거나 밀릴 때마다 경로가 흔들린다.
            const tg = findAssaultTarget(w, p);
            const want = tg ? (tg.x - p.x) * p.face + SKILL.passBy : SKILL.defaultDist;
            const dist = want > SKILL.minDist ? want : SKILL.minDist;
            p.dashFrom = p.x;
            p.dashTo = clamp(p.x + p.face * dist, 20, WORLD_W - 20);
        } else if (canAct(p) && input.attack && p.cool === 0) {
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
        } else if (a.state === ST_SKILL) {
            // 시작점에서 목표까지 blinkTicks 안에 고르게 옮겨간다.
            // 속도로 밀지 않고 도착 지점을 먼저 정하는 방식이라, 몇 틱이 밀려도 도착지가 안 흔들린다.
            if (a.stateTick <= SKILL.blinkTicks) {
                const fromX = a.x;                     // 출발 자리를 기억해 구간을 훑는다
                const r = a.stateTick / SKILL.blinkTicks;
                a.x = a.dashFrom + (a.dashTo - a.dashFrom) * r;
                a.lastSkillHit += resolveDash(w, a, fromX);  // 몇 마리 벴는지 — 서버가 검증할 값
            }
            if (a.stateTick >= SKILL.ticks) setState(a, ST_IDLE);
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
                       DEPTH_MIN, DEPTH_MAX, ST_IDLE, ST_WALK, ST_ATTACK, ST_HURT, ST_DEAD, ST_SKILL,
                       KIND_PLAYER, KIND_ORC, ATK_TICKS, HURT_TICKS, DEATH_TICKS, SKILL };
}
