#pragma once
// 방과 방 매니저 — 설계 §5 + 구현 플랜 「수명·해제 규약」.
//
//   · 방은 생성 시 가장 한가한 게임 워커에 붙이고 소멸까지 안 옮긴다 (방 안 락 0 의 전제).
//   · 매니저는 저빈도(입장·생성·소멸)만 만진다. 좌석 예약·대기열은 방의 작은 뮤텍스로.
//   · 게임 워커는 틱 경계에서 넣을큐 먼저 → 뺄큐 나중 순서로 반영한다
//     (JOIN 직후 끊김 인터리브가 net 0 으로 상쇄되는 순서).
//   · 방 슬롯은 풀에서 재활용되고 소멸자를 부르지 않는다 — 뮤텍스·큐가 살아 있어
//     늦게 도착한 push 가 크래시 대신 무해한 no-op 이 된다 (엔트리의 세대 대조로 거른다).
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

#include "Session.h"

constexpr int kRoomMembers = 4;

// 게임 워커가 틱 경계에 처리할 대기열 엔트리
struct RoomJoinEntry
{
    uint32_t sessIdx;
    uint32_t sessGen;
    uint8_t  actorId;
};
struct RoomLeaveEntry
{
    uint32_t sessIdx;
    uint32_t sessGen;
};

struct Room
{
    uint32_t idx = 0;
    uint32_t gen = 0;                    // 슬롯 세대 — 풀 회수 시 증가

    uint8_t  gameWorker = 0;             // 소유 게임 워커 — 불변

    // ── 좌석 장부 (room.lock 아래) — 매니저(예약)와 게임 워커(해제)가 공유
    std::mutex lock;
    bool     dead = false;
    bool     everReserved = false;       // 한 번이라도 예약된 적 있나 — 빈 방 소멸 판정용
    bool     seatUsed[kRoomMembers] = {};
    int      reservedSeats = 0;          // == 방 참조수. 0 이고 dead 면 매니저가 회수
    std::vector<RoomJoinEntry>  pendingJoin;
    std::vector<RoomLeaveEntry> pendingLeave;

    // ── 멤버 실체 (소유 게임 워커 전용 — 락 없음)
    struct Member
    {
        bool     used = false;
        uint32_t sessIdx = 0;
        uint32_t sessGen = 0;
        uint8_t  sessOwner = 0;          // dirty 비트맵 표시용 (U2.3)
    };
    Member members[kRoomMembers];
    int    memberCount = 0;

    Room() = default;
    Room(const Room&) = delete;
    Room& operator=(const Room&) = delete;
};

class RoomManager
{
public:
    bool Init(uint32_t roomCapacity, unsigned gameWorkerCount, SessionPool* sessions);

    // epoll 워커가 JOIN 에서 부른다. 성공 시 좌석 예약 + gameRef 동봉 pendingJoin 적재.
    // 반환: true = ok(out 채움) / false = 만석·풀 고갈.
    bool Join(Session& s, uint8_t& outActorId, uint32_t& outRoomId);

    // epoll 워커가 절단 경로에서 부른다. 방 뺄큐에 적재만 — 실제 제거는 게임 워커 틱 경계.
    void OnSessionLeave(Session& s);

    // ── 게임 워커 쪽 (소유 워커 전용) ──
    void DrainRoomAddQ(uint8_t worker, std::vector<Room*>& outMyRooms);
    // 틱 경계 처리: 넣을큐 먼저 → 뺄큐 나중. 방이 비어 죽으면 true (목록에서 뺄 것).
    bool ProcessRoomQueues(Room& room);

private:
    Room* AllocRoom();
    void  FreeRoomIfDrained(Room& room);   // dead && reservedSeats==0 이면 풀로

    SessionPool* _sessions = nullptr;

    std::unique_ptr<Room[]> _slots;
    uint32_t                _cap = 0;

    std::mutex            _lock;           // 매니저 장부(가용 목록·열린 방·워커 부하)
    std::vector<uint32_t> _freeRooms;
    std::vector<uint32_t> _openRooms;      // 빈자리 있는 방
    unsigned              _workerCount = 1;
    uint32_t              _roomsPerWorker[16] = {};

    // 매니저 → 게임 워커 방 전달 우편함
    struct Mailbox
    {
        std::mutex         lock;
        std::vector<Room*> addQ;
    };
    Mailbox _mail[16];
};
