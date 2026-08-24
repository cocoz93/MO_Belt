#pragma once
// 소켓 단일 소유 epoll 워커 — 설계 4절.
//   각 워커가 자기 listen 소켓(SO_REUSEPORT)과 자기 epoll 을 갖고,
//   소켓 하나의 accept·recv·send·epoll_ctl·close 는 전부 소유 워커 스레드에서만 일어난다.
//   이 규약이 MMO epoll 포팅의 수신 직렬화 게이트(F1)류를 애초에 없앤다.
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "Session.h"

class EpollWorker
{
public:
    bool Start(uint8_t id, uint16_t port, SessionPool* pool);
    void Stop();     // 플래그만 내린다 — 정리(세션 전부 절단·fd 닫기)는 소유 스레드가 루프 끝에서 한다
    ~EpollWorker() { Stop(); }

private:
    void Loop();
    void CleanupInThread();
    void HandleAccept();
    void HandleSession(uint32_t idx, uint32_t gen, uint32_t events);
    void ReadSession(Session& s);
    void ParseFrames(Session& s);
    void Dispatch(Session& s, const uint8_t* msg, size_t len);
    void SendBytes(Session& s, const void* data, size_t len);
    void FlushSend(Session& s);
    void UpdateWriteInterest(Session& s, bool want);
    void Disconnect(Session& s);

    uint8_t               _id       = 0;
    int                   _epfd     = -1;
    int                   _listenFd = -1;
    std::atomic<bool>     _running{false};
    std::thread           _thread;
    SessionPool*          _pool = nullptr;
    std::vector<uint32_t> _mySessions;   // 소유 세션 idx 목록 — 소유 스레드 전용
};

// 워커 N개 + 세션 풀 묶음.
class NetService
{
public:
    bool Start(uint16_t port, unsigned workerCount, uint32_t maxSessions);
    void Stop();

private:
    SessionPool                               _pool;
    std::vector<std::unique_ptr<EpollWorker>> _workers;
};
