// belt_dummy — 부하 생성기 (수치의 정본, 설계 §12).
//
//   MMO 부하 클라의 개념만 계승해 리눅스 네이티브(WSL 내부)로 새로 짰다:
//   · 스레드당 epoll 단일 루프 · rampUp 점진 접속 · 60Hz 유지 입력 · 스냅샷 소비
//   · 로컬 누적 → 주기 일괄 반영(계측이 측정을 오염시키지 않게)
//   · "부하 생성기 자신이 포화했나" 게이트: 루프 작업시간 히스토그램 + 송신 보류 넘침 카운터
//     — 이 게이트가 깨진 런은 서버 수치도 무효다.
//
//   기본 입력은 mx=0(정지) — 이동하면 출구 방 이동이 발동해 방 수 판독이 흔들린다.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "Metrics.h"
#include "Protocol.h"

using metrics::Counter;

// ── 전역 지표 (relaxed — 워커들이 일괄 반영) ──
namespace {

constexpr int64_t kLoopBucketUs[] = { 50, 100, 200, 500, 1000, 2000, 5000, 10000, 40000 };
constexpr int     kLoopBuckets    = 10;   // 위 9개 + inf. 게이트: p99 < 40ms (MMO 관례)

struct DummyStats
{
    Counter active;          // ACTIVE 상태 세션 수
    Counter joinOk;
    Counter joinFail;
    Counter connectFail;
    Counter snapshots;
    Counter inputsSent;
    Counter pendOverflow;    // 송신 보류 버퍼 넘침 — 0 이 아니면 그 런은 무효 (게이트)
    Counter serverClosed;    // 서버가 끊음
    Counter loops;
    Counter loopBucket[kLoopBuckets];
    Counter loopMaxUs;
};
DummyStats g_stats;

int64_t NowNs()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

std::string BuildDummyText()
{
    char line[128];
    std::string out;
    out.reserve(1024);
    auto put = [&](const char* name, int64_t v) {
        std::snprintf(line, sizeof(line), "%s %lld\n", name, static_cast<long long>(v));
        out += line;
    };
    put("dummy_active",            g_stats.active.Load());
    put("dummy_join_ok_total",     g_stats.joinOk.Load());
    put("dummy_join_fail_total",   g_stats.joinFail.Load());
    put("dummy_connect_fail_total", g_stats.connectFail.Load());
    put("dummy_snapshots_total",   g_stats.snapshots.Load());
    put("dummy_inputs_sent_total", g_stats.inputsSent.Load());
    put("dummy_pend_overflow_total", g_stats.pendOverflow.Load());
    put("dummy_server_closed_total", g_stats.serverClosed.Load());
    put("dummy_loops_total",       g_stats.loops.Load());
    put("dummy_loop_max_us",       g_stats.loopMaxUs.Load());
    int64_t cum = 0;
    for (int b = 0; b < kLoopBuckets; ++b)
    {
        cum += g_stats.loopBucket[b].Load();
        if (b < kLoopBuckets - 1)
            std::snprintf(line, sizeof(line), "dummy_loop_us_bucket{le=\"%lld\"} %lld\n",
                          static_cast<long long>(kLoopBucketUs[b]), static_cast<long long>(cum));
        else
            std::snprintf(line, sizeof(line), "dummy_loop_us_bucket{le=\"+Inf\"} %lld\n",
                          static_cast<long long>(cum));
        out += line;
    }
    return out;
}

// ── 클라 하나 ──
enum class CState : uint8_t { Idle, Connecting, Joining, Active, Dead };

struct Client
{
    int      fd = -1;
    CState   state = CState::Idle;
    uint32_t seq = 0;
    int64_t  nextInputNs = 0;
    size_t   recvLen = 0;
    uint8_t  recvBuf[512];
    std::vector<uint8_t> pend;     // write 가 EAGAIN 일 때의 보류분 (상한 넘으면 게이트 위반)
    bool     wantWrite = false;
};

constexpr size_t kPendMax = 4096;

// ── 워커 ──
class Worker
{
public:
    void Start(int id, const char* host, uint16_t port, int clients,
               int inputHz, int rampPerLoop, int64_t deadlineNs)
    {
        _id = id; _host = host; _port = port; _inputHz = inputHz;
        _ramp = rampPerLoop; _deadline = deadlineNs;
        _clients.resize(clients);
        _thread = std::thread([this] { Loop(); });
    }
    void Join() { if (_thread.joinable()) _thread.join(); }

private:
    void Loop();
    void StartConnect(size_t idx);
    void OnEvent(size_t idx, uint32_t ev);
    void OnReadable(Client& c);
    void ParseFrames(Client& c);
    void SendBytes(Client& c, const void* data, size_t len);
    void FlushPend(Client& c);
    void UpdateWrite(Client& c, size_t idx, bool want);
    void Kill(Client& c, bool byServer);

