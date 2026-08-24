#pragma once
// 결정론 시뮬 — logic.js 의 C++ 거울 (U3.1: 이동+경계+separate 만).
//
// ⚠ 수식은 logic.js 의 표현식·곱셈 순서 그대로 옮긴다. 순서가 바뀌면 double 비트가
//   갈려 2차 lockstep 해시 대조가 통째로 재작업된다 (플랜 P3 조항).
// ⚠ 이 번역단위에 -ffast-math 류 금지 (CMakeLists 주석 참조).
#include "Room.h"

struct SimInput
{
    int mx = 0;      // -1|0|1
    int my = 0;
    // attack·skill 은 2차(전투 이식)에서 소비한다
};

// 한 틱: 플레이어 이동 → (오크 AI 는 2차) → separate + 무대 경계.
void SimStep(SimWorld& w, const SimInput inputs[kRoomMembers]);
