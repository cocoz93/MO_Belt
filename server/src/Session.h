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

    // 방 소속. inRoom·actorId 는 소유 epoll 워커 전용.
    // roomIdx 만 원자 — 방 이동 때 게임 워커가 새 방 번호를 써넣고(relaxed),
    // 소유 워커는 입력 라우팅·절단 때 읽는다. 이동 직후 옛 방으로 간 입력은
    // 소비 쪽 세대 대조가 버린다(유지 입력이라 다음 입력이 곧 갱신).
    bool                  inRoom  = false;
    std::atomic<uint32_t> roomIdx{0};
    uint8_t               actorId = 0;

    Session() = default;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
};

// dirty 비트맵 — 게임 워커가 "이 세션 송신링에 새 것 있음"을 표시하고,
// 소유 epoll 워커가 1ms 라운드마다 자기 몫을 exchange(0) 으로 걷어 간다 (설계 §4).
// 세션 구조체 안 플래그 순회(캐시라인 수백 개 × 1kHz)를 피하려고 밖에 dense 로 둔다.
#include <climits>
class DirtyMap
{
public:
    bool Init(uint32_t maxSessions, uint32_t workerCount)
    {
        _stride = (maxSessions + 63) / 64;
        _words.reset(new (std::nothrow) std::atomic<uint64_t>[_stride * workerCount]);
        if (_words == nullptr)
            return false;
        for (uint32_t i = 0; i < _stride * workerCount; ++i)
            _words[i].store(0, std::memory_order_relaxed);
        return true;
    }

    void Mark(uint8_t worker, uint32_t sessIdx)
    {
        _words[worker * _stride + sessIdx / 64]
            .fetch_or(1ull << (sessIdx % 64), std::memory_order_release);
    }

    // fn(sessIdx) — set 비트만 순회. 드레인 중 재set 은 다음 라운드가 회수한다.
    template <typename F>
    void Drain(uint8_t worker, F&& fn)
    {
        std::atomic<uint64_t>* base = &_words[worker * _stride];
        for (uint32_t w = 0; w < _stride; ++w)
        {
            uint64_t v = base[w].exchange(0, std::memory_order_acquire);
            while (v != 0)
            {
                const uint32_t b = static_cast<uint32_t>(__builtin_ctzll(v));
                fn(w * 64 + b);
                v &= v - 1;
            }
        }
    }

private:
    std::unique_ptr<std::atomic<uint64_t>[]> _words;
    uint32_t                                 _stride = 0;
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
        s.inRoom    = false;
        s.roomIdx.store(0, std::memory_order_relaxed);
        s.actorId   = 0;
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
