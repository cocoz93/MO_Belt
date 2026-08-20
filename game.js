'use strict';
// ─────────────────────────────────────────────────────────────
//  그리기와 입력 — 여기 있는 건 전부 화면 사정이다.
//  판정과 좌표는 logic.js 가 혼자 정한다. 이 파일은 그 결과를 보여주기만 한다.
//  (서버를 붙일 때 이 파일은 거의 그대로 남고, logic.js 만 서버로 간다)
// ─────────────────────────────────────────────────────────────

const CV = document.getElementById('cv');
const CX = CV.getContext('2d');
const VIEW_W = CV.width, VIEW_H = CV.height;

// ── 그림 ──
// 원본은 Tiny RPG Character Asset Pack 01 (Zerie) — 출처와 조건은 README 참고.
// 가로로 이어 붙인 시트다. 한 칸이 100×100.
const FRAME = 100;     // 시트 한 칸
const SCALE = 4;       // 화면에 그릴 배율 — 몸(21px)이 84px 이 된다

// 시트 안에서 발이 닿는 자리. 실측값이다(minY=39 maxY=59, 바닥여백 40).
// 이걸 안 맞추면 캐릭터가 바닥에 붕 뜨거나 파묻힌다.
const FOOT_Y = 59;
const ANCHOR = { 0: 49, 1: 54 };   // 몸 가로 중심 — 오크는 도끼 때문에 오른쪽으로 치우쳐 있다

// [파일, 프레임수, 프레임당 틱]
const SHEETS = {
  0: { // 플레이어
    dir: 'assets/soldier/',
    0: ['Soldier_Idle.png',     6, 8],
    1: ['Soldier_Walk.png',     8, 5],
    2: ['Soldier_Attack01.png', 6, 3],
    3: ['Soldier_Hurt.png',     4, 3],
    4: ['Soldier_Death.png',    4, 5],
    5: ['Soldier_Attack02.png', 6, 3]   // 어설트 — 앞 3칸(준비 동작)은 건너뛴다. frameIndex 참고
  },
  1: { // 오크
    dir: 'assets/orc/',
    0: ['Orc_Idle.png',     6, 8],
    1: ['Orc_Walk.png',     8, 5],
    2: ['Orc_Attack01.png', 6, 3],
    3: ['Orc_Hurt.png',     4, 3],
    4: ['Orc_Death.png',    4, 5]
  }
};

const IMG = {};
let loaded = 0, total = 0;
for (const kind of [0, 1]) {
  for (const st of [0, 1, 2, 3, 4, 5]) {
    if (!SHEETS[kind][st]) continue;      // 오크는 어설트(5)가 없다
    total++;
    const img = new Image();
    img.onload  = () => { loaded++; };
    img.onerror = () => { loaded++; console.error('그림을 못 읽었다:', SHEETS[kind][st][0]); };
    img.src = SHEETS[kind].dir + SHEETS[kind][st][0];
    IMG[kind + '_' + st] = img;
  }
}

// ── 입력 ──
const KEY = {};
addEventListener('keydown', e => {
  KEY[e.code] = true;
  if (['ArrowUp','ArrowDown','ArrowLeft','ArrowRight','Space'].includes(e.code)) e.preventDefault();
  if (e.code === 'KeyR') world = createWorld();
});
addEventListener('keyup', e => { KEY[e.code] = false; });

function readInput() {
  const l = KEY['ArrowLeft']  || KEY['KeyA'];
  const r = KEY['ArrowRight'] || KEY['KeyD'];
  const u = KEY['ArrowUp']    || KEY['KeyW'];
  const d = KEY['ArrowDown']  || KEY['KeyS'];
  return {
    mx: (r ? 1 : 0) - (l ? 1 : 0),
    my: (d ? 1 : 0) - (u ? 1 : 0),
    attack: !!(KEY['KeyJ'] || KEY['Space']),
    skill:  !!(KEY['KeyK'])
  };
}

// ── 무대 ──
// 바닥 띠는 화면 아래쪽에 둔다. y(깊이)가 그대로 화면 세로 오프셋이 된다.
const FLOOR_TOP = 300;   // 깊이 0 이 놓이는 화면 높이

