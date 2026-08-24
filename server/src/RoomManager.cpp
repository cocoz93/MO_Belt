#include "Room.h"

#include "Metrics.h"

bool RoomManager::Init(uint32_t roomCapacity, unsigned gameWorkerCount, SessionPool* sessions)
{
    _sessions    = sessions;
    _workerCount = gameWorkerCount;    // 0 이면 Join 이 전부 실패한다(게임 워커 없이는 방을 못 돌린다)

    _slots.reset(new (std::nothrow) Room[roomCapacity]);
    if (_slots == nullptr)
        return false;
    _cap = roomCapacity;

    _freeRooms.reserve(roomCapacity);
    for (uint32_t i = roomCapacity; i > 0; --i)
    {
        _slots[i - 1].idx = i - 1;
        _freeRooms.push_back(i - 1);
    }
    return true;
}

Room* RoomManager::AllocRoom()
{
    // _lock 을 잡은 채로만 불린다
    if (_freeRooms.empty())
        return nullptr;
    const uint32_t idx = _freeRooms.back();
    _freeRooms.pop_back();

    Room& r = _slots[idx];
    {
        std::lock_guard<std::mutex> rl(r.lock);
        r.dead         = false;
        r.everReserved = false;
        r.reservedSeats = 0;
        for (int i = 0; i < kRoomMembers; ++i)
            r.seatUsed[i] = false;
        r.pendingJoin.clear();
        r.pendingLeave.clear();
    }
    for (int i = 0; i < kRoomMembers; ++i)
        r.members[i].used = false;
    r.memberCount = 0;

    // 가장 한가한 워커에 붙인다 — roomId % K 금지 (설계 §5)
    unsigned best = 0;
    for (unsigned w = 1; w < _workerCount; ++w)
        if (_roomsPerWorker[w] < _roomsPerWorker[best])
            best = w;
    r.gameWorker = static_cast<uint8_t>(best);
    ++_roomsPerWorker[best];

    {
        std::lock_guard<std::mutex> ml(_mail[best].lock);
        _mail[best].addQ.push_back(&r);
    }

    metrics::g.roomsCreated.Inc();
    metrics::g.roomsActive.Inc();
    return &r;
}

bool RoomManager::Join(Session& s, uint8_t& outActorId, uint32_t& outRoomId)
{
    if (_workerCount == 0)
        return false;

    std::lock_guard<std::mutex> lk(_lock);

    // 빈자리 있는 방을 뒤에서부터 — 없으면 새로 만든다
    Room* room = nullptr;
    int   seat = -1;
    while (!_openRooms.empty())
    {
        Room& cand = _slots[_openRooms.back()];
        std::lock_guard<std::mutex> rl(cand.lock);
        if (!cand.dead)
        {
            for (int i = 0; i < kRoomMembers; ++i)
                if (!cand.seatUsed[i]) { seat = i; break; }
            if (seat >= 0)
            {
                cand.seatUsed[seat]  = true;
                cand.everReserved    = true;
                ++cand.reservedSeats;
                if (cand.reservedSeats == kRoomMembers)
                    _openRooms.pop_back();          // 만석 — 열린 목록에서 제외
                room = &cand;
                break;
            }
        }
        _openRooms.pop_back();                      // 죽었거나 만석 — 목록 정리
    }

    if (room == nullptr)
    {
        room = AllocRoom();
        if (room == nullptr)
            return false;                            // 방 풀 고갈
        std::lock_guard<std::mutex> rl(room->lock);
        seat = 0;
        room->seatUsed[0]   = true;
        room->everReserved  = true;
        room->reservedSeats = 1;
        _openRooms.push_back(room->idx);
    }

    // gameRef 동봉 — pendingJoin 엔트리가 세션을 붙잡는다 (수명 규약)
    _sessions->AddRef(s);
    {
        std::lock_guard<std::mutex> rl(room->lock);
        room->pendingJoin.push_back({ s.idx, s.gen, static_cast<uint8_t>(seat) });
    }

    s.inRoom  = true;
    s.roomIdx = room->idx;
    s.actorId = static_cast<uint8_t>(seat);

    outActorId = static_cast<uint8_t>(seat);
    outRoomId  = room->idx;
    metrics::g.joins.Inc();
    return true;
}

