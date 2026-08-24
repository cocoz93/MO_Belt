#include "EpollWorker.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "Metrics.h"
#include "Protocol.h"

namespace {

// epoll_data 태그: 상위 32비트 = 세대, 하위 32비트 = 슬롯 번호.
// 슬롯이 회수·재사용돼도 같은 wait 배치에 남은 옛 이벤트를 세대 불일치로 거른다.
uint64_t PackTag(uint32_t idx, uint32_t gen)
{
    return (static_cast<uint64_t>(gen) << 32) | idx;
}
constexpr uint32_t kListenTag = 0xFFFFFFFFu;

constexpr int kEpollTimeoutMs   = 1;     // dirty 스캔 주기(U2.3)와 종료 감지를 겸한다
constexpr int kMaxEventsPerWait = 128;
constexpr int kMaxAcceptPerWake = 64;    // accept 폭주가 1ms 박자를 깨지 않게 상한

} // namespace

// ──────────────────────────────────────────────────────────────────
// EpollWorker
// ──────────────────────────────────────────────────────────────────

bool EpollWorker::Start(uint8_t id, uint16_t port, SessionPool* pool)
{
    _id   = id;
    _pool = pool;

    _epfd = ::epoll_create1(0);
    if (_epfd < 0)
        return false;

    _listenFd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (_listenFd < 0)
        return false;

    int on = 1;
    ::setsockopt(_listenFd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    // 워커마다 자기 listen 소켓 — 커널이 4-tuple 해시로 새 접속을 배분한다
    ::setsockopt(_listenFd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);
    if (::bind(_listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(_listenFd, 512) != 0)
    {
        return false;
    }

    epoll_event ev{};
    ev.events   = EPOLLIN;
    ev.data.u64 = PackTag(kListenTag, _id);
    if (::epoll_ctl(_epfd, EPOLL_CTL_ADD, _listenFd, &ev) != 0)
        return false;

    _running.store(true);
    _thread = std::thread([this] { Loop(); });
    return true;
}

void EpollWorker::Stop()
{
    if (!_running.exchange(false))
        return;
    if (_thread.joinable())
        _thread.join();          // 1ms 타임아웃 루프라 곧 깨어나 정리 후 내려온다
}

void EpollWorker::Loop()
{
    char name[16];
    std::snprintf(name, sizeof(name), "epoll%u", static_cast<unsigned>(_id));
    metrics::RegisterThisThread(name);

    epoll_event events[kMaxEventsPerWait];
    while (_running.load(std::memory_order_relaxed))
    {
        const int n = ::epoll_wait(_epfd, events, kMaxEventsPerWait, kEpollTimeoutMs);
        for (int i = 0; i < n; ++i)
        {
            const uint64_t tag = events[i].data.u64;
            const uint32_t idx = static_cast<uint32_t>(tag & 0xFFFFFFFFu);
            const uint32_t gen = static_cast<uint32_t>(tag >> 32);
            if (idx == kListenTag)
                HandleAccept();
            else
                HandleSession(idx, gen, events[i].events);
        }
        // (U2.3) 여기서 dirty 비트맵을 스캔해 송신링을 걷는다
    }
    CleanupInThread();
}

void EpollWorker::CleanupInThread()
{
    // 소유 스레드에서만 정리한다 — 단일 소유 규약 유지
    std::vector<uint32_t> mine = _mySessions;
    for (uint32_t idx : mine)
        Disconnect(_pool->At(idx));

    if (_listenFd >= 0)
    {
        ::close(_listenFd);
        _listenFd = -1;
    }
    if (_epfd >= 0)
    {
        ::close(_epfd);
        _epfd = -1;
    }
}

void EpollWorker::HandleAccept()
{
    for (int i = 0; i < kMaxAcceptPerWake; ++i)
    {
        const int fd = ::accept4(_listenFd, nullptr, nullptr, SOCK_NONBLOCK);
        if (fd < 0)
        {
            if (errno == EINTR)
                continue;
            break;      // EAGAIN 포함 — 이번 라운드는 여기까지
        }

        Session* s = _pool->Alloc();
        if (s == nullptr)
        {
            ::close(fd);       // 풀 고갈 — 받자마자 끊는다
            continue;
        }

        int on = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));   // 저지연 우선 — 설계 §12

        s->fd    = fd;
        s->owner = _id;

        epoll_event ev{};
        ev.events   = EPOLLIN | EPOLLRDHUP;
        ev.data.u64 = PackTag(s->idx, s->gen);
        if (::epoll_ctl(_epfd, EPOLL_CTL_ADD, fd, &ev) != 0)
        {
            ::close(fd);
            s->fd = -1;
            s->dead.store(true, std::memory_order_relaxed);
            _pool->Release(*s);
            continue;
        }

        _mySessions.push_back(s->idx);
        metrics::g.accepts.Inc();
    }
}