function drawStage(camX) {
  // 하늘·벽
  const g = CX.createLinearGradient(0, 0, 0, FLOOR_TOP);
  g.addColorStop(0, '#161a24');
  g.addColorStop(1, '#232a38');
  CX.fillStyle = g;
  CX.fillRect(0, 0, VIEW_W, FLOOR_TOP);

  // 바닥
  const f = CX.createLinearGradient(0, FLOOR_TOP, 0, FLOOR_TOP + DEPTH_MAX + 40);
  f.addColorStop(0, '#3a3327');
  f.addColorStop(1, '#241f18');
  CX.fillStyle = f;
  CX.fillRect(0, FLOOR_TOP, VIEW_W, VIEW_H - FLOOR_TOP);

  // 바닥과 벽이 만나는 선
  CX.fillStyle = '#4a4335';
  CX.fillRect(0, FLOOR_TOP - 2, VIEW_W, 2);

  // 벽 무늬 — 카메라가 움직이는 게 보여야 해서 넣는다
  CX.fillStyle = 'rgba(255,255,255,.035)';
  const gap = 160;
  for (let x = -((camX * 0.5) % gap); x < VIEW_W; x += gap) {
    CX.fillRect(x, 120, 3, FLOOR_TOP - 122);
  }
  // 바닥 무늬
  CX.fillStyle = 'rgba(0,0,0,.16)';
  for (let x = -(camX % gap); x < VIEW_W; x += gap) {
    CX.fillRect(x, FLOOR_TOP, 2, VIEW_H - FLOOR_TOP);
  }
}

// 상태·경과틱 → 시트에서 꺼낼 칸 번호
// 어설트가 쓰는 Attack02 는 2~3칸에서 몸이 바닥선 위로 떠오른다(그림자는 바닥에 남는다).
// 살짝 도약하는 찌르기라 그런데, 파고드는 기술에는 안 맞아 준비 동작을 건너뛴다.
// ⚠ 픽셀 최하단으로는 못 잡는다 — 그림자가 y=59 까지 그려져 있어 전부 59 로 나온다.
const SKILL_SKIP = 3;

function frameIndex(a) {
  const [, n, per] = SHEETS[a.kind][a.state];
  let i = Math.floor(a.stateTick / per);
  if (a.state === ST_SKILL) i += SKILL_SKIP;
  if (a.state === ST_ATTACK || a.state === ST_SKILL || a.state === ST_HURT || a.state === ST_DEAD) {
    return Math.min(i, n - 1);          // 한 번만 재생하고 마지막 칸에서 멈춘다
  }
  return i % n;                          // idle·walk 는 계속 돈다
}

// ── 타격 연출 ──
// 전부 화면 사정이다. logic.js 는 이걸 모르고, 서버로도 안 간다.
// 한 장 그릴 때마다 실루엣을 새로 만들면 느리니 작은 캔버스 하나를 돌려 쓴다.
const FX = document.createElement('canvas');
FX.width = FX.height = FRAME;
const FXC = FX.getContext('2d');

// 스프라이트 한 칸을 단색으로 칠해서 그린다 — 피격 번쩍임과 잔상에 쓴다
function drawTinted(img, sx, color, alpha, dx, dy, dw) {
  FXC.clearRect(0, 0, FRAME, FRAME);
  FXC.globalCompositeOperation = 'source-over';
  FXC.drawImage(img, sx, 0, FRAME, FRAME, 0, 0, FRAME, FRAME);
  FXC.globalCompositeOperation = 'source-atop';   // 그려진 픽셀 위에만 칠한다
  FXC.fillStyle = color;
  FXC.fillRect(0, 0, FRAME, FRAME);
  CX.globalAlpha = alpha;
  CX.imageSmoothingEnabled = false;
  CX.drawImage(FX, 0, 0, FRAME, FRAME, dx, dy, dw, dw);
  CX.globalAlpha = 1;
}

const flash = new Map();   // 액터 id → 남은 번쩍임 틱
const prevHp = new Map();  // 액터 id → 지난 프레임 체력
let shake = 0;             // 화면 흔들림 세기
const ghosts = [];         // 어설트 잔상 {img, sx, x, y, face, life}