    int _id = 0;
    const char* _host = nullptr;
    uint16_t _port = 0;
    int _inputHz = 60;
    int _ramp = 20;
    int64_t _deadline = 0;
    int _epfd = -1;
    size_t _nextToConnect = 0;
    std::vector<Client> _clients;
    std::thread _thread;

    // 로컬 누적 → 100ms 마다 일괄 반영
    struct Local
    {
        int64_t snapshots = 0, inputs = 0, loops = 0;
        int64_t buckets[kLoopBuckets] = {};
        int64_t maxUs = 0;
        void Flush()
        {
            g_stats.snapshots.Add(snapshots);  snapshots = 0;
            g_stats.inputsSent.Add(inputs);    inputs = 0;
            g_stats.loops.Add(loops);          loops = 0;
            for (int b = 0; b < kLoopBuckets; ++b) { g_stats.loopBucket[b].Add(buckets[b]); buckets[b] = 0; }
            g_stats.loopMaxUs.StoreMax(maxUs); maxUs = 0;
        }
    } _local;
};

void Worker::StartConnect(size_t idx)
{
    Client& c = _clients[idx];
    c.fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (c.fd < 0) { c.state = CState::Dead; g_stats.connectFail.Inc(); return; }

    int on = 1;
    ::setsockopt(c.fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(_port);
    ::inet_pton(AF_INET, _host, &addr.sin_addr);

    const int r = ::connect(c.fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (r != 0 && errno != EINPROGRESS)
    {
        ::close(c.fd); c.fd = -1; c.state = CState::Dead; g_stats.connectFail.Inc();
        return;
    }
    c.state = CState::Connecting;

    epoll_event ev{};
    ev.events   = EPOLLIN | EPOLLOUT | EPOLLRDHUP;   // OUT = 접속 완료 신호
    ev.data.u64 = idx;
    ::epoll_ctl(_epfd, EPOLL_CTL_ADD, c.fd, &ev);
}

void Worker::OnEvent(size_t idx, uint32_t ev)
{
    Client& c = _clients[idx];
    if (c.state == CState::Dead || c.fd < 0)
        return;

    if (ev & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) { Kill(c, true); return; }

    if (c.state == CState::Connecting && (ev & EPOLLOUT))
    {
        int err = 0; socklen_t len = sizeof(err);
        ::getsockopt(c.fd, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err != 0) { Kill(c, false); g_stats.connectFail.Inc(); return; }

        MSG_C2S_JOIN join{};
        join.header.size  = sizeof(join);
        join.header.type  = static_cast<uint16_t>(MsgType::C2S_JOIN);
        join.protocolVer  = kProtocolVersion;
        SendBytes(c, &join, sizeof(join));
        c.state = CState::Joining;
        UpdateWrite(c, idx, !c.pend.empty());
        return;
    }
    if (ev & EPOLLOUT)
    {
        FlushPend(c);
        if (c.pend.empty()) UpdateWrite(c, idx, false);
    }
    if (ev & EPOLLIN)
        OnReadable(c);
}

void Worker::OnReadable(Client& c)
{
    for (;;)
    {
        if (c.recvLen == sizeof(c.recvBuf)) { Kill(c, false); return; }   // 파싱 불능 — 방어
        const ssize_t n = ::read(c.fd, c.recvBuf + c.recvLen, sizeof(c.recvBuf) - c.recvLen);
        if (n > 0) { c.recvLen += static_cast<size_t>(n); ParseFrames(c); if (c.state == CState::Dead) return; continue; }
        if (n == 0) { Kill(c, true); return; }
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        if (errno == EINTR) continue;
        Kill(c, true); return;
    }
}

void Worker::ParseFrames(Client& c)
{
    size_t off = 0;
    while (c.recvLen - off >= sizeof(MsgHeader))
    {
        MsgHeader h{};
        std::memcpy(&h, c.recvBuf + off, sizeof(h));
        if (h.size < sizeof(MsgHeader) || h.size > MAX_PACKET_SIZE) { Kill(c, false); return; }
        if (c.recvLen - off < h.size) break;

        switch (static_cast<MsgType>(h.type))
        {
        case MsgType::S2C_JOIN_OK:
            if (c.state == CState::Joining) { c.state = CState::Active; g_stats.joinOk.Inc(); g_stats.active.Inc(); }
            break;
        case MsgType::S2C_JOIN_FAIL:
            g_stats.joinFail.Inc(); Kill(c, false); return;
        case MsgType::S2C_SNAPSHOT:
            ++_local.snapshots;
            break;
        default:
            break;      // PONG·ROOM_EVENT 등은 소비만
        }
        off += h.size;
    }
    if (off > 0)
    {
        std::memmove(c.recvBuf, c.recvBuf + off, c.recvLen - off);
        c.recvLen -= off;
    }
}

void Worker::SendBytes(Client& c, const void* data, size_t len)
{
    if (!c.pend.empty())
    {
        if (c.pend.size() + len > kPendMax) { g_stats.pendOverflow.Inc(); Kill(c, false); return; }
        const uint8_t* p = static_cast<const uint8_t*>(data);
        c.pend.insert(c.pend.end(), p, p + len);
        return;
    }
    ssize_t n = ::write(c.fd, data, len);
    if (n < 0) { if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) n = 0; else { Kill(c, true); return; } }
    if (static_cast<size_t>(n) < len)
    {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        c.pend.insert(c.pend.end(), p + n, p + len);
        c.wantWrite = false;    // UpdateWrite 가 실제 등록 — 호출자가 잇는다
    }
}

void Worker::FlushPend(Client& c)
{
    while (!c.pend.empty())
    {
        ssize_t n = ::write(c.fd, c.pend.data(), c.pend.size());
        if (n > 0) { c.pend.erase(c.pend.begin(), c.pend.begin() + n); continue; }
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        if (errno == EINTR) continue;
        Kill(c, true); return;
    }
}

void Worker::UpdateWrite(Client& c, size_t idx, bool want)
{
    if (c.wantWrite == want || c.fd < 0)
        return;
    epoll_event ev{};
    ev.events   = EPOLLIN | EPOLLRDHUP | (want ? EPOLLOUT : 0u);
    ev.data.u64 = idx;
    if (::epoll_ctl(_epfd, EPOLL_CTL_MOD, c.fd, &ev) == 0)
        c.wantWrite = want;
}

void Worker::Kill(Client& c, bool byServer)
{
    if (c.state == CState::Dead)
        return;
    if (c.state == CState::Active)
        g_stats.active.Add(-1);
    if (byServer)
        g_stats.serverClosed.Inc();
    if (c.fd >= 0) { ::epoll_ctl(_epfd, EPOLL_CTL_DEL, c.fd, nullptr); ::close(c.fd); c.fd = -1; }
    c.state = CState::Dead;
}

void Worker::Loop()
{
    char name[16];
    std::snprintf(name, sizeof(name), "dummy%d", _id);
    metrics::RegisterThisThread(name);

    _epfd = ::epoll_create1(0);
    const int64_t inputPeriodNs = 1000000000 / (_inputHz > 0 ? _inputHz : 60);
    int64_t lastFlush = NowNs();

    epoll_event events[256];
    for (;;)
    {
        const int64_t loopStart = NowNs();
        if (loopStart >= _deadline)
            break;

        // ramp: 루프당 R 개씩 접속 시작
        for (int r = 0; r < _ramp && _nextToConnect < _clients.size(); ++r)
            StartConnect(_nextToConnect++);

        const int n = ::epoll_wait(_epfd, events, 256, 1);
        const int64_t workStart = NowNs();

        for (int i = 0; i < n; ++i)
            OnEvent(static_cast<size_t>(events[i].data.u64), events[i].events);

        // 유지 입력 — ACTIVE 클라, 주기 도래분만
        for (size_t i = 0; i < _clients.size(); ++i)
        {
            Client& c = _clients[i];
            if (c.state != CState::Active || workStart < c.nextInputNs)
                continue;
            MSG_C2S_INPUT in{};
            in.header.size = sizeof(in);
            in.header.type = static_cast<uint16_t>(MsgType::C2S_INPUT);
            in.seq = ++c.seq;
            in.mx = 0; in.my = 0; in.attack = 0; in.skill = 0;   // 정지 유지 — 출구 발동 방지
            SendBytes(c, &in, sizeof(in));
            if (c.state == CState::Dead) continue;
            if (!c.pend.empty()) UpdateWrite(c, i, true);
            ++_local.inputs;
            c.nextInputNs = (c.nextInputNs == 0 ? workStart : c.nextInputNs) + inputPeriodNs;
        }

        // 루프 작업시간 (epoll_wait 제외)
        const int64_t workUs = (NowNs() - workStart) / 1000;
        ++_local.loops;
        if (workUs > _local.maxUs) _local.maxUs = workUs;
        int b = 0;
        while (b < kLoopBuckets - 1 && workUs > kLoopBucketUs[b]) ++b;
        ++_local.buckets[b];

        if (NowNs() - lastFlush > 100 * 1000000) { _local.Flush(); lastFlush = NowNs(); }
    }
    _local.Flush();

    for (size_t i = 0; i < _clients.size(); ++i)
        Kill(_clients[i], false);
    ::close(_epfd);
}

bool RaiseFdLimit()
{
    rlimit rl{};
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) return false;
    if (rl.rlim_cur < rl.rlim_max) { rl.rlim_cur = rl.rlim_max; setrlimit(RLIMIT_NOFILE, &rl); }
    getrlimit(RLIMIT_NOFILE, &rl);
    return rl.rlim_cur >= 8192;
}

} // namespace