void EpollWorker::HandleSession(uint32_t idx, uint32_t gen, uint32_t events)
{
    if (idx >= _pool->Capacity())
        return;
    Session& s = _pool->At(idx);
    if (s.gen != gen || s.dead.load(std::memory_order_relaxed))
        return;      // 회수됐거나 같은 배치에서 이미 절단된 세션의 잔여 이벤트

    if (events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP))
    {
        Disconnect(s);
        return;
    }
    if (events & EPOLLIN)
    {
        ReadSession(s);
        if (s.dead.load(std::memory_order_relaxed))
            return;
    }
    if (events & EPOLLOUT)
        FlushSend(s);
}

void EpollWorker::ReadSession(Session& s)
{
    for (;;)
    {
        const size_t direct = s.recvQ.GetDirectWriteSize();
        if (direct == 0)
        {
            // 파싱이 못 따라가는 만큼 쌓였다 — 프레임 상한(1KB) 대비 링이 8KB라
            // 정상 클라에서는 나올 수 없는 상태. 프로토콜 위반으로 절단.
            Disconnect(s);
            return;
        }

        const ssize_t n = ::read(s.fd, s.recvQ.GetWritePtr(), direct);
        if (n > 0)
        {
            s.recvQ.MoveWritePtr(static_cast<size_t>(n));
            metrics::g.recvBytes.Add(n);
            ParseFrames(s);
            if (s.dead.load(std::memory_order_relaxed))
                return;
            continue;      // EAGAIN 이 나올 때까지 마저 읽는다
        }
        if (n == 0)
        {
            Disconnect(s);     // 상대가 곱게 닫음
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        if (errno == EINTR)
            continue;
        Disconnect(s);
        return;
    }
}

void EpollWorker::ParseFrames(Session& s)
{
    for (;;)
    {
        if (s.recvQ.GetDataSize() < sizeof(MsgHeader))
            return;

        MsgHeader h{};
        s.recvQ.Peek(&h, sizeof(h));

        // 파서 하드닝 — 범위 밖이면 즉시 절단 (MMO 상한 침범 교훈 승계)
        if (h.size < sizeof(MsgHeader) || h.size > MAX_PACKET_SIZE)
        {
            Disconnect(s);
            return;
        }
        if (s.recvQ.GetDataSize() < h.size)
            return;      // 아직 덜 왔다

        uint8_t buf[MAX_PACKET_SIZE];
        s.recvQ.Dequeue(buf, h.size);
        Dispatch(s, buf, h.size);
        if (s.dead.load(std::memory_order_relaxed))
            return;
    }
}

void EpollWorker::Dispatch(Session& s, const uint8_t* msg, size_t len)
{
    const MsgHeader* h = reinterpret_cast<const MsgHeader*>(msg);
    switch (static_cast<MsgType>(h->type))
    {
    case MsgType::ECHO:
        SendBytes(s, msg, len);
        return;

    case MsgType::C2S_PING:
    {
        if (len != sizeof(MSG_C2S_PING))
        {
            Disconnect(s);
            return;
        }
        const MSG_C2S_PING* ping = reinterpret_cast<const MSG_C2S_PING*>(msg);
        MSG_S2C_PONG pong{};
        pong.header.size   = sizeof(MSG_S2C_PONG);
        pong.header.type   = static_cast<uint16_t>(MsgType::S2C_PONG);
        pong.clientTimeUs  = ping->clientTimeUs;
        pong.serverTick    = 0;      // 게임 워커(U2.1)가 생기면 채운다
        pong.tickRemainUs  = 0;
        SendBytes(s, &pong, sizeof(pong));
        return;
    }

    default:
        Disconnect(s);     // 모르는 타입 — 하드닝: 통째 절단
        return;
    }
}

void EpollWorker::SendBytes(Session& s, const void* data, size_t len)
{
    if (s.sendQ.Enqueue(data, len) == 0)
    {
        // U1.3 시점의 송신링 가득 = 상대가 안 읽는 것 — 절단.
        // (스냅샷의 drop-oldest 정책은 U2.3에서 스냅샷 경로에만 붙는다)
        Disconnect(s);
        return;
    }
    FlushSend(s);
}

void EpollWorker::FlushSend(Session& s)
{
    for (;;)
    {
        const auto si = s.sendQ.GetSendInfo();
        if (si.dataSize == 0)
        {
            if (s.wantWrite)
                UpdateWriteInterest(s, false);
            return;
        }

        const ssize_t n = ::write(s.fd, si.readPtr, si.directReadSize);
        if (n > 0)
        {
            s.sendQ.Consume(static_cast<size_t>(n));
            metrics::g.sendBytes.Add(n);
            if (static_cast<size_t>(n) < si.directReadSize)
            {
                UpdateWriteInterest(s, true);      // 커널 버퍼가 찼다 — 나머지는 EPOLLOUT 뒤에
                return;
            }
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            UpdateWriteInterest(s, true);
            return;
        }
        if (errno == EINTR)
            continue;
        Disconnect(s);
        return;
    }
}

void EpollWorker::UpdateWriteInterest(Session& s, bool want)
{
    if (s.wantWrite == want)
        return;      // 같은 상태 재호출 생략 — MMO _epollWantWrite 표식 승계

    epoll_event ev{};
    ev.events   = EPOLLIN | EPOLLRDHUP | (want ? EPOLLOUT : 0u);
    ev.data.u64 = PackTag(s.idx, s.gen);
    if (::epoll_ctl(_epfd, EPOLL_CTL_MOD, s.fd, &ev) == 0)
        s.wantWrite = want;
}

void EpollWorker::Disconnect(Session& s)
{
    // 해제 순서(플랜 「수명·해제 규약」): ① dead ② DEL+close ③ (방 뺄큐 — U2.2)
    // ④ 소유 목록·dirty 제거 ⑤ netRef 반환
    if (s.dead.exchange(true))
        return;

    if (s.fd >= 0)
    {
        ::epoll_ctl(_epfd, EPOLL_CTL_DEL, s.fd, nullptr);
        ::close(s.fd);
        s.fd = -1;
    }

    for (size_t i = 0; i < _mySessions.size(); ++i)
    {
        if (_mySessions[i] == s.idx)
        {
            _mySessions[i] = _mySessions.back();
            _mySessions.pop_back();
            break;
        }
    }

    metrics::g.disconnects.Inc();
    _pool->Release(s);      // netRef 반환 — gameRef(U2.2)가 없으면 여기서 슬롯 회수
}

// ──────────────────────────────────────────────────────────────────
// NetService
// ──────────────────────────────────────────────────────────────────

bool NetService::Start(uint16_t port, unsigned workerCount, uint32_t maxSessions)
{
    if (!_pool.Init(maxSessions))
        return false;

    for (unsigned i = 0; i < workerCount; ++i)
    {
        auto w = std::make_unique<EpollWorker>();
        if (!w->Start(static_cast<uint8_t>(i), port, &_pool))
            return false;
        _workers.push_back(std::move(w));
    }
    return true;
}

void NetService::Stop()
{
    for (auto& w : _workers)
        w->Stop();
    _workers.clear();
}