// 체력이 줄어든 놈을 찾아 번쩍이게 하고 화면을 흔든다.
// 로직이 알려주지 않으니 체력 변화를 보고 짐작한다 — 화면 사정이라 이걸로 충분하다.
function detectHits(w) {
  for (const a of w.actors) {
    const p = prevHp.get(a.id);
    if (p !== undefined && a.hp < p) {
      flash.set(a.id, 7);
      shake = Math.max(shake, a.kind === KIND_PLAYER ? 9 : 5);
    }
    prevHp.set(a.id, a.hp);
  }
  for (const [id, n] of flash) {
    if (n <= 1) flash.delete(id); else flash.set(id, n - 1);
  }
}

// 파고드는 중이면 지나온 자리에 잔상을 남긴다
function pushGhost(a) {
  const img = IMG[a.kind + '_' + a.state];
  if (!img || !img.complete) return;
  ghosts.push({ img, sx: frameIndex(a) * FRAME, x: a.x, y: a.y, face: a.face, life: 14 });
}

function drawGhosts(camX) {
  for (const g of ghosts) {
    const px = g.x - camX, py = FLOOR_TOP + g.y;
    const ax = ANCHOR[0] * SCALE, ay = FOOT_Y * SCALE, sw = FRAME * SCALE;
    CX.save();
    CX.translate(px, py);
    if (g.face < 0) CX.scale(-1, 1);
    drawTinted(g.img, g.sx, '#7fd4ff', (g.life / 14) * 0.45, -ax, -ay, sw);
    CX.restore();
    g.life--;
  }
  for (let i = ghosts.length - 1; i >= 0; i--) if (ghosts[i].life <= 0) ghosts.splice(i, 1);
}

function drawActor(a, camX) {
  const img = IMG[a.kind + '_' + a.state];
  if (!img || !img.complete || img.naturalWidth === 0) return;

  const fi = frameIndex(a);
  const sx = fi * FRAME;
  const sw = FRAME * SCALE;

  const px = a.x - camX;
  const py = FLOOR_TOP + a.y;

  const ax = ANCHOR[a.kind] * SCALE;
  const ay = FOOT_Y * SCALE;

  CX.save();
  CX.translate(px, py);
  if (a.face < 0) CX.scale(-1, 1);       // 왼쪽을 볼 땐 뒤집는다
  CX.imageSmoothingEnabled = false;      // 픽셀이 뭉개지면 안 된다
  CX.drawImage(img, sx, 0, FRAME, FRAME, -ax, -ay, sw, sw);
  // 맞은 직후 몇 틱은 흰색으로 번쩍인다 — 맞았다는 걸 알려주는 가장 싼 신호다
  const f = flash.get(a.id);
  if (f) drawTinted(img, sx, '#ffffff', Math.min(1, f / 5), -ax, -ay, sw);
  CX.restore();

  // 남은 체력 — 맞고 있다는 게 보여야 판정이 도는지 알 수 있다
  if (a.state !== ST_DEAD && a.hp < a.maxHp) {
    const w = 46, h = 5;
    CX.fillStyle = 'rgba(0,0,0,.6)';
    CX.fillRect(px - w / 2, py - 100, w, h);
    CX.fillStyle = a.kind === KIND_PLAYER ? '#6ee08a' : '#e06a6a';
    CX.fillRect(px - w / 2, py - 100, w * (a.hp / a.maxHp), h);
  }
}

// ── 시험용 값 덮어쓰기 ──
// ?bt=6&seek=500&dmg=3&cd=0&knock=50 처럼 URL 로 어설트 값을 바꿔본다.
// 시작할 때 한 번만 바꾼다 — 도는 중에 바뀌면 클라·서버가 갈린다.
// 값이 정해지면 이 블록은 지우고 logic.js 의 SKILL 을 상수로 굳힌다.
{
  const q = new URLSearchParams(location.search);
  if (q.has('bt'))    SKILL.blinkTicks  = +q.get('bt');
  if (q.has('seek'))  SKILL.searchRange = +q.get('seek');
  if (q.has('pass'))  SKILL.passBy      = +q.get('pass');
  if (q.has('min'))   SKILL.minDist     = +q.get('min');
  if (q.has('range')) SKILL.range       = +q.get('range');
  if (q.has('depth')) SKILL.depth       = +q.get('depth');
  if (q.has('dmg'))   SKILL.dmg         = +q.get('dmg');
  if (q.has('cd'))    SKILL.cooldown    = +q.get('cd');
  if (q.has('knock')) SKILL.knock       = +q.get('knock');
}

