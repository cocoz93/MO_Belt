// belt_dummy — 부하 생성기 (수치의 정본, 설계 §12).
//
//   MMO 부하 클라의 개념만 계승해 리눅스 네이티브(WSL 내부)로 새로 짰다:
//   · 스레드당 epoll 단일 루프 · rampUp 점진 접속 · 60Hz 유지 입력 · 스냅샷 소비
//   · 로컬 누적 → 주기 일괄 반영(계측이 측정을 오염시키지 않게)
//   · "부하 생성기 자신이 포화했나" 게이트: 루프 작업시간 히스토그램 + 송신 보류 넘침 카운터
//     — 이 게이트가 깨진 런은 서버 수치도 무효다.
//
//   기본 입력은 mx=0(정지) — 이동하면 출구 방 이동이 발동해 방 수 판독이 흔들린다.
//
//   모드 둘:
//     --mode game (기본) : JOIN → 60Hz 입력 → 스냅샷 소비. 게임까지 포함한 부하.
//     --mode echo        : JOIN 없이 ECHO 만 왕복. 서버를 `--game 0` 으로 띄워 전송 계층만
//                          남긴 뒤 ⓐ 얼마나 빠른가(지연·CPU) ⓑ 제대로 오는가(무결성)를 함께 본다.
//                          무결성은 패킷 크기와 패딩을 **seq 에서 결정적으로 뽑아** 되받은 것과
//                          대조하는 식이다 — 바이트 훼손·크기 변조·순서 역전이 전부 여기서 걸린다.
//                          개수만 세는 지표(dropped 0 따위)로는 내용이 맞는지 알 수 없어서 넣었다.
//                          ⚠ 검증기 자체가 눈뜬장님이 아닌지는 tools/echo-faultcheck.sh 로 확인한다.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
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

// 왕복 지연 버킷 — 에코 모드에서만 채운다. 루프 버킷과 값이 같아도 뜻이 달라 따로 둔다.
constexpr int64_t kRttBucketUs[] = { 50, 100, 200, 500, 1000, 2000, 5000, 10000, 40000 };
constexpr int     kRttBuckets    = 10;

// 에코 모드 — 게임 워커를 끈 서버(`--game 0`)에 붙어 전송 계층만 재는 기준선용.
// JOIN 을 하지 않으므로 방·틱·스냅샷이 개입하지 않는다.
bool g_echoMode      = false;
bool g_verify        = true;    // 값·크기·패딩 대조 (끄면 지연만 재는 옛 동작)
int  g_pktMin        = 20;      // 에코 패킷 크기 범위 [min,max] — 두 값이 같으면 고정 크기
int  g_pktMax        = 256;     //   서버 MAX_PACKET_SIZE 는 1024 라 그 아래로 잡아야 한다
int  g_overSend      = 0;       // 미응답 허용 윈도우 (0 = 제한 없음 — 주기대로 계속 보냄)
int  g_echoTimeoutMs = 500;     // 이 시간 넘게 응답이 없으면 echoNotRecv
int  g_churnMs       = 0;       // >0 이면 [n,5n] 랜덤 시간 뒤 스스로 끊고 재접속
int  g_reconnectMs   = 1000;    // 끊긴 뒤 다시 붙기까지 대기
// sendQ 압박 — 앞 N 개 세션이 **받기를 그만두고 계속 보내기만** 한다.
// 그러면 커널 수신창이 차고 → 서버 write 가 EAGAIN → 보류(EPOLLOUT)·부분 전송·송신링 가득이
// 차례로 발동한다. 정상 부하로는 이 세 경로가 아예 안 밟혀서(경로 계측 0), 일부러 만든다.
int  g_attackSendQ   = 0;       // 공격 세션 수 (0 = 끔)
// 느린 소비자 — >0 이면 공격 세션이 "아예 안 읽는" 대신 이 주기로만 읽는다.
// 링이 가득 차 절단되기 전에 조금씩 비워주므로, 서버가 보류→재개를 되풀이한다.
// 완전 무시는 절단 경로를, 이쪽은 보류·부분 전송 경로를 깊게 파는 게 목적이다.
int  g_slowRecvMs    = 0;
bool g_failFast      = true;    // 무결성 위반 즉시 중단
std::atomic<bool> g_stop{false};

