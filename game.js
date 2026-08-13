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
    2: ['Soldier_Attack01.png', 6, 5],
    3: ['Soldier_Hurt.png',     4, 5],
    4: ['Soldier_Death.png',    4, 6]
  },
  1: { // 오크
    dir: 'assets/orc/',
    0: ['Orc_Idle.png',     6, 8],
    1: ['Orc_Walk.png',     8, 5],
    2: ['Orc_Attack01.png', 6, 5],
    3: ['Orc_Hurt.png',     4, 5],
    4: ['Orc_Death.png',    4, 6]
  }
};

const IMG = {};
let loaded = 0, total = 0;
for (const kind of [0, 1]) {
  for (const st of [0, 1, 2, 3, 4]) {
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
    attack: !!(KEY['KeyJ'] || KEY['Space'])
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
function frameIndex(a) {
  const [, n, per] = SHEETS[a.kind][a.state];
  const i = Math.floor(a.stateTick / per);
  if (a.state === ST_ATTACK || a.state === ST_HURT || a.state === ST_DEAD) {
    return Math.min(i, n - 1);          // 한 번만 재생하고 마지막 칸에서 멈춘다
  }
  return i % n;                          // idle·walk 는 계속 돈다
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

function drawHud(w) {
  CX.fillStyle = '#e8ecf4';
  CX.font = '600 15px "Malgun Gothic",sans-serif';
  CX.fillText('체력 ' + w.player.hp + ' / ' + w.player.maxHp, 16, 28);

  const alive = w.actors.filter(a => a.kind === KIND_ORC && a.state !== ST_DEAD).length;
  CX.fillText('남은 적 ' + alive, 16, 50);

  CX.fillStyle = '#93a0b6';
  CX.font = '13px "Malgun Gothic",sans-serif';
  CX.fillText('방향키 / WASD 이동   ·   J 또는 스페이스 공격   ·   R 다시', 16, VIEW_H - 16);

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
    acc -= TICK_DT;
  }

  // 카메라는 주인공을 가운데 두되 무대 밖으로는 안 나간다
  let camX = world.player.x - VIEW_W / 2;
  camX = Math.max(0, Math.min(camX, WORLD_W - VIEW_W));

  drawStage(camX);

  // 깊이가 얕은 쪽(안쪽)부터 그려야 앞사람이 뒷사람을 가린다
  const order = world.actors.slice().sort((a, b) => a.y - b.y);
  for (const a of order) drawActor(a, camX);

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