// ?show 를 붙이면 판정 범위를 겹쳐 그린다. 값을 눈으로 재보는 용도다.
const SHOW_RANGE = new URLSearchParams(location.search).has('show');

function drawRange(w, camX) {
  if (!SHOW_RANGE) return;
  const p = w.player;
  const px = p.x - camX, py = FLOOR_TOP + p.y;

  // 일반 공격 — 사각 판정(앞 ATK_RANGE, 뒤 ATK_BACK, 깊이 ±ATK_DEPTH)
  const x0 = p.face > 0 ? px - ATK_BACK  : px - ATK_RANGE;
  const x1 = p.face > 0 ? px + ATK_RANGE : px + ATK_BACK;
  CX.strokeStyle = 'rgba(110,224,138,.75)';
  CX.lineWidth = 2;
  CX.strokeRect(x0, py - ATK_DEPTH, x1 - x0, ATK_DEPTH * 2);

  // 목표를 찾는 범위
  CX.strokeStyle = 'rgba(255,212,121,.35)';
  CX.strokeRect(p.face > 0 ? px : px - SKILL.searchRange,
                py - SKILL.searchDepth, SKILL.searchRange, SKILL.searchDepth * 2);

  // 지금 고를 목표와 실제로 파고들 거리 — 로직과 같은 규칙으로 다시 센다
  let tg = null, bestD = -1, n = 0;
  for (const t of w.actors) {
    if (t.kind === KIND_PLAYER || t.state === ST_DEAD) continue;
    const d = (t.x - p.x) * p.face;
    if (d <= 0 || d > SKILL.searchRange) continue;
    if (Math.abs(t.y - p.y) > SKILL.searchDepth) continue;
    if (d > bestD || (d === bestD && t.id < tg.id)) { tg = t; bestD = d; }
  }
  const want = tg ? bestD + SKILL.passBy : SKILL.defaultDist;
  const reach = (want > SKILL.minDist ? want : SKILL.minDist) + SKILL.range;
  CX.strokeStyle = 'rgba(255,212,121,.9)';
  CX.strokeRect(p.face > 0 ? px : px - reach, py - SKILL.depth, reach, SKILL.depth * 2);

  for (const t of w.actors) {
    if (t.kind === KIND_PLAYER || t.state === ST_DEAD) continue;
    const d = (t.x - p.x) * p.face;
    if (d >= 0 && d <= reach && Math.abs(t.y - p.y) <= SKILL.depth) n++;
  }
  if (tg) {   // 고른 목표에 표시
    CX.strokeStyle = '#ff7b7b';
    CX.beginPath();
    CX.arc(tg.x - camX, FLOOR_TOP + tg.y, 20, 0, Math.PI * 2);
    CX.stroke();
  }
  CX.fillStyle = '#ffd479';
  CX.font = '600 14px "Malgun Gothic",sans-serif';
  CX.fillText('파고들 거리 ' + Math.round(reach) + (tg ? ' (목표 있음)' : ' (목표 없음 — 기본거리)')
              + '  →  경로 위 ' + n + '마리', 16, 114);
}