// ── 에코 패킷 규격 ──
//   [헤더 4B][seq 8B][보낸시각 8B][패딩 0~N]  — 최소 20B
//   크기와 패딩을 **둘 다 seq 에서 결정적으로 뽑는다**. 그래서 받은 쪽은 "이 seq 는 원래
//   몇 바이트에 어떤 내용이어야 하는가"를 다시 만들어 대조할 수 있다 —
//   크기 변조·바이트 훼손·순서 역전이 전부 여기서 걸린다.
#pragma pack(push, 1)
struct EchoHead
{
    MsgHeader header;
    uint64_t  seq;       // 세션마다 1 부터 증가
    int64_t   sentNs;    // 왕복 지연용
};
#pragma pack(pop)
static_assert(sizeof(EchoHead) == 20, "ECHO 머리 20B");
constexpr size_t kEchoMin = sizeof(EchoHead);

// splitmix64 — 시드 하나에서 되풀이 가능한 값을 뽑는다(표준 난수기는 구현마다 달라 못 쓴다)
uint64_t Mix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

// seq → 이 패킷의 전체 크기
uint16_t EchoSizeFor(uint64_t seed, uint64_t seq)
{
    if (g_pktMax <= g_pktMin)
        return static_cast<uint16_t>(g_pktMin);
    const uint64_t span = static_cast<uint64_t>(g_pktMax - g_pktMin + 1);
    return static_cast<uint16_t>(g_pktMin + Mix64(seed ^ (seq * 0xD1B54A32D192ED03ULL)) % span);
}

// seq → 패딩 내용 (머리 20B 뒤를 채운다)
void FillPad(uint8_t* p, size_t n, uint64_t seed, uint64_t seq)
{
    uint64_t s = seed ^ seq;
    for (size_t i = 0; i < n; i += 8)
    {
        s = Mix64(s);
        const size_t chunk = (n - i < 8) ? (n - i) : 8;
        std::memcpy(p + i, &s, chunk);
    }
}

// ── 무결성 위반 덤프 — 카운터만으로는 어느 세션의 몇 바이트가 틀렸는지 못 쫓는다 ──
std::mutex g_dumpLock;
int        g_dumpCount = 0;
const char* kDumpPath = "/tmp/belt-echo-integrity.log";