int main(int argc, char** argv)
{
    const char* host = "127.0.0.1";
    uint16_t port = 15400, mport = 19160;
    int sessions = 500, threads = 0, inputHz = 60, runSecs = 15, ramp = 20;

    for (int i = 1; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--server") && i + 1 < argc) host = argv[++i];
        else if (!std::strcmp(argv[i], "--port") && i + 1 < argc) port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--sessions") && i + 1 < argc) sessions = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--threads") && i + 1 < argc) threads = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--input-hz") && i + 1 < argc) inputHz = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--run-secs") && i + 1 < argc) runSecs = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--ramp") && i + 1 < argc) ramp = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--metrics-port") && i + 1 < argc) mport = static_cast<uint16_t>(std::atoi(argv[++i]));
    }
    if (threads <= 0)
        threads = (sessions + 1499) / 1500;      // MMO 유효성 경계(1,800/스레드) 아래로

    if (!RaiseFdLimit()) { std::fprintf(stderr, "belt_dummy: RLIMIT 상향 실패\n"); return 1; }

    metrics::Server ms;
    ms.Start(mport, &BuildDummyText);

    std::printf("belt_dummy: %s:%u sessions %d / threads %d / input %dHz / %ds\n",
                host, static_cast<unsigned>(port), sessions, threads, inputHz, runSecs);

    const int64_t deadline = NowNs() + static_cast<int64_t>(runSecs) * 1000000000;
    std::vector<std::unique_ptr<Worker>> workers;
    int per = sessions / threads, extra = sessions % threads;
    for (int t = 0; t < threads; ++t)
    {
        auto w = std::make_unique<Worker>();
        w->Start(t, host, port, per + (t < extra ? 1 : 0), inputHz, ramp, deadline);
        workers.push_back(std::move(w));
    }
    for (auto& w : workers) w->Join();
    ms.Stop();

    // ── 자가 판정 (게이트) ──
    const int64_t joined = g_stats.joinOk.Load();
    const int64_t bufFull = g_stats.pendOverflow.Load();
    const int64_t closed = g_stats.serverClosed.Load();
    int64_t cum = 0, total = 0;
    int64_t bucketCum[kLoopBuckets];
    for (int b = 0; b < kLoopBuckets; ++b) { cum += g_stats.loopBucket[b].Load(); bucketCum[b] = cum; }
    total = cum;
    int64_t p99Le = -1;
    if (total > 0)
    {
        const int64_t target = (total * 99 + 99) / 100;
        for (int b = 0; b < kLoopBuckets; ++b)
            if (bucketCum[b] >= target) { p99Le = (b < kLoopBuckets - 1) ? kLoopBucketUs[b] : -1; break; }
    }
    const bool ok = joined == sessions && bufFull == 0 && closed == 0 &&
                    p99Le > 0 && p99Le <= 40000;
    std::printf("belt_dummy: joined %lld/%d, snapshots %lld, inputs %lld, "
                "bufFull %lld, serverClosed %lld, loop p99<=%lldus max %lldus — %s\n",
                static_cast<long long>(joined), sessions,
                static_cast<long long>(g_stats.snapshots.Load()),
                static_cast<long long>(g_stats.inputsSent.Load()),
                static_cast<long long>(bufFull), static_cast<long long>(closed),
                static_cast<long long>(p99Le), static_cast<long long>(g_stats.loopMaxUs.Load()),
                ok ? "GATE OK" : "GATE FAIL");
    return ok ? 0 : 1;
}
