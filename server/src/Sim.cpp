#include "Sim.h"

#include <algorithm>
#include <cmath>

#include "Constants.h"

namespace {

// logic.js 의 상태값과 짝 (2차에서 전체 6종으로 확장)
constexpr uint8_t ST_IDLE = 0;
constexpr uint8_t ST_WALK = 1;

double clampd(double v, double lo, double hi)
{
    return std::clamp(v, lo, hi);    // logic.js clamp — C++ 은 std::clamp (설계 §9)
}

} // namespace

void SimStep(SimWorld& w, const SimInput inputs[kRoomMembers])
{
    // ── 플레이어 이동 — logic.js:269-279 표현식 순서 그대로 ──
    for (int i = 0; i < kRoomMembers; ++i)
    {
        SimActor& p = w.actors[i];
        if (!p.present)
            continue;
        const int mx = inputs[i].mx, my = inputs[i].my;
        if (mx != 0 || my != 0)
        {
            // 대각선이라고 빨라지지 않게 — 두 축을 같이 누르면 각 축을 줄인다
            const double k = (mx != 0 && my != 0) ? 0.7071067811865476 : 1;
            p.x += mx * P_SPEED_X * TICK_DT * k;
            p.y += my * P_SPEED_Y * TICK_DT * k;
            if (mx != 0) p.face = mx > 0 ? 1 : -1;
            p.state = ST_WALK;
        }
        else
        {
            p.state = ST_IDLE;
        }
    }

    // ── 몸 밀어내기 + 무대 경계 — logic.js:136-163(separate) 그대로 ──
    //    U3.1 에는 DEAD/SKILL 상태가 없어 제외 조건은 present 로만 거른다(빈 슬롯).
    //    2차 전투 이식 때 ST_DEAD/ST_SKILL 제외가 원문 그대로 복원된다.
    for (int i = 0; i < kSnapActors; ++i)
    {
        SimActor& a = w.actors[i];
        if (!a.present)
            continue;
        for (int j = i + 1; j < kSnapActors; ++j)
        {
            SimActor& b = w.actors[j];
            if (!b.present)
                continue;
            const double dx = b.x - a.x, dy = b.y - a.y;
            const double ox = BODY_HALF_W * 2 - std::fabs(dx);
            const double oy = BODY_HALF_D * 2 - std::fabs(dy);
            if (ox <= 0 || oy <= 0)
                continue;
            // 덜 파고든 축으로만 밀어낸다 — 두 축 다 밀면 캐릭터가 튄다
            if (ox < oy)
            {
                const double push = (dx >= 0 ? 1 : -1) * ox * 0.5;
                a.x -= push; b.x += push;
            }
            else
            {
                const double push = (dy >= 0 ? 1 : -1) * oy * 0.5;
                a.y -= push; b.y += push;
            }
        }
    }
    // logic.js:160-163 은 전 액터를 조건 없이 clamp 한다 — 빈 슬롯까지 그대로 (관측 무영향)
    for (int i = 0; i < kSnapActors; ++i)
    {
        SimActor& a = w.actors[i];
        a.x = clampd(a.x, 20, WORLD_W - 20);
        a.y = clampd(a.y, DEPTH_MIN, DEPTH_MAX);
    }
}
