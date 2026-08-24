#pragma once
// 세션과 슬롯 풀 — 수명 규약(설계 4·5절 + 구현 플랜 「수명·해제 규약」):
//
//   · 세션 = netRef(소유 epoll 워커 보유) + gameRef(방·큐 엔트리 보유).
//     참조 합이 0이 되는 순간에만 슬롯을 반환하고 세대(gen)를 올린다.
//   · dead 는 소유 워커가 세운다. 이후 모든 경로는 만지기 전에 dead 를 본다.
//   · 세션 id = 슬롯 번호 + 세대 짝 — 소켓 번호(fd)는 재사용되므로 식별자로 쓰지 않는다(설계 5절).
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

#include "RingBuffer.h"

constexpr size_t kRecvRingSize = 8192;    // 프레임 상한(1KB) 대비 넉넉
constexpr size_t kSendRingSize = 16384;   // U2.3에서 "스냅샷 3~4개" 기준으로 재산정

struct Session
{
    std::atomic<int32_t> refs{0};
    std::atomic<bool>    dead{false};
    uint32_t gen   = 0;          // 슬롯 세대 — FreeSlot(풀 락 안)에서만 증가
    uint32_t idx   = 0;          // 슬롯 번호
    int      fd    = -1;
    uint8_t  owner = 0;          // 소유 epoll 워커 번호
    bool     wantWrite = false;  // EPOLLOUT 등록 표식 — 같은 상태 epoll_ctl 반복 방지 (MMO 승계)

    CRingBufferST recvQ;         // 소유 워커 전용 — 단일 소유라 락 불필요
    CRingBufferMT sendQ;         // 게임 워커(생산) / 소유 워커(소비). U2.3부터 생산자 등장

    Session() = default;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
};

class SessionPool
{
public:
    bool Init(uint32_t capacity)
    {
        _slots.reset(new (std::nothrow) Session[capacity]);
        if (_slots == nullptr)
            return false;
        _cap = capacity;
        _free.reserve(capacity);
        // 낮은 번호부터 뽑히게 역순으로 쌓는다
        for (uint32_t i = capacity; i > 0; --i)
        {
            _slots[i - 1].idx = i - 1;
            _free.push_back(i - 1);
        }
        return true;
    }

    // 실패 nullptr. 성공 시 refs=1(netRef)·dead=false 상태로 준다.
    Session* Alloc()
    {
        std::lock_guard<std::mutex> lk(_lock);
        if (_free.empty())
            return nullptr;
        const uint32_t idx = _free.back();

        Session& s = _slots[idx];
        if (!s.recvQ.IsValid())
        {
            // 첫 사용 — 링 확보. 실패하면 슬롯을 꺼내지 않은 것으로 한다.
            if (!s.recvQ.Init(kRecvRingSize) || !s.sendQ.Init(kSendRingSize))
                return nullptr;
        }
        else
        {
            // 재사용 — 버퍼는 그대로 두고 위치만 리셋
            s.recvQ.Clear();
            s.sendQ.Clear();
        }
        _free.pop_back();

        s.fd        = -1;
        s.wantWrite = false;
        s.dead.store(false, std::memory_order_relaxed);
        s.refs.store(1, std::memory_order_relaxed);      // netRef
        return &s;
    }

    void AddRef(Session& s) { s.refs.fetch_add(1, std::memory_order_relaxed); }

    // ref 하나 반환. 마지막이면 슬롯 회수 + 세대 증가 — 어느 스레드에서 떨어져도 안전
    // (회수는 풀 락 안의 목록 조작뿐, fd/epoll 은 소유 워커가 이미 끝냈다).
    void Release(Session& s)
    {
        if (s.refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
        {
            std::lock_guard<std::mutex> lk(_lock);
            ++s.gen;                     // 이 순간부터 옛 {idx,gen} 태그는 전부 무효
            _free.push_back(s.idx);
        }
    }

    Session& At(uint32_t idx) { return _slots[idx]; }
    uint32_t Capacity() const { return _cap; }

private:
    std::unique_ptr<Session[]> _slots;
    uint32_t                   _cap = 0;
    std::mutex                 _lock;
    std::vector<uint32_t>      _free;
};
