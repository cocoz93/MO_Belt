#pragma once
// 와이어 규약 — MMO 관례 유지: 리틀엔디안, size = 헤더 포함 전체 크기.
// enum 값은 손으로 박는다 — 암묵 증가는 클라(proto.js) 하드코딩과 어긋난 전례가 있다.
#include <cstdint>

enum class MsgType : uint16_t
{
    ECHO           = 1,      // 페이로드 그대로 반환 (전송 계층 검증용)

    C2S_JOIN       = 10,     // {protocolVer u16}                          — U2.2에서 처리
    C2S_INPUT      = 11,     // {seq u32, mx i8, my i8, attack u8, skill u8} — U2.3에서 처리
    C2S_PING       = 12,     // {clientTimeUs i64}

    S2C_JOIN_OK    = 100,    // {actorId u8, roomId u32, serverTick u32}
    S2C_JOIN_FAIL  = 101,    // {reason u8}
    S2C_SNAPSHOT   = 102,    // U2.3에서 정의
    S2C_PONG       = 103,    // {clientTimeUs i64, serverTick u32, tickRemainUs u32}
    S2C_ROOM_EVENT = 104,    // {kind u8, actorId u8, roomId u32, spawnQX u16, spawnQY u16}
};

// 와이어 호환 확인용 — JOIN 에 실려 오고, 다르면 즉시 JOIN_FAIL (빌드 불일치 fail-fast)
constexpr uint16_t kProtocolVersion = 1;

constexpr int kRoomMembersWire = 4;    // 방 정원 — Room.h 의 kRoomMembers 와 static_assert 로 짝 맞춤

// 좌표 양자화 — 1/32px. WORLD_W 1600×32 = 51,200 < 65,535 라 u16 에 든다.
// +0.5 반올림은 계약(절삭이면 왕복마다 좌표가 한쪽으로 민다 — MMO 교훈).
constexpr double kPosQuantScale = 32.0;
inline uint16_t QuantPos(double v)
{
    if (v < 0) v = 0;
    return static_cast<uint16_t>(v * kPosQuantScale + 0.5);
}

enum class JoinFailReason : uint8_t
{
    VersionMismatch = 1,
    NoCapacity      = 2,
    AlreadyJoined   = 3,
};

#pragma pack(push, 1)

struct MsgHeader
{
    uint16_t size;     // 패킷 전체 크기 (헤더 포함)
    uint16_t type;     // MsgType 값
};

struct MSG_C2S_JOIN
{
    MsgHeader header;
    uint16_t  protocolVer;
};

// 클라 자기 식별 + 시계 맞추기 시드 (설계 §6)
struct MSG_S2C_JOIN_OK
{
    MsgHeader header;
    uint8_t   actorId;       // 스냅샷 속 "내 것" — 슬롯 0~3
    uint32_t  roomId;
    uint32_t  serverTick;    // 게임 워커 틱 반영은 U2.3
};

struct MSG_S2C_JOIN_FAIL
{
    MsgHeader header;
    uint8_t   reason;        // JoinFailReason
};

// 입력 = 유지 상태 + seq(=클라 틱, 단일 카운터). 서버는 플레이어별 링에 seq순 적재 후
// 틱당 정확히 1개 소비한다 — 재적용 불변식(플랜 「확정 아키텍처」)의 와이어 쪽 절반.
struct MSG_C2S_INPUT
{
    MsgHeader header;
    uint32_t  seq;
    int8_t    mx;        // -1|0|1
    int8_t    my;
    uint8_t   attack;    // 0|1
    uint8_t   skill;
};

struct MSG_C2S_PING
{
    MsgHeader header;
    int64_t   clientTimeUs;
};

// 스냅샷 액터 한 칸 — 좌표는 1/32px 눈금(+0.5 반올림 계약)
struct SnapActor
{
    uint8_t  id;         // 슬롯 번호 = 액터 id (0~3 플레이어, 4~13 오크)
    uint8_t  kind;       // 0 플레이어 / 1 오크
    uint8_t  state;      // 255 = 빈 슬롯(플레이어 미접속)
    int8_t   face;
    uint16_t qx;
    uint16_t qy;
    int16_t  hp;
};

constexpr int kSnapActors = 14;

// 전체 상태 스냅샷(20Hz) — 델타 없음(설계 §7). lastInputSeq 는 슬롯 4개를 전부 실어
// 수신자가 자기 것만 골라 읽는다 → 방 전체가 같은 바이트라 방당 1회 직렬화로 끝난다.
struct MSG_S2C_SNAPSHOT
{
    MsgHeader header;
    uint32_t  serverTick;
    uint8_t   actorCount;              // = kSnapActors
    uint32_t  lastInputSeq[kRoomMembersWire];
    SnapActor actors[kSnapActors];
};

// 시계 맞추기(설계 §6): 클라 에코 + 서버 틱 + 다음 틱까지 잔여 µs.
// PONG 은 소유 워커가 recv 자리에서 즉답한다 — 게임 워커를 거치면 틱만큼 RTT 가 오염된다.
struct MSG_S2C_PONG
{
    MsgHeader header;
    int64_t   clientTimeUs;    // 받은 값 그대로 돌려준다
    uint32_t  serverTick;      // 게임 워커가 생기기 전(U2.1)까지는 0
    uint32_t  tickRemainUs;    // 〃
};

#pragma pack(pop)

// 파서 하드닝 상한 — 이 밖이면 즉시 절단. 스냅샷이 정의되면(U2.3) 재산정한다.
constexpr size_t MAX_PACKET_SIZE = 1024;

static_assert(sizeof(MsgHeader) == 4,        "MsgHeader 는 4바이트 고정");
static_assert(sizeof(MSG_C2S_JOIN) == 6,     "JOIN 레이아웃");
static_assert(sizeof(MSG_S2C_JOIN_OK) == 13, "JOIN_OK 레이아웃");
static_assert(sizeof(MSG_S2C_JOIN_FAIL) == 5, "JOIN_FAIL 레이아웃");
static_assert(sizeof(MSG_C2S_INPUT) == 12,   "INPUT 레이아웃");
static_assert(sizeof(MSG_C2S_PING) == 12,    "PING 레이아웃");
static_assert(sizeof(MSG_S2C_PONG) == 20,    "PONG 레이아웃");
static_assert(sizeof(SnapActor) == 10,       "SnapActor 레이아웃");
static_assert(sizeof(MSG_S2C_SNAPSHOT) == 4 + 4 + 1 + 16 + 140, "SNAPSHOT 레이아웃(165B)");
