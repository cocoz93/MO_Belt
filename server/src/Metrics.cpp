#include "Metrics.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <vector>

namespace metrics {

Counters g;

int64_t ThreadCpuNs()
{
    clockid_t cid;
    if (pthread_getcpuclockid(pthread_self(), &cid) != 0)
        return -1;
    timespec ts{};
    if (clock_gettime(cid, &ts) != 0)
        return -1;
    return static_cast<int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

namespace {

struct ThreadEntry
{
    std::string name;
    clockid_t   cid;
};

std::mutex               s_threadsLock;
std::vector<ThreadEntry> s_threads;

} // namespace

void RegisterThisThread(const char* name)
{
    clockid_t cid;
    if (pthread_getcpuclockid(pthread_self(), &cid) != 0)
        return;
    std::lock_guard<std::mutex> lk(s_threadsLock);
    s_threads.push_back({name, cid});
}

std::string BuildText()
{
    char line[128];
    std::string out;
    out.reserve(1024);

    auto put = [&](const char* name, int64_t v) {
        std::snprintf(line, sizeof(line), "%s %lld\n", name, static_cast<long long>(v));
        out += line;
    };

    put("belt_up",              g.up.Load());
    put("belt_accepts_total",   g.accepts.Load());
    put("belt_disconnects_total", g.disconnects.Load());
    put("belt_recv_bytes_total", g.recvBytes.Load());
    put("belt_send_bytes_total", g.sendBytes.Load());

    {
        std::lock_guard<std::mutex> lk(s_threadsLock);
        for (const auto& t : s_threads)
        {
            timespec ts{};
            if (clock_gettime(t.cid, &ts) != 0)
                continue;
            int64_t ns = static_cast<int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
            std::snprintf(line, sizeof(line), "belt_thread_cpu_ns{thread=\"%s\"} %lld\n",
                          t.name.c_str(), static_cast<long long>(ns));
            out += line;
        }
    }
    return out;
}

bool Server::Start(uint16_t port)
{
    _listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (_listenFd < 0)
        return false;

    int on = 1;
    ::setsockopt(_listenFd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // 측정 박스 로컬 스크레이프 전용
    addr.sin_port        = htons(port);
    if (::bind(_listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(_listenFd, 16) != 0)
    {
        ::close(_listenFd);
        _listenFd = -1;
        return false;
    }

    _running.store(true);
    _thread = std::thread([this] { Loop(); });
    return true;
}

void Server::Loop()
{
    while (_running.load())
    {
        int fd = ::accept(_listenFd, nullptr, nullptr);
        if (fd < 0)
        {
            if (!_running.load())
                break;      // Stop() 의 shutdown 이 깨운 것
            continue;       // EINTR 등 — 재시도
        }

        char discard[512];
        ssize_t ignored = ::read(fd, discard, sizeof(discard));   // 요청은 안 본다 — 뭐가 오든 전체 덤프
        (void)ignored;

        std::string body = BuildText();
        char header[160];
        int hlen = std::snprintf(header, sizeof(header),
                                 "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: text/plain; version=0.0.4\r\n"
                                 "Content-Length: %zu\r\n"
                                 "Connection: close\r\n\r\n",
                                 body.size());
        std::string resp(header, static_cast<size_t>(hlen));
        resp += body;

        size_t off = 0;
        while (off < resp.size())
        {
            ssize_t n = ::write(fd, resp.data() + off, resp.size() - off);
            if (n <= 0)
                break;
            off += static_cast<size_t>(n);
        }
        ::close(fd);
    }
}

void Server::Stop()
{
    if (!_running.exchange(false))
        return;
    if (_listenFd >= 0)
    {
        ::shutdown(_listenFd, SHUT_RDWR);   // 블로킹 accept 를 깨운다 — close 만으로는 안 깨어남
        ::close(_listenFd);
        _listenFd = -1;
    }
    if (_thread.joinable())
        _thread.join();
}

} // namespace metrics
