#include "Room.h"

#include <ctime>

#include "Constants.h"
#include "GameWorker.h"
#include "Metrics.h"
#include "Sim.h"

namespace {

int64_t MgrNowNs()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

void ResetWorld(SimWorld& w)
{
    w.tick = 0;
    for (int i = 0; i < kSnapActors; ++i)
        w.actors[i] = SimActor{};
    // 오크 10 — 설계 §9 스폰표. AI 없이 정지(U3.1 전), separate 참여는 U3.1 부터.
    for (int i = 0; i < 10; ++i)
    {
        SimActor& a = w.actors[kRoomMembers + i];
        a.present = true;
        a.kind    = 1;
        a.x       = kOrcSpawn[i][0];
        a.y       = kOrcSpawn[i][1];
        a.face    = -1;
        a.hp      = 3;
    }
}

} // namespace

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
    return AllocRoomSlotLocked(0, /*useMailbox=*/true);
}

Room* RoomManager::AllocRoomSlotLocked(uint8_t forceWorker, bool useMailbox)
{
    // _lock 을 잡은 채로만 불린다. useMailbox=false 면 forceWorker 에 직접 붙인다
    // (방 이동 — 호출자가 그 워커 자신이라 우편함이 필요 없다).
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
        r.members[i] = Room::Member{};
    r.memberCount = 0;
    {
        std::lock_guard<std::mutex> il(r.inLock);
        r.inQ.clear();
    }
    ResetWorld(r.world);
    r.tickAtomic.store(0, std::memory_order_relaxed);

    unsigned best = forceWorker;
    if (useMailbox)
    {
        // 가장 한가한 워커에 붙인다 — roomId % K 금지 (설계 §5)
        best = 0;
        for (unsigned w = 1; w < _workerCount; ++w)
            if (_roomsPerWorker[w] < _roomsPerWorker[best])
                best = w;
    }
    r.gameWorker = static_cast<uint8_t>(best);
    ++_roomsPerWorker[best];

    if (useMailbox)
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
    s.roomIdx.store(room->idx, std::memory_order_relaxed);
    s.actorId = static_cast<uint8_t>(seat);

    outActorId = static_cast<uint8_t>(seat);
    outRoomId  = room->idx;
    metrics::g.joins.Inc();
    return true;
}