void DumpViolation(const char* kind, int worker, size_t idx, uint64_t seq,
                   const char* detail)
{
    std::lock_guard<std::mutex> lk(g_dumpLock);
    if (g_dumpCount >= 8)
        return;
    ++g_dumpCount;
    FILE* f = std::fopen(kDumpPath, "a");
    if (!f)
        return;
    std::fprintf(f, "[%s] worker=%d client=%zu seq=%llu %s\n",
                 kind, worker, idx, static_cast<unsigned long long>(seq), detail);
    std::fclose(f);
}

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
    Counter echoSent;        // 아래는 에코 모드 전용
    Counter echoRecv;
    Counter rttBucket[kRttBuckets];
    Counter rttMaxUs;
    Counter padError;        // payload 훼손 — 링버퍼 랩·코얼레싱·부분전송 경계 결함 신호. 반드시 0
    Counter orderError;      // 기대보다 큰 seq 도착 = 건너뜀(유실·뒤섞임). 반드시 0
    Counter lateArrival;     // 기대보다 작은 seq — 타임아웃 뒤 뒤늦게 도착
    Counter echoNotRecv;     // 미응답이 echoTimeoutMs 를 넘긴 세션 수
    Counter reconnects;      // churn 재접속 횟수
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
    if (g_echoMode)
    {
        put("dummy_echo_sent_total", g_stats.echoSent.Load());
        put("dummy_echo_recv_total", g_stats.echoRecv.Load());
        put("dummy_pad_error_total",   g_stats.padError.Load());
        put("dummy_order_error_total", g_stats.orderError.Load());
        put("dummy_packet_error_total", g_stats.padError.Load() + g_stats.orderError.Load());
        put("dummy_late_arrival_total", g_stats.lateArrival.Load());
        put("dummy_echo_not_recv_total", g_stats.echoNotRecv.Load());
        put("dummy_reconnects_total",  g_stats.reconnects.Load());
        put("dummy_pending_packets",   g_stats.echoSent.Load() - g_stats.echoRecv.Load());
        put("dummy_rtt_max_us",      g_stats.rttMaxUs.Load());
        int64_t rcum = 0;
        for (int b = 0; b < kRttBuckets; ++b)
        {
            rcum += g_stats.rttBucket[b].Load();
            if (b < kRttBuckets - 1)
                std::snprintf(line, sizeof(line), "dummy_rtt_us_bucket{le=\"%lld\"} %lld\n",
                              static_cast<long long>(kRttBucketUs[b]), static_cast<long long>(rcum));
            else
                std::snprintf(line, sizeof(line), "dummy_rtt_us_bucket{le=\"+Inf\"} %lld\n",
                              static_cast<long long>(rcum));
            out += line;
        }
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
    // 가변 크기(최대 1024) + 코얼레싱으로 여러 장이 한 번에 오므로 512 로는 모자란다
    uint8_t  recvBuf[4096];
    std::vector<uint8_t> pend;     // write 가 EAGAIN 일 때의 보류분 (상한 넘으면 게이트 위반)
    bool     wantWrite = false;

    // ── 에코 검증 ──
    uint64_t echoSeq    = 0;       // 다음에 보낼 값 (1 부터)
    uint64_t expectRecv = 1;       // 다음에 받을 값
    uint64_t seed       = 0;       // 세션 고유 — 크기·패딩 시드
    int      pending    = 0;       // 미응답 개수
    // 마지막으로 응답이 온 시각. 미응답이 쌓인 채로 이 시각에서 echoTimeoutMs 가 지나면 미응답 판정.
    // (MMO 도구는 송신 시각 deque 로 개별 추적하지만, 여기선 "응답이 끊긴 지 얼마나 됐나"면 충분하다)
    int64_t  lastRecvNs = 0;
    bool     notRecvFlagged = false;
    int64_t  churnAtNs     = 0;    // 이 시각에 스스로 끊는다 (0 = 안 끊음)
    int64_t  reconnectAtNs = 0;    // 끊긴 뒤 이 시각에 다시 붙는다 (0 = 안 붙음)
    bool     attacker      = false;   // sendQ 압박 — 받지 않고 보내기만 한다
    int64_t  nextRecvNs    = 0;       // 느린 소비자: 이 시각에 한 번 읽는다
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
    void OnReadable(size_t idx);
    void ParseFrames(size_t idx);
    void SendEcho(Client& c, size_t idx);
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
        int64_t echoSent = 0, echoRecv = 0;
        int64_t rttBuckets[kRttBuckets] = {};
        int64_t rttMaxUs = 0;
        void Flush()
        {
            g_stats.snapshots.Add(snapshots);  snapshots = 0;
            g_stats.inputsSent.Add(inputs);    inputs = 0;
            g_stats.loops.Add(loops);          loops = 0;
            for (int b = 0; b < kLoopBuckets; ++b) { g_stats.loopBucket[b].Add(buckets[b]); buckets[b] = 0; }
            g_stats.loopMaxUs.StoreMax(maxUs); maxUs = 0;
            g_stats.echoSent.Add(echoSent);    echoSent = 0;
            g_stats.echoRecv.Add(echoRecv);    echoRecv = 0;
            for (int b = 0; b < kRttBuckets; ++b) { g_stats.rttBucket[b].Add(rttBuckets[b]); rttBuckets[b] = 0; }
            g_stats.rttMaxUs.StoreMax(rttMaxUs); rttMaxUs = 0;
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

    // 에코 검증 상태 초기화 — 재접속이면 서버 쪽 세션도 새것이라 seq 를 1 부터 다시 센다.
    // 시드는 세션 자리마다 고정(재접속해도 유지) — 워커·자리로 갈라 세션 간 내용이 겹치지 않게 한다.
    if (c.seed == 0)
        c.seed = Mix64((static_cast<uint64_t>(_id) << 32) ^ (idx + 1));
    // 공격 세션은 워커 0 의 앞자리로 고정 — 나머지는 정상 에코라 비교군이 된다
    c.attacker = (g_attackSendQ > 0 && _id == 0 && idx < static_cast<size_t>(g_attackSendQ));
    if (c.attacker && g_slowRecvMs > 0)
    {
        // 느린 소비자만 수신창을 좁힌다 — 기본 버퍼(수백 KB)로는 조금씩 읽어도 커널이 다 받아줘서
        // 서버 송신이 막히질 않는다(30ms 주기 실측: 보류 0).
        // ⚠ 반대로 "아예 안 읽는" 쪽에 이걸 걸면 우리 송신이 먼저 막혀 서버까지 부하가 안 간다
        //   (④ 실측: 클라 보류만 넘치고 서버 링가득 0). 그쪽은 넓은 창으로 밀어야 한다.
        int rb = 4096;
        ::setsockopt(c.fd, SOL_SOCKET, SO_RCVBUF, &rb, sizeof(rb));
    }
    c.echoSeq = 0;
    c.expectRecv = 1;
    c.pending = 0;
    c.lastRecvNs = NowNs();
    c.notRecvFlagged = false;
    c.recvLen = 0;
    c.pend.clear();
    c.reconnectAtNs = 0;
    c.churnAtNs = 0;
    if (g_churnMs > 0)
    {
        // 접속 유지 시간 = [churnMs, 5×churnMs] 랜덤
        const uint64_t span = static_cast<uint64_t>(g_churnMs) * 4 + 1;
        const int64_t hold = g_churnMs + static_cast<int64_t>(Mix64(c.seed + c.echoSeq + static_cast<uint64_t>(NowNs())) % span);
        c.churnAtNs = NowNs() + hold * 1000000;
    }

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

        if (g_echoMode)
        {
            // 에코 모드는 JOIN 을 건너뛴다 — 붙은 즉시 ACTIVE 로 보고 ECHO 만 주고받는다.
            // joinOk 는 여기선 「접속 완료 누계」로 쓴다(active 는 종료 때 0 으로 돌아가 게이트에 못 쓴다).
            c.state = CState::Active;
            g_stats.active.Inc();
            g_stats.joinOk.Inc();
            // 공격 세션은 여기서 EPOLLIN 을 떼고 다시는 읽지 않는다 — 커널 수신창을 채워
            // 서버 송신 경로(보류·부분 전송·링 가득)를 밀어붙이는 게 목적이다.
            if (c.attacker)
            {
                epoll_event ea{};
                ea.events   = EPOLLRDHUP;      // EPOLLIN 없음 = 안 읽는다
                ea.data.u64 = idx;
                ::epoll_ctl(_epfd, EPOLL_CTL_MOD, c.fd, &ea);
                c.wantWrite = false;
                if (g_slowRecvMs > 0)
                    c.nextRecvNs = NowNs() + static_cast<int64_t>(g_slowRecvMs) * 1000000;
                return;
            }
            UpdateWrite(c, idx, !c.pend.empty());
            return;
        }

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
    {
        OnReadable(idx);
        // 느린 소비자는 한 번 비우고 다시 귀를 닫는다 — 서버를 다시 보류로 밀어 넣는다
        if (c.attacker && g_slowRecvMs > 0 && c.state == CState::Active)
        {
            epoll_event ea{};
            ea.events   = EPOLLRDHUP | (c.wantWrite ? EPOLLOUT : 0u);
            ea.data.u64 = idx;
            ::epoll_ctl(_epfd, EPOLL_CTL_MOD, c.fd, &ea);
            c.nextRecvNs = NowNs() + static_cast<int64_t>(g_slowRecvMs) * 1000000;
        }
    }
}

void Worker::OnReadable(size_t idx)
{
    Client& c = _clients[idx];
    for (;;)
    {
        if (c.recvLen == sizeof(c.recvBuf)) { Kill(c, false); return; }   // 파싱 불능 — 방어
        // 느린 소비자는 한 번에 한 술만 뜬다 — 커널 수신창이 조금씩만 열려야
        // 서버 write 가 "일부만 나가는"(부분 전송) 상태에 들어간다.
        const size_t room = (c.attacker && g_slowRecvMs > 0)
                            ? (sizeof(c.recvBuf) - c.recvLen < 2048 ? sizeof(c.recvBuf) - c.recvLen : 2048)
                            : sizeof(c.recvBuf) - c.recvLen;
        const ssize_t n = ::read(c.fd, c.recvBuf + c.recvLen, room);
        if (n > 0)
        {
            c.recvLen += static_cast<size_t>(n);
            ParseFrames(idx);
            if (c.state == CState::Dead) return;
            if (c.attacker && g_slowRecvMs > 0) return;   // 이번 차례는 여기까지
            continue;
        }
        if (n == 0) { Kill(c, true); return; }
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        if (errno == EINTR) continue;
        Kill(c, true); return;
    }
}

void Worker::ParseFrames(size_t idx)
{
    Client& c = _clients[idx];
    size_t off = 0;
    while (c.recvLen - off >= sizeof(MsgHeader))
    {
        MsgHeader h{};
        std::memcpy(&h, c.recvBuf + off, sizeof(h));
        if (h.size < sizeof(MsgHeader) || h.size > MAX_PACKET_SIZE) { Kill(c, false); return; }
        if (c.recvLen - off < h.size) break;

        switch (static_cast<MsgType>(h.type))
        {
        case MsgType::ECHO:
        {
            char det[160];
            if (h.size < kEchoMin)
            {
                g_stats.padError.Inc();
                std::snprintf(det, sizeof(det), "머리보다 짧다: size=%u (최소 %zu)",
                              static_cast<unsigned>(h.size), kEchoMin);
                DumpViolation("SIZE", _id, idx, 0, det);
                if (g_failFast) g_stop.store(true, std::memory_order_relaxed);
                Kill(c, false);
                return;
            }
            EchoHead eh{};
            std::memcpy(&eh, c.recvBuf + off, sizeof(eh));

            // 지연은 대조 결과와 무관하게 기록한다 — 무결성과 지연은 따로 봐야 한다
            const int64_t now   = NowNs();
            const int64_t rttUs = (now - eh.sentNs) / 1000;
            ++_local.echoRecv;
            if (rttUs > _local.rttMaxUs) _local.rttMaxUs = rttUs;
            int rb = 0;
            while (rb < kRttBuckets - 1 && rttUs > kRttBucketUs[rb]) ++rb;
            ++_local.rttBuckets[rb];
            if (c.pending > 0) --c.pending;
            c.lastRecvNs = now;
            c.notRecvFlagged = false;

            if (g_verify)
            {
                // ① 크기 — 이 seq 는 원래 몇 바이트여야 하는가
                const uint16_t want = EchoSizeFor(c.seed, eh.seq);
                if (want != h.size)
                {
                    g_stats.padError.Inc();
                    std::snprintf(det, sizeof(det), "크기 불일치: 기대 %u, 실제 %u",
                                  static_cast<unsigned>(want), static_cast<unsigned>(h.size));
                    DumpViolation("SIZE", _id, idx, eh.seq, det);
                    if (g_failFast) g_stop.store(true, std::memory_order_relaxed);
                }
                else if (h.size > kEchoMin)
                {
                    // ② 패딩 — 같은 시드로 다시 만들어 바이트째 대조
                    const size_t   padLen = h.size - kEchoMin;
                    const uint8_t* got    = c.recvBuf + off + kEchoMin;
                    uint8_t expect[MAX_PACKET_SIZE];
                    FillPad(expect, padLen, c.seed, eh.seq);
                    if (std::memcmp(expect, got, padLen) != 0)
                    {
                        size_t bad = 0;
                        while (bad < padLen && expect[bad] == got[bad]) ++bad;
                        g_stats.padError.Inc();
                        std::snprintf(det, sizeof(det),
                                      "패딩 훼손: size=%u 패딩 %zuB 중 오프셋 %zu 부터 (기대 0x%02X, 실제 0x%02X)",
                                      static_cast<unsigned>(h.size), padLen, bad,
                                      expect[bad], got[bad]);
                        DumpViolation("PAD", _id, idx, eh.seq, det);
                        if (g_failFast) g_stop.store(true, std::memory_order_relaxed);
                    }
                }

                // ③ 순서 — 건너뛴 값이 오면 유실·뒤섞임
                if (eh.seq == c.expectRecv)
                {
                    ++c.expectRecv;
                }
                else if (eh.seq < c.expectRecv)
                {
                    g_stats.lateArrival.Inc();
                }
                else
                {
                    g_stats.orderError.Inc();
                    std::snprintf(det, sizeof(det), "순서 역전: 기대 %llu, 실제 %llu (%llu 개 건너뜀)",
                                  static_cast<unsigned long long>(c.expectRecv),
                                  static_cast<unsigned long long>(eh.seq),
                                  static_cast<unsigned long long>(eh.seq - c.expectRecv));
                    DumpViolation("ORDER", _id, idx, eh.seq, det);
                    if (g_failFast) g_stop.store(true, std::memory_order_relaxed);
                    c.expectRecv = eh.seq + 1;
                }
            }
            break;
        }
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
    ev.events   = (c.attacker ? 0u : EPOLLIN) | EPOLLRDHUP | (want ? EPOLLOUT : 0u);
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
    // churn 모드면 서버가 끊었든 스스로 끊었든 다시 붙는다 (세션 수명 관리를 부하 중에 때린다)
    if (g_churnMs > 0)
        c.reconnectAtNs = NowNs() + static_cast<int64_t>(g_reconnectMs) * 1000000;
}

void Worker::SendEcho(Client& c, size_t idx)
{
    const uint64_t seq  = ++c.echoSeq;
    const uint16_t size = EchoSizeFor(c.seed, seq);

    uint8_t  buf[MAX_PACKET_SIZE];
    EchoHead eh{};
    eh.header.size = size;
    eh.header.type = static_cast<uint16_t>(MsgType::ECHO);
    eh.seq         = seq;
    eh.sentNs      = NowNs();
    std::memcpy(buf, &eh, sizeof(eh));
    if (size > kEchoMin)
        FillPad(buf + kEchoMin, size - kEchoMin, c.seed, seq);

    SendBytes(c, buf, size);
    if (c.state == CState::Dead)
        return;
    if (!c.pend.empty())
        UpdateWrite(c, idx, true);
    ++_local.echoSent;
    ++c.pending;
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
        // 무결성 위반이 나면 즉시 멈춘다 — 더 돌려봐야 증거만 덮이고, 그 뒤 수치는 이미 못 믿는다
        if (loopStart >= _deadline || g_stop.load(std::memory_order_relaxed))
            break;

        // ramp: 루프당 R 개씩 접속 시작
        for (int r = 0; r < _ramp && _nextToConnect < _clients.size(); ++r)
            StartConnect(_nextToConnect++);

        const int n = ::epoll_wait(_epfd, events, 256, 1);
        const int64_t workStart = NowNs();

        for (int i = 0; i < n; ++i)
            OnEvent(static_cast<size_t>(events[i].data.u64), events[i].events);

        // 유지 송신 + 검증 상태 관리 — ACTIVE 클라, 주기 도래분만
        for (size_t i = 0; i < _clients.size(); ++i)
        {
            Client& c = _clients[i];

            // 끊긴 세션 되살리기 (churn)
            if (c.state == CState::Dead && c.reconnectAtNs != 0 && workStart >= c.reconnectAtNs)
            {
                StartConnect(i);
                g_stats.reconnects.Inc();
                continue;
            }
            if (c.state != CState::Active)
                continue;

            // 스스로 끊을 차례 (churn) — 부하 한가운데서 세션이 나가고 들어온다
            if (c.churnAtNs != 0 && workStart >= c.churnAtNs)
            {
                Kill(c, false);
                continue;
            }

            // 응답이 끊긴 지 오래됐나 — 세션당 한 번만 센다(응답 오면 플래그 해제)
            if (g_echoMode && c.pending > 0 && !c.notRecvFlagged &&
                workStart - c.lastRecvNs > static_cast<int64_t>(g_echoTimeoutMs) * 1000000)
            {
                g_stats.echoNotRecv.Inc();
                c.notRecvFlagged = true;
            }

            // 공격 세션은 주기·창 제한을 무시하고 매 루프 민다. 받지를 않으니 미응답은 계속 늘고,
            // 서버 쪽 송신이 먼저 막힌다(우리 송신은 서버가 계속 읽으므로 안 막힌다).
            if (g_echoMode && c.attacker)
            {
                // 느린 소비자: 읽을 차례가 되면 귀를 잠깐 연다
                if (g_slowRecvMs > 0 && c.nextRecvNs != 0 && workStart >= c.nextRecvNs)
                {
                    epoll_event ea{};
                    ea.events   = EPOLLIN | EPOLLRDHUP | (c.wantWrite ? EPOLLOUT : 0u);
                    ea.data.u64 = i;
                    ::epoll_ctl(_epfd, EPOLL_CTL_MOD, c.fd, &ea);
                    c.nextRecvNs = 0;      // 다음 수신 뒤 OnEvent 가 다시 잡는다
                }
                // 여기서 송신을 조이면 서버가 되돌릴 양이 줄어 정작 링이 안 찬다(④ 실측: 보류 0).
                // 우리 쪽 보류 넘침은 서버가 이 세션을 끊은 뒤의 잔여 송신이라 게이트에서 따로 면제한다.
                SendEcho(c, i);
                continue;
            }

            if (workStart < c.nextInputNs)
                continue;

            if (g_echoMode)
            {
                // 과부하 윈도우 — 미응답이 상한이면 이번 차례는 거른다 (0 = 제한 없음)
                if (g_overSend > 0 && c.pending >= g_overSend)
                {
                    c.nextInputNs = (c.nextInputNs == 0 ? workStart : c.nextInputNs) + inputPeriodNs;
                    continue;
                }
                SendEcho(c, i);
                if (c.state == CState::Dead) continue;
            }
            else
            {
                MSG_C2S_INPUT in{};
                in.header.size = sizeof(in);
                in.header.type = static_cast<uint16_t>(MsgType::C2S_INPUT);
                in.seq = ++c.seq;
                in.mx = 0; in.my = 0; in.attack = 0; in.skill = 0;   // 정지 유지 — 출구 발동 방지
                SendBytes(c, &in, sizeof(in));
                if (c.state == CState::Dead) continue;
                if (!c.pend.empty()) UpdateWrite(c, i, true);
                ++_local.inputs;
            }
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
        else if (!std::strcmp(argv[i], "--mode") && i + 1 < argc) g_echoMode = (std::strcmp(argv[++i], "echo") == 0);
        else if (!std::strcmp(argv[i], "--verify") && i + 1 < argc) g_verify = std::atoi(argv[++i]) != 0;
        else if (!std::strcmp(argv[i], "--pkt-min") && i + 1 < argc) g_pktMin = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--pkt-max") && i + 1 < argc) g_pktMax = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--over-send") && i + 1 < argc) g_overSend = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--echo-timeout-ms") && i + 1 < argc) g_echoTimeoutMs = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--churn-ms") && i + 1 < argc) g_churnMs = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--reconnect-ms") && i + 1 < argc) g_reconnectMs = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--fail-fast") && i + 1 < argc) g_failFast = std::atoi(argv[++i]) != 0;
        else if (!std::strcmp(argv[i], "--attack-sendq") && i + 1 < argc) g_attackSendQ = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--slow-recv-ms") && i + 1 < argc) g_slowRecvMs = std::atoi(argv[++i]);
    }

    // 서버가 끊은 소켓에 마저 쓰다 SIGPIPE 로 죽으면 그 런 전체가 무효다 — 서버와 같은 처리를 한다
    ::signal(SIGPIPE, SIG_IGN);

    // 크기 범위 정리 — 머리 20B 미만이면 검증 자체가 성립 안 하고, 서버 상한을 넘으면 서버가 끊는다
    if (g_pktMin < static_cast<int>(kEchoMin)) g_pktMin = static_cast<int>(kEchoMin);
    if (g_pktMax > static_cast<int>(MAX_PACKET_SIZE)) g_pktMax = static_cast<int>(MAX_PACKET_SIZE);
    if (g_pktMax < g_pktMin) g_pktMax = g_pktMin;
    if (threads <= 0)
        threads = (sessions + 1499) / 1500;      // MMO 유효성 경계(1,800/스레드) 아래로

    if (!RaiseFdLimit()) { std::fprintf(stderr, "belt_dummy: RLIMIT 상향 실패\n"); return 1; }

    metrics::Server ms;
    ms.Start(mport, &BuildDummyText);

    std::printf("belt_dummy: %s:%u sessions %d / threads %d / %s %dHz / %ds\n",
                host, static_cast<unsigned>(port), sessions, threads,
                g_echoMode ? "echo" : "input", inputHz, runSecs);
    if (g_echoMode)
        std::printf("belt_dummy: 검증 %s / 패킷 %d~%dB / 미응답창 %d / 타임아웃 %dms / churn %dms / failFast %s\n",
                    g_verify ? "켬" : "끔", g_pktMin, g_pktMax, g_overSend,
                    g_echoTimeoutMs, g_churnMs, g_failFast ? "켬" : "끔");

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
    // 히스토그램에서 p99 이 걸리는 버킷 상한을 돌려준다 (-1 = 최상단 초과 또는 표본 없음)
    auto p99Of = [](const Counter* buckets, const int64_t* edges, int n) -> int64_t {
        int64_t cum = 0;
        std::vector<int64_t> bucketCum(static_cast<size_t>(n));
        for (int b = 0; b < n; ++b) { cum += buckets[b].Load(); bucketCum[static_cast<size_t>(b)] = cum; }
        if (cum == 0) return -1;
        const int64_t target = (cum * 99 + 99) / 100;
        for (int b = 0; b < n; ++b)
            if (bucketCum[static_cast<size_t>(b)] >= target)
                return (b < n - 1) ? edges[b] : -1;
        return -1;
    };

    const int64_t joined  = g_stats.joinOk.Load();     // 에코 모드에선 「접속 완료 누계」
    const int64_t bufFull = g_stats.pendOverflow.Load();
    const int64_t closed  = g_stats.serverClosed.Load();
    const int64_t p99Le   = p99Of(g_stats.loopBucket, kLoopBucketUs, kLoopBuckets);
    const bool    loopOk  = p99Le > 0 && p99Le <= 40000;

    if (g_echoMode)
    {
        const int64_t sent   = g_stats.echoSent.Load();
        const int64_t recv   = g_stats.echoRecv.Load();
        const int64_t padErr = g_stats.padError.Load();
        const int64_t ordErr = g_stats.orderError.Load();
        const int64_t rttP99 = p99Of(g_stats.rttBucket, kRttBucketUs, kRttBuckets);

        // churn 을 켜면 세션이 오가므로 접속 누계·서버발 끊김은 게이트에서 뺀다.
        // sendQ 압박은 "서버가 그 세션을 끊는 것"이 기대 동작이라 마찬가지로 뺀다
        // (끊지 않으면 방어가 안 도는 것이므로 아래에서 따로 본다).
        const bool churning  = g_churnMs > 0 || g_attackSendQ > 0;
        const bool connectOk = churning ? (joined >= sessions) : (joined == sessions);
        const bool closeOk   = churning ? true : (closed == 0);
        // 마지막 왕복은 아직 오는 중일 수 있다 — 세션당 1개까지만 봐준다.
        // 과부하 창을 열어두면 미응답이 그만큼 더 남으므로 상한도 그만큼 넓힌다.
        const int64_t inflightAllow = static_cast<int64_t>(sessions) *
                                      (g_overSend > 0 ? g_overSend : 1);
        // 공격 세션은 애초에 받지를 않으니 미도착 검사는 뜻이 없다. 대신 서버가 끊었는지를 본다 —
        // 안 끊으면 송신링 방어가 안 도는 것이라 그게 실패다.
        const bool inflightOk = (g_attackSendQ > 0) || (recv >= sent - inflightAllow);
        // 완전히 안 읽는 공격이면 서버가 끊어야 정상. 느린 소비자는 끊길 이유가 없으므로 요구하지 않는다.
        const bool attackOk   = (g_attackSendQ == 0) || (g_slowRecvMs > 0) || (closed > 0);
        // 공격 런에서는 서버가 그 세션을 끊은 뒤에도 우리가 밀던 게 남아 보류가 넘칠 수 있다 —
        // 그건 더미가 포화한 게 아니라 절단의 뒷정리라 면제한다.
        const bool bufOk = (g_attackSendQ > 0) || (bufFull == 0);
        const bool ok = connectOk && closeOk && inflightOk && attackOk &&
                        padErr == 0 && ordErr == 0 && bufOk && loopOk;

        std::printf("belt_dummy: connected %lld/%d, echo %lld sent / %lld recv (%lld 미도착), "
                    "padErr %lld, orderErr %lld, late %lld, notRecv %lld, reconnect %lld, "
                    "rtt p99<=%lldus max %lldus, bufFull %lld, serverClosed %lld, "
                    "loop p99<=%lldus max %lldus — %s\n",
                    static_cast<long long>(joined), sessions,
                    static_cast<long long>(sent), static_cast<long long>(recv),
                    static_cast<long long>(sent - recv),
                    static_cast<long long>(padErr), static_cast<long long>(ordErr),
                    static_cast<long long>(g_stats.lateArrival.Load()),
                    static_cast<long long>(g_stats.echoNotRecv.Load()),
                    static_cast<long long>(g_stats.reconnects.Load()),
                    static_cast<long long>(rttP99), static_cast<long long>(g_stats.rttMaxUs.Load()),
                    static_cast<long long>(bufFull), static_cast<long long>(closed),
                    static_cast<long long>(p99Le), static_cast<long long>(g_stats.loopMaxUs.Load()),
                    ok ? "GATE OK" : "GATE FAIL");
        if (padErr != 0 || ordErr != 0)
            std::printf("belt_dummy: ★ 무결성 위반 — 자세한 내용은 %s\n", kDumpPath);
        return ok ? 0 : 1;
    }

    const bool ok = joined == sessions && bufFull == 0 && closed == 0 && loopOk;
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
