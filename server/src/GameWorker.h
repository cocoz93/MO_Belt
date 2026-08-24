#pragma once
// 게임 워커 — 60Hz 고정 틱 (설계 §4·§6).
//   페이싱: clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME) + next += period 누적.
//   밀리면 기상당 최대 3틱까지 따라잡고, 그 이상은 rebase + 건너뛴 틱 계측(§6).
//   워커 g 는 t0 + g·period/G 위상으로 스태거 — 동시 기상·dirty 폭주 방지.
//   방 목록·step() 은 U2.2~U3.1 에서 채운다. 지금은 페이싱과 계측만.
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

constexpr int64_t kTickHz       = 60;
constexpr int64_t kTickPeriodNs = 1000000000 / kTickHz;   // 16,666,666ns

class GameWorker
{
public:
    // t0Ns: 전 워커 공통 기준 시각(CLOCK_MONOTONIC ns). 위상은 안에서 계산.
    bool Start(uint8_t id, unsigned totalWorkers, int64_t t0Ns);
    void Stop();
    ~GameWorker() { Stop(); }

private:
    void Loop();
    void TickOnce();     // U2.2 부터 방 순회가 들어온다

    uint8_t           _id    = 0;
    unsigned          _total = 1;
    int64_t           _t0Ns  = 0;
    std::atomic<bool> _running{false};
    std::thread       _thread;
};

class GameService
{
public:
    bool Start(unsigned workerCount);
    void Stop();

private:
    std::vector<std::unique_ptr<GameWorker>> _workers;
};