function drawHud(w) {
  CX.fillStyle = '#e8ecf4';
  CX.font = '600 15px "Malgun Gothic",sans-serif';
  CX.fillText('체력 ' + w.player.hp + ' / ' + w.player.maxHp, 16, 28);

  const alive = w.actors.filter(a => a.kind === KIND_ORC && a.state !== ST_DEAD).length;
  CX.fillText('남은 적 ' + alive, 16, 50);

  // 대쉬 쿨 — 몇 마리 벴는지도 같이 보여준다(나중에 서버가 검증할 값이다)
  const p = w.player;
  CX.font = '600 15px "Malgun Gothic",sans-serif';
  if (p.skillCool > 0) {
    CX.fillStyle = '#7b8496';
    CX.fillText('어설트  ' + (p.skillCool / 60).toFixed(1) + '초', 16, 72);
  } else {
    CX.fillStyle = '#ffd479';
    CX.fillText('어설트  K  준비됨', 16, 72);
  }
  if (p.lastSkillHit > 0) {
    CX.fillStyle = '#ffd479';
    CX.font = '13px "Malgun Gothic",sans-serif';
    CX.fillText('지난 어설트 ' + p.lastSkillHit + '마리 관통', 16, 92);
  }

  CX.fillStyle = '#93a0b6';
  CX.font = '13px "Malgun Gothic",sans-serif';
  CX.fillText('방향키 / WASD 이동   ·   J 공격   ·   K 어설트   ·   R 다시', 16, VIEW_H - 16);

  if (w.cleared) {
    CX.fillStyle = 'rgba(0,0,0,.55)';
    CX.fillRect(0, 200, VIEW_W, 90);
    CX.fillStyle = '#ffd479';
    CX.font = '700 30px "Malgun Gothic",sans-serif';
    CX.textAlign = 'center';
    CX.fillText('다 잡았다 — R 로 다시', VIEW_W / 2, 258);
    CX.textAlign = 'left';
  } else if (w.player.state === ST_DEAD) {
    CX.fillStyle = 'rgba(0,0,0,.55)';
    CX.fillRect(0, 200, VIEW_W, 90);
    CX.fillStyle = '#e08a9a';
    CX.font = '700 30px "Malgun Gothic",sans-serif';
    CX.textAlign = 'center';
    CX.fillText('쓰러졌다 — R 로 다시', VIEW_W / 2, 258);
    CX.textAlign = 'left';
  }
}

// ── 루프 ──
// 화면 주사율이 몇이든 로직은 항상 60Hz 로만 돈다.
// 이걸 안 지키면 나중에 서버 결과와 안 맞는다.
let world = createWorld();
let acc = 0, last = 0;

// ?sim=N 이면 시작 전에 N틱을 미리 굴려 그 장면부터 보여준다.
// 헤드리스로 화면을 찍을 때 쓴다 — 거기선 requestAnimationFrame 이 돌지 않아
// 가만히 두면 언제 찍어도 첫 프레임만 나온다.
//   ?sim=300&mx=1        오른쪽으로 5초 걸어간 장면
//   ?sim=300&mx=1&atk=1  걸어가며 계속 휘두른 장면
{
  const QS = new URLSearchParams(location.search);
  const n = +(QS.get('sim') || 0);
  if (n > 0) {
    const inp = { mx: +(QS.get('mx') || 0), my: +(QS.get('my') || 0), attack: QS.has('atk') };
    for (let i = 0; i < n; i++) step(world, inp);
  }
}

function loop(now) {
  requestAnimationFrame(loop);
  if (!last) { last = now; return; }

  let dt = (now - last) / 1000;
  last = now;
  if (dt > 0.25) dt = 0.25;              // 탭을 오래 놔뒀다 돌아온 경우
  acc += dt;

  const input = readInput();
  while (acc >= TICK_DT) {
    step(world, input);
    if (world.player.state === ST_SKILL) pushGhost(world.player);   // 어설트 자취
    detectHits(world);
    acc -= TICK_DT;
  }

  // 카메라는 주인공을 가운데 두되 무대 밖으로는 안 나간다
  let camX = world.player.x - VIEW_W / 2;
  camX = Math.max(0, Math.min(camX, WORLD_W - VIEW_W));

  // 흔들림은 무대와 사람에게만 준다. HUD 까지 흔들리면 읽기가 괴롭다
  CX.save();
  if (shake > 0) {
    // 세로는 많이 줄인다 — 벨트스크롤에선 위아래 움직임이 '깊이 이동'으로 읽혀 헷갈린다
    CX.translate((Math.random() - 0.5) * shake, (Math.random() - 0.5) * shake * 0.35);
    shake *= 0.82;
    if (shake < 0.4) shake = 0;
  }

  drawStage(camX);
  drawGhosts(camX);

  // 깊이가 얕은 쪽(안쪽)부터 그려야 앞사람이 뒷사람을 가린다
  const order = world.actors.slice().sort((a, b) => a.y - b.y);
  for (const a of order) drawActor(a, camX);

  drawRange(world, camX);
  CX.restore();

  drawHud(world);

  if (loaded < total) {
    CX.fillStyle = '#0b0d12';
    CX.fillRect(0, 0, VIEW_W, VIEW_H);
    CX.fillStyle = '#93a0b6';
    CX.font = '14px "Malgun Gothic",sans-serif';
    CX.fillText('그림 불러오는 중  ' + loaded + ' / ' + total, 16, 28);
  }
}
requestAnimationFrame(loop);
