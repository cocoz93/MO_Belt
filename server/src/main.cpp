// belt_server — 진입점.
// U1.2: 기동 하드닝(RLIMIT_NOFILE 상향 · 코어 예산 확인) + 계측 노출(/metrics) + 자기시험.
// U1.3: --listen <포트> 로 전송 계층(SO_REUSEPORT epoll 워커) 기동 — ECHO·PING 왕복.
#include <sys/resource.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "EpollWorker.h"
#include "Metrics.h"
#include "SelfTest.h"

namespace {

// WSL2 기본 soft 한도는 1024 — 세션 수천이면 accept 1천대에서 즉사한다. hard 까지 올린다.
bool RaiseFdLimit(rlim_t want)
{
    rlimit rl{};
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0)
        return false;
    if (rl.rlim_cur < rl.rlim_max)
    {
        rl.rlim_cur = rl.rlim_max;
        if (setrlimit(RLIMIT_NOFILE, &rl) != 0)
            return false;
    }
    std::printf("belt_server: RLIMIT_NOFILE soft=%llu hard=%llu\n",
                static_cast<unsigned long long>(rl.rlim_cur),
                static_cast<unsigned long long>(rl.rlim_max));
    return rl.rlim_cur >= want;
}

// 코어 예산(시작값): epoll 4 + 게임 7 + main·monitor 1 = 12, OS 여유 2 이상.
// 과거 20코어를 전부 소진해 측정이 무너진 사고의 재발 방지 — 초과면 기동 거부.
bool CheckCoreBudget(unsigned epollN, unsigned gameG, unsigned mainN, unsigned reserve)
{
    const unsigned hw   = std::thread::hardware_concurrency();
    const unsigned need = epollN + gameG + mainN;
    const bool     ok   = (hw == 0) || (need + reserve <= hw);
    std::printf("belt_server: core budget %u(epoll) + %u(game) + %u(main) + %u(reserve) <= %u : %s\n",
                epollN, gameG, mainN, reserve, hw, ok ? "ok" : "OVER");
    return ok;
}

} // namespace

int main(int argc, char** argv)
{
    uint16_t port       = 19100;
    uint16_t listenPort = 0;      // 0 = 전송 계층 끔
    int      runSecs    = -1;     // 음수 = 계속 떠 있음
    bool     selftest   = false;

    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--selftest") == 0)
            selftest = true;
        else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--listen") == 0 && i + 1 < argc)
            listenPort = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--run-secs") == 0 && i + 1 < argc)
            runSecs = std::atoi(argv[++i]);
    }

    if (selftest)
        return RunSelfTest();

    if (!RaiseFdLimit(8192))
    {
        std::fprintf(stderr, "belt_server: RLIMIT_NOFILE 상향 실패 — 기동 중단\n");
        return 1;
    }
    if (!CheckCoreBudget(4, 7, 1, 2))
    {
        std::fprintf(stderr, "belt_server: 코어 예산 초과 — 워커 수를 줄일 것\n");
        return 1;
    }

    metrics::g.up.Store(1);
    metrics::RegisterThisThread("main");

    metrics::Server ms;
    if (!ms.Start(port))
    {
        std::fprintf(stderr, "belt_server: /metrics %u 포트 바인드 실패\n", static_cast<unsigned>(port));
        return 1;
    }
    std::printf("belt_server: /metrics on 127.0.0.1:%u\n", static_cast<unsigned>(port));

    NetService net;
    if (listenPort != 0)
    {
        if (!net.Start(listenPort, /*workerCount=*/4, /*maxSessions=*/8192))
        {
            std::fprintf(stderr, "belt_server: 전송 계층 기동 실패 (port %u)\n",
                         static_cast<unsigned>(listenPort));
            return 1;
        }
        std::printf("belt_server: listen on 0.0.0.0:%u (epoll 워커 4)\n",
                    static_cast<unsigned>(listenPort));
    }

    if (runSecs >= 0)
        std::this_thread::sleep_for(std::chrono::seconds(runSecs));
    else
        for (;;)
            std::this_thread::sleep_for(std::chrono::seconds(1));

    net.Stop();
    ms.Stop();
    return 0;
}
