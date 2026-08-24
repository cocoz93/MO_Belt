#include "GameWorker.h"

#include <ctime>

#include <cerrno>
#include <cstdio>

#include "Metrics.h"

namespace {

int64_t NowNs()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

// 절대 시각까지 잔다. EINTR 이면 같은 절대치로 재호출 — 상대 수면 반복은 그만큼 밀린다.
void SleepUntilNs(int64_t whenNs)
{
    timespec ts{};
    ts.tv_sec  = whenNs / 1000000000;
    ts.tv_nsec = whenNs % 1000000000;
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {}
}

void RecordOversleep(metrics::GameWorkerStats& gw, int64_t oversleepNs)
{
    gw.wakes.Inc();
    gw.oversleepMaxNs.StoreMax(oversleepNs);
    const int64_t us = oversleepNs / 1000;
    int b = 0;
    while (b < metrics::kOversleepBuckets - 1 && us > metrics::kOversleepBucketUs[b])
        ++b;
    gw.oversleepBucket[b].Inc();
}

constexpr int kMaxCatchupSteps = 3;    // 설계 §6 — 그 이상 밀리면 rebase

} // namespace

bool GameWorker::Start(uint8_t id, unsigned totalWorkers, int64_t t0Ns)
{
    _id    = id;
    _total = totalWorkers == 0 ? 1 : totalWorkers;
    _t0Ns  = t0Ns;
    _running.store(true);
    _thread = std::thread([this] { Loop(); });
    return true;
}

void GameWorker::Stop()
{
    if (!_running.exchange(false))
        return;
    if (_thread.joinable())
        _thread.join();      // 틱 대기 ≤ 16.7ms 라 곧 내려온다
}

void GameWorker::Loop()
{
    char name[16];
    std::snprintf(name, sizeof(name), "game%u", static_cast<unsigned>(_id));
    metrics::RegisterThisThread(name);

    metrics::GameWorkerStats& gw = metrics::g.game[_id];

    // 위상 스태거: 워커 g 는 t0 + g·period/G 에서 첫 틱 — 동시 기상을 피한다
    int64_t next = _t0Ns + static_cast<int64_t>(_id) * (kTickPeriodNs / _total);

    while (_running.load(std::memory_order_relaxed))
    {
        SleepUntilNs(next);
        const int64_t now = NowNs();
        RecordOversleep(gw, now > next ? now - next : 0);

        // 최소 1틱 + 밀린 만큼 최대 3틱까지 따라잡는다
        int steps = 0;
        do
        {
            TickOnce();
            gw.ticks.Inc();
            next += kTickPeriodNs;
            ++steps;
        } while (now >= next && steps < kMaxCatchupSteps);

        if (now >= next)
        {
            // 여전히 밀렸다 — 몰아서 돌리면 눈덩이(설계 §6). 기준을 지금으로 다시 놓고
            // 건너뛴 틱 수를 방 한계 지표로 센다.
            const int64_t behind = (now - next) / kTickPeriodNs + 1;
            gw.skippedTicks.Add(behind);
            gw.rebases.Inc();
            next = now + kTickPeriodNs;
        }
    }
}

void GameWorker::TickOnce()
{
    // U2.2: 넣을큐/뺄큐 반영 → 방 순회 step() → 스냅샷 직렬화가 여기 들어온다.
    // 지금은 페이싱 계측만 검증한다.
}

// ──────────────────────────────────────────────────────────────────

bool GameService::Start(unsigned workerCount)
{
    if (workerCount > static_cast<unsigned>(metrics::kMaxGameWorkers))
        return false;

    metrics::g.gameWorkerCount.Store(workerCount);

    const int64_t t0 = NowNs() + 100 * 1000000;    // 100ms 뒤 공통 기준 시각
    for (unsigned i = 0; i < workerCount; ++i)
    {
        auto w = std::make_unique<GameWorker>();
        if (!w->Start(static_cast<uint8_t>(i), workerCount, t0))
            return false;
        _workers.push_back(std::move(w));
    }
    return true;
}

void GameService::Stop()
{
    for (auto& w : _workers)
        w->Stop();
    _workers.clear();
}