void RoomManager::OnSessionLeave(Session& s)
{
    const uint32_t ridx = s.roomIdx.load(std::memory_order_relaxed);
    if (!s.inRoom || ridx >= _cap)
        return;
    s.inRoom = false;

    Room& room = _slots[ridx];
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
        m = Room::Member{};
        m.used      = true;
        m.sessIdx   = e.sessIdx;
        m.sessGen   = e.sessGen;
        m.sessOwner = s.owner;
        ++room.memberCount;

        // 플레이어 액터 스폰 — 설계 §9 스폰표 (이동은 U3.1)
        SimActor& a = room.world.actors[e.actorId];
        a.present = true;
        a.kind    = 0;
        a.state   = 0;
        a.face    = 1;
        a.x       = kPlayerSpawn[e.actorId][0];
        a.y       = kPlayerSpawn[e.actorId][1];
        a.hp      = 10;
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
                room.world.actors[i].present = false;    // 스냅샷에 빈 슬롯으로 나간다
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

void RoomManager::PushInput(Session& s, uint32_t seq, int8_t mx, int8_t my,
                            uint8_t attack, uint8_t skill)
{
    const uint32_t ridx = s.roomIdx.load(std::memory_order_relaxed);
    if (!s.inRoom || ridx >= _cap)
        return;
    Room& room = _slots[ridx];
    {
        std::lock_guard<std::mutex> il(room.inLock);
        room.inQ.push_back({ s.idx, s.gen, s.actorId, seq, mx, my, attack, skill });
    }
    metrics::g.inputsQueued.Inc();
}

void RoomManager::GetPingInfo(Session& s, uint32_t& outTick, uint32_t& outRemainUs)
{
    outTick     = 0;
    outRemainUs = 0;
    const uint32_t ridx = s.roomIdx.load(std::memory_order_relaxed);
    if (!s.inRoom || ridx >= _cap)
        return;
    Room& room = _slots[ridx];
    outTick = room.tickAtomic.load(std::memory_order_relaxed);

    const int64_t next = GameWorkerNextTickNs(room.gameWorker);
    const int64_t now  = MgrNowNs();
    if (next > now)
        outRemainUs = static_cast<uint32_t>((next - now) / 1000);
}

Room* RoomManager::RoomTick(Room& room, DirtyMap& dirty)
{
    // ── 입력 라우팅: 방 큐 swap → 플레이어별 링 (seq 는 클라가 단조 증가로 보낸다) ──
    static thread_local std::vector<InputCmd> batch;    // 워커 스레드 전용 재사용 버퍼
    batch.clear();
    {
        std::lock_guard<std::mutex> il(room.inLock);
        batch.swap(room.inQ);
    }
    for (const InputCmd& c : batch)
    {
        if (c.actorId >= kRoomMembers)
            continue;
        Room::Member& m = room.members[c.actorId];
        if (!m.used || m.sessIdx != c.sessIdx || m.sessGen != c.sessGen)
            continue;      // 옛 세대·자리 바뀐 입력 — 버린다
        if (m.ringCount == kInputRingDepth)
        {
            // 가득 — oldest drop. 재적용 불일치가 생기는 지점이라 반드시 센다
            m.ringHead = (m.ringHead + 1) % kInputRingDepth;
            --m.ringCount;
            metrics::g.inputRingDrops.Inc();
        }
        m.ring[(m.ringHead + m.ringCount) % kInputRingDepth] = c;
        ++m.ringCount;
    }

    // ── 틱당 정확히 1개 소비 (불변식: 서버 적용 횟수 = 클라 재적용 제외 횟수) ──
    for (int i = 0; i < kRoomMembers; ++i)
    {
        Room::Member& m = room.members[i];
        if (!m.used)
            continue;
        metrics::g.inputRingOcc[m.ringCount <= 8 ? m.ringCount : 8].Inc();

        if (m.ringCount > 0)
        {
            m.held = m.ring[m.ringHead];
            m.ringHead = (m.ringHead + 1) % kInputRingDepth;
            --m.ringCount;
            m.lastInputSeq = m.held.seq;
            m.emptyStreak  = 0;
        }
        else if (++m.emptyStreak > 10)
        {
            // 입력이 10틱 넘게 끊겼다 — 중립 전환 (설계 §6: 끊긴 사람이 벽으로 걷는 것 방지)
            if (m.held.mx != 0 || m.held.my != 0 || m.held.attack != 0 || m.held.skill != 0)
                metrics::g.inputNeutralized.Inc();
            m.held.mx = 0; m.held.my = 0; m.held.attack = 0; m.held.skill = 0;
        }
        // 빈 틱(10틱 이내)은 m.held 유지 — lastInputSeq 불변
    }

    // ── step: 이동 + separate + 무대 경계 (logic.js 거울 — Sim.cpp) ──
    SimInput inputs[kRoomMembers]{};
    for (int i = 0; i < kRoomMembers; ++i)
    {
        const Room::Member& m = room.members[i];
        if (!m.used)
            continue;
        inputs[i].mx = m.held.mx;
        inputs[i].my = m.held.my;
    }
    SimStep(room.world, inputs);

    ++room.world.tick;
    room.tickAtomic.store(room.world.tick, std::memory_order_relaxed);

    // ── 20Hz(3틱마다) 전체 스냅샷 — 방 전체가 같은 바이트라 1회 직렬화로 끝 ──
    if (room.world.tick % 3 != 0 || room.memberCount == 0)
        return ExitCheckAndMaybeTransfer(room, dirty);

    MSG_S2C_SNAPSHOT snap{};
    snap.header.size = sizeof(snap);
    snap.header.type = static_cast<uint16_t>(MsgType::S2C_SNAPSHOT);
    snap.serverTick  = room.world.tick;
    snap.actorCount  = kSnapActors;
    for (int i = 0; i < kRoomMembers; ++i)
        snap.lastInputSeq[i] = room.members[i].used ? room.members[i].lastInputSeq : 0;
    for (int i = 0; i < kSnapActors; ++i)
    {
        const SimActor& a = room.world.actors[i];
        SnapActor& o = snap.actors[i];
        o.id   = static_cast<uint8_t>(i);
        o.kind = a.kind;
        if (!a.present)
        {
            o.state = 255;
            continue;
        }
        o.state = a.state;
        o.face  = a.face;
        o.qx    = QuantPos(a.x);
        o.qy    = QuantPos(a.y);
        o.hp    = a.hp;
    }

    for (int i = 0; i < kRoomMembers; ++i)
    {
        Room::Member& m = room.members[i];
        if (!m.used)
            continue;
        Session& s = _sessions->At(m.sessIdx);
        if (s.gen != m.sessGen || s.dead.load(std::memory_order_relaxed))
            continue;
        if (s.sendQ.Enqueue(&snap, sizeof(snap)) == sizeof(snap))
        {
            metrics::g.snapshotsSent.Inc();
            dirty.Mark(m.sessOwner, m.sessIdx);
        }
        else
        {
            // 가득 — 이 스냅샷은 버린다(전체 상태라 다음 것이 대체). 정밀 drop-oldest 는 후속.
            metrics::g.snapshotsDropped.Inc();
        }
    }

    return ExitCheckAndMaybeTransfer(room, dirty);
}

Room* RoomManager::ExitCheckAndMaybeTransfer(Room& room, DirtyMap& dirty)
{
    // 1차 이동 조건(설계 §12): 생존 플레이어 전원이 우측 출구 구역(x ≥ WORLD_W−80)
    if (room.memberCount == 0)
        return nullptr;
    for (int i = 0; i < kRoomMembers; ++i)
    {
        if (!room.members[i].used)
            continue;
        const SimActor& a = room.world.actors[i];
        if (!a.present || a.x < EXIT_ZONE_X)
            return nullptr;
    }
    return TransferRoom(room, dirty);
}

Room* RoomManager::TransferRoom(Room& oldRoom, DirtyMap& dirty)
{
    Room* nr = nullptr;
    {
        std::lock_guard<std::mutex> lk(_lock);

        nr = AllocRoomSlotLocked(oldRoom.gameWorker, /*useMailbox=*/false);
        if (nr == nullptr)
            return nullptr;      // 방 풀 고갈 — 이동 포기, 다음 틱 재시도

        // 옛 방 마감: 새 예약 차단 + 좌석 장부 이월 준비. 대기열도 통째로 옮긴다 —
        // 이월 없이 옛 방을 회수하면, 이동 순간 끊긴 세션의 좌석·참조가 새 방에서 샌다.
        std::vector<RoomJoinEntry>  carryJoin;
        std::vector<RoomLeaveEntry> carryLeave;
        {
            std::lock_guard<std::mutex> rl(oldRoom.lock);
            oldRoom.dead = true;
            carryJoin.swap(oldRoom.pendingJoin);
            carryLeave.swap(oldRoom.pendingLeave);
            for (int i = 0; i < kRoomMembers; ++i)
                oldRoom.seatUsed[i] = false;
            oldRoom.reservedSeats = 0;
        }

        // 멤버 이전 — 참조 이관(Release/AddRef 없음), 입력 링·held·lastSeq 연속 유지.
        // 플레이어는 스폰표 위치에서 다시 시작한다 (같은 슬롯 = 같은 actorId).
        int moved = 0;
        for (int i = 0; i < kRoomMembers; ++i)
        {
            Room::Member& om = oldRoom.members[i];
            if (!om.used)
                continue;
            nr->members[i] = om;
            om.used = false;
            ++moved;

            SimActor& a = nr->world.actors[i];
            a.present = true;
            a.kind    = 0;
            a.state   = 0;
            a.face    = 1;
            a.x       = kPlayerSpawn[i][0];
            a.y       = kPlayerSpawn[i][1];
            a.hp      = 10;

            Session& s = _sessions->At(nr->members[i].sessIdx);
            if (s.gen == nr->members[i].sessGen)
                s.roomIdx.store(nr->idx, std::memory_order_relaxed);
        }
        nr->world.tick = oldRoom.world.tick;    // 방 이동으로 시간이 되감기지 않게 이어 센다
        nr->tickAtomic.store(nr->world.tick, std::memory_order_relaxed);
        oldRoom.memberCount = 0;

        {
            std::lock_guard<std::mutex> rl(nr->lock);
            nr->everReserved = true;
            for (int i = 0; i < kRoomMembers; ++i)
                if (nr->members[i].used)
                {
                    nr->seatUsed[i] = true;
                    ++nr->reservedSeats;
                }
            // 대기열 이월: pendingJoin 은 좌석도 같이 이월 (JOIN_OK 는 이미 나갔다)
            for (const RoomJoinEntry& e : carryJoin)
            {
                if (!nr->seatUsed[e.actorId])
                {
                    nr->seatUsed[e.actorId] = true;
                    ++nr->reservedSeats;
                }
                nr->pendingJoin.push_back(e);
                Session& js = _sessions->At(e.sessIdx);
                if (js.gen == e.sessGen)
                    js.roomIdx.store(nr->idx, std::memory_order_relaxed);
            }
            for (const RoomLeaveEntry& e : carryLeave)
                nr->pendingLeave.push_back(e);
        }
        nr->memberCount = moved;

        // 열린 목록·워커 부하 장부 정리 후 옛 방 회수
        for (size_t i = 0; i < _openRooms.size(); ++i)
            if (_openRooms[i] == oldRoom.idx)
            {
                _openRooms[i] = _openRooms.back();
                _openRooms.pop_back();
                break;
            }
        if (nr->reservedSeats < kRoomMembers)
            _openRooms.push_back(nr->idx);
        --_roomsPerWorker[oldRoom.gameWorker];
        FreeRoomIfDrained(oldRoom);
        metrics::g.roomMoves.Inc();
    }

    // ROOM_EVENT(방이동) — 새 방 멤버에게 새 방 번호·재스폰 좌표를 알린다 (락 밖)
    for (int i = 0; i < kRoomMembers; ++i)
    {
        Room::Member& m = nr->members[i];
        if (!m.used)
            continue;
        Session& s = _sessions->At(m.sessIdx);
        if (s.gen != m.sessGen || s.dead.load(std::memory_order_relaxed))
            continue;

        MSG_S2C_ROOM_EVENT ev{};
        ev.header.size = sizeof(ev);
        ev.header.type = static_cast<uint16_t>(MsgType::S2C_ROOM_EVENT);
        ev.kind        = static_cast<uint8_t>(RoomEventKind::RoomMove);
        ev.actorId     = static_cast<uint8_t>(i);
        ev.roomId      = nr->idx;
        ev.spawnQX     = QuantPos(kPlayerSpawn[i][0]);
        ev.spawnQY     = QuantPos(kPlayerSpawn[i][1]);
        if (s.sendQ.Enqueue(&ev, sizeof(ev)) == sizeof(ev))
            dirty.Mark(m.sessOwner, m.sessIdx);
    }
    return nr;
}