void RoomManager::OnSessionLeave(Session& s)
{
    if (!s.inRoom || s.roomIdx >= _cap)
        return;
    s.inRoom = false;

    Room& room = _slots[s.roomIdx];
    std::lock_guard<std::mutex> rl(room.lock);
    // 죽은 방이어도 무해 — 엔트리는 세대 대조로 걸러진다
    room.pendingLeave.push_back({ s.idx, s.gen });
}

void RoomManager::DrainRoomAddQ(uint8_t worker, std::vector<Room*>& outMyRooms)
{
    Mailbox& m = _mail[worker];
    std::lock_guard<std::mutex> lk(m.lock);
    for (Room* r : m.addQ)
        outMyRooms.push_back(r);
    m.addQ.clear();
}

bool RoomManager::ProcessRoomQueues(Room& room)
{
    // 대기열만 락 아래에서 빼오고, 멤버 실체 조작은 락 밖(소유 워커 전용)에서 한다
    std::vector<RoomJoinEntry>  joins;
    std::vector<RoomLeaveEntry> leaves;
    {
        std::lock_guard<std::mutex> rl(room.lock);
        joins.swap(room.pendingJoin);
        leaves.swap(room.pendingLeave);
    }

    // ── 넣을큐 먼저 ──
    for (const RoomJoinEntry& e : joins)
    {
        Session& s = _sessions->At(e.sessIdx);
        if (s.gen != e.sessGen || s.dead.load(std::memory_order_acquire))
        {
            // JOIN 직후 끊김 — 자리·참조를 되돌린다 (skip + 동봉 ref 즉시 반환)
            {
                std::lock_guard<std::mutex> rl(room.lock);
                room.seatUsed[e.actorId] = false;
                --room.reservedSeats;
            }
            _sessions->Release(s);
            continue;
        }
        Room::Member& m = room.members[e.actorId];
        m.used      = true;
        m.sessIdx   = e.sessIdx;
        m.sessGen   = e.sessGen;
        m.sessOwner = s.owner;
        ++room.memberCount;
        // (U3.1) 여기서 액터 스폰 — 스폰표 §9
    }

    // ── 뺄큐 나중 ──
    for (const RoomLeaveEntry& e : leaves)
    {
        for (int i = 0; i < kRoomMembers; ++i)
        {
            Room::Member& m = room.members[i];
            if (m.used && m.sessIdx == e.sessIdx && m.sessGen == e.sessGen)
            {
                m.used = false;
                --room.memberCount;
                {
                    std::lock_guard<std::mutex> rl(room.lock);
                    room.seatUsed[i] = false;
                    --room.reservedSeats;
                }
                _sessions->Release(_sessions->At(e.sessIdx));   // 멤버가 쥐던 gameRef 반환
                break;
            }
        }
        // 못 찾으면 no-op — 넣을큐에서 이미 skip 됐거나(즉끊) 옛 세대
    }

    // ── 빈 방 소멸 판정 (락 아래에서 dead 확정 — Join 의 예약과 같은 락) ──
    bool died = false;
    {
        std::lock_guard<std::mutex> rl(room.lock);
        if (!room.dead && room.everReserved && room.reservedSeats == 0 &&
            room.pendingJoin.empty())
        {
            room.dead = true;
            died = true;
        }
    }
    if (died)
    {
        std::lock_guard<std::mutex> lk(_lock);
        --_roomsPerWorker[room.gameWorker];
        // 열린 목록에서 제거 (있다면)
        for (size_t i = 0; i < _openRooms.size(); ++i)
            if (_openRooms[i] == room.idx)
            {
                _openRooms[i] = _openRooms.back();
                _openRooms.pop_back();
                break;
            }
        FreeRoomIfDrained(room);
    }
    return died;
}

void RoomManager::FreeRoomIfDrained(Room& room)
{
    // _lock 을 잡은 채로만 불린다. reservedSeats==0 은 ProcessRoomQueues 가 보장.
    ++room.gen;
    _freeRooms.push_back(room.idx);
    metrics::g.roomsActive.Add(-1);
}
