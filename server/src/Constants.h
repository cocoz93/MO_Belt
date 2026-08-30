#pragma once
// 게임 규칙 상수 — 값의 정본은 logic.js (설계 §9 "값을 다시 정하지 않는다").
//
// 형식 계약: `inline constexpr double NAME = 값;` 한 줄 — 2차에서 verify.js 의
// 세 번째 짝(문서 ↔ logic.js ↔ C++)이 이 형식을 정규식으로 뽑아 대조한다.
// U3.1 은 이동 부분집합만 싣는다. 전투 상수는 2차 이식 때 같은 형식으로 늘린다.

inline constexpr double TICK_HZ = 60;
inline constexpr double TICK_DT = 1.0 / 60.0;   // logic.js 와 같은 파생식 — 같은 double 비트

inline constexpr double WORLD_W = 1600;
inline constexpr double DEPTH_MIN = 0;
inline constexpr double DEPTH_MAX = 165;

inline constexpr double P_SPEED_X = 215;
inline constexpr double P_SPEED_Y = 132;
inline constexpr double E_SPEED_X = 98;
inline constexpr double E_SPEED_Y = 62;

inline constexpr double BODY_HALF_W = 17;
inline constexpr double BODY_HALF_D = 9;

// 무대 좌우 여백 — logic.js 는 리터럴 20 (4곳 중복). 2차 매직넘버 승격 대상이라 이름을 미리 둔다.
inline constexpr double STAGE_MARGIN = 20;

// 1차 방 이동 조건(설계 §12): 생존 플레이어 전원이 이 선을 넘으면 다음 방.
// ⚠ 문서는 WORLD_W−80 이었으나 실측에서 상충 발견 — 오크가 오른쪽 벽에 밀려 쌓이면
//   (같은 깊이 레인에 3겹 = 34px×3) 플레이어 전선이 ~1444 에서 잼이 걸려 1520 에 못 닿는다.
//   벽 잼 최악치보다 안쪽인 −200 으로 구현 (문서 §12 반영 완료).
inline constexpr double EXIT_ZONE_X = WORLD_W - 200;
