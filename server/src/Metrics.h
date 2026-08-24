#pragma once
// 계측 — MMO MonitorManager 의 구조(원자 카운터 · 캐시라인 분리 · relaxed 고정)만 가져오고
// 리눅스 전용으로 새로 쓴다. 노출은 의존 0 의 미니 HTTP(/metrics, 프로메테우스 텍스트).
//
// 스레드 CPU 는 /proc(USER_HZ=100, 10ms 양자화)이 아니라 pthread CPU 시계(ns)로 잰다 —
// rooms-per-core 지표가 게임 워커 스레드 CPU 기준이라 해상도가 판정을 좌우한다.
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace metrics {

// 원자 카운터. relaxed 고정 — 지표는 순서가 아니라 총량만 맞으면 된다.
// 복사 금지 + 평문 대입 미제공으로 Store() 를 강제한다 (MMO 규약 계승).
struct alignas(64) Counter
{
    std::atomic<int64_t> v{0};

    void    Inc() noexcept             { v.fetch_add(1, std::memory_order_relaxed); }
    void    Add(int64_t d) noexcept    { v.fetch_add(d, std::memory_order_relaxed); }
    void    Store(int64_t d) noexcept  { v.store(d, std::memory_order_relaxed); }
    int64_t Load() const noexcept      { return v.load(std::memory_order_relaxed); }

    // 최대값 갱신 — 실패해도 다른 스레드가 더 큰 값을 넣은 것이니 그대로 둔다
    void StoreMax(int64_t d) noexcept
    {
        int64_t cur = v.load(std::memory_order_relaxed);
        while (d > cur && !v.compare_exchange_weak(cur, d, std::memory_order_relaxed)) {}
    }

    Counter() = default;
    Counter(const Counter&) = delete;
    Counter& operator=(const Counter&) = delete;
};

// 호출한 스레드 자신의 CPU 시간(ns). 실패하면 -1.
int64_t ThreadCpuNs();

// 이 스레드를 /metrics 의 belt_thread_cpu_ns{thread="이름"} 줄로 노출한다.
// 스레드가 살아 있는 동안만 유효 — 지금은 장수 스레드(워커)만 등록한다.
void RegisterThisThread(const char* name);

// 게임 워커 oversleep 히스토그램 버킷 상한(µs). 마지막은 +inf.
constexpr int64_t kOversleepBucketUs[] = { 50, 100, 200, 500, 1000, 2000, 5000, 10000 };
constexpr int     kOversleepBuckets    = 9;    // 위 8개 + inf
constexpr int     kMaxGameWorkers      = 16;

// 게임 워커 하나의 페이싱 지표 — 워커당 한 벌이라 스레드 간 경합 없음(관측만 교차).
struct GameWorkerStats
{
    Counter ticks;              // 실행한 step 수
    Counter wakes;              // 기상 수 (히스토그램 표본 수)
    Counter skippedTicks;       // rebase 로 건너뛴 틱 수 — 방 한계 지표(설계 §6)
    Counter rebases;
    Counter oversleepMaxNs;
    Counter oversleepBucket[kOversleepBuckets];   // 비누적 저장 — 노출 시 누적 변환
};

// 서버가 노출하는 카운터 전부. 필드를 늘리면 BuildText() 도 같이 고친다.
struct Counters
{
    Counter up;            // 1 고정 — 스크레이프 생존 확인용
    Counter accepts;
    Counter disconnects;
    Counter recvBytes;
    Counter sendBytes;

    Counter gameWorkerCount;                      // 노출 범위 결정용
    GameWorkerStats game[kMaxGameWorkers];
};
extern Counters g;

// 프로메테우스 텍스트 조립 (카운터 + 등록된 스레드 CPU).
std::string BuildText();

// /metrics 미니 HTTP 서버 — GET 이 뭐든 전체 덤프 하나만 준다. 요청 파싱 없음.
class Server
{
public:
    bool Start(uint16_t port);
    void Stop();    // shutdown() 으로 accept 를 깨운다 — close 만으로는 못 깨운다 (MMO F5 교훈)
    ~Server() { Stop(); }

private:
    void Loop();

    int               _listenFd = -1;
    std::atomic<bool> _running{false};
    std::thread       _thread;
};

} // namespace metrics
