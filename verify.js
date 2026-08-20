'use strict';
// ─────────────────────────────────────────────────────────────
//  설계 문서와 규칙 코드가 갈리지 않았는지 대조한다.
//    node verify.js
//
//  왜 스크립트인가 — 상수가 서른 개가 넘고, 값 하나가 어긋나도
//  클라와 서버의 판정이 갈린다. 눈으로 훑으면 반드시 놓친다.
//  나중에 C++ 이 생기면 여기에 세 번째 짝을 더한다(아래 TODO).
// ─────────────────────────────────────────────────────────────

const fs = require('fs');
const path = require('path');

const root = __dirname;
const src = fs.readFileSync(path.join(root, 'logic.js'), 'utf8');
const doc = fs.readFileSync(path.join(root, 'Docs', 'SERVER_DESIGN.md'), 'utf8');
const docLines = doc.split('\n');

// 상수 검사에서 뺄 것 — 다른 칸에서 따로 보거나, 값이 아니라 파생물이다
const SKIP = new Set([
  'TICK_DT',                                  // TICK_HZ 에서 계산된다
  'ST_IDLE', 'ST_WALK', 'ST_ATTACK',          // 상태는 아래 '상태' 칸에서 본다
  'ST_HURT', 'ST_DEAD', 'ST_SKILL',
  'KIND_PLAYER', 'KIND_ORC'                   // 종류도 마찬가지
]);

// ── logic.js 에서 뽑기 ──
const consts = {};
for (const m of src.matchAll(/^const ([A-Z][A-Z0-9_]*)\s*=\s*(-?[0-9.]+)\s*[;,]/gm)) {
  consts[m[1]] = m[2];
}
// 한 줄에 둘씩 선언한 것 (const A = 1, B = 2;)
for (const m of src.matchAll(/^const ([A-Z][A-Z0-9_]*)\s*=\s*(-?[0-9.]+),\s*([A-Z][A-Z0-9_]*)\s*=\s*(-?[0-9.]+)/gm)) {
  consts[m[1]] = m[2];
  consts[m[3]] = m[4];
}

const skillBlock = src.match(/const SKILL = \{([\s\S]*?)\n\};/);
const skill = {};
if (skillBlock) {
  for (const m of skillBlock[1].matchAll(/^\s*([a-zA-Z]+):\s*(-?[0-9.]+)/gm)) skill[m[1]] = m[2];
}

const fns = [...src.matchAll(/^function ([a-zA-Z]+)/gm)].map(m => m[1]);
const states = ((src.match(/const ST_[\s\S]*?;/) || [''])[0].match(/ST_[A-Z]+/g)) || [];

// ── 대조 ──
const bad = [];
let checked = 0;

// 이름이 적힌 줄을 찾아 그 줄에 값이 있는지 본다.
// 표의 다른 칸에 우연히 같은 숫자가 있을 수 있으니 값은 '토큰 단위'로 맞춘다.
function hasValue(line, v) {
  return new RegExp('(^|[^0-9.])' + v.replace('.', '\\.') + '([^0-9.]|$)').test(line);
}
function checkNamed(name, value, label) {
  checked++;
  const line = docLines.find(l => l.includes('`' + name + '`'));
  if (!line) { bad.push(`${label} 이 문서에 없다 — ${name} = ${value}`); return; }
  if (!hasValue(line, value)) {
    bad.push(`${label} 값이 다르다 — ${name}: 코드 ${value} / 문서줄 "${line.trim().slice(0, 70)}"`);
  }
}

for (const [k, v] of Object.entries(consts)) {
  if (SKIP.has(k)) continue;
  checkNamed(k, v, '상수');
}
for (const [k, v] of Object.entries(skill)) checkNamed(k, v, 'SKILL');

for (const f of fns) {
  checked++;
  if (!doc.includes('`' + f)) bad.push(`함수가 '옮길 함수' 목록에 없다 — ${f}()`);
}
for (const s of states) {
  checked++;
  const short = s.replace('ST_', '');
  if (!doc.includes('`' + short + '`')) bad.push(`상태가 문서에 없다 — ${s}`);
}

// ── 규칙 검사 — 값이 아니라 '지키기로 한 것' ──
// 한 번 데인 자리라 사라지면 바로 티가 난다. 없어졌으면 알려준다.
// ⚠ 주석을 걷어내고 본다 — logic.js 머리에 '금지 목록'으로 Math.random·Date 가 적혀 있어서
//    그대로 검사하면 "난수가 들어왔다"고 오탐한다.
const code = src.replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/[^\n]*/g, '');
const rules = [
  [/hitIds/,                        '어설트 중복 타격 방지(hitIds)가 사라졌다'],
  [/resolveDash\(w, caster, fromX/, '구간 판정(fromX)이 사라졌다 — 빠른 이동에서 적을 뛰어넘는다'],
  [/state === ST_SKILL\) continue/, '파고드는 중 밀어내기 제외가 사라졌다 — 깊이로 튕긴다'],
  [/t\.id < best\.id/,              '목표 선택 동점 규칙이 사라졌다 — 클라와 서버가 다른 적을 고른다'],
  [/Math\.random|Date\.now|new Date/, '난수·시계가 들어왔다 — 결정론이 깨진다']
];
for (const [re, msg] of rules) {
  checked++;
  const found = re.test(code);
  const shouldExist = !msg.includes('들어왔다');
  if (found !== shouldExist) bad.push('규칙: ' + msg);
}

// TODO: C++ 이식이 시작되면 server/ 의 상수 헤더를 세 번째 짝으로 넣는다.
//       (문서 ↔ logic.js ↔ C++ 셋이 같아야 해시 대조가 성립한다)

console.log(`검사 ${checked}건 · 불일치 ${bad.length}건`);
if (bad.length) {
  console.log();
  for (const b of bad) console.log('  ✗ ' + b);
  process.exit(1);
}
console.log('문서 = 코드 일치');
