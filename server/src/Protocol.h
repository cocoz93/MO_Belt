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

#pragma pack(push, 1)

struct MsgHeader
{
    uint16_t size;     // 패킷 전체 크기 (헤더 포함)
    uint16_t type;     // MsgType 값
};

struct MSG_C2S_PING
{
    MsgHeader header;
    int64_t   clientTimeUs;
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

static_assert(sizeof(MsgHeader) == 4,      "MsgHeader 는 4바이트 고정");
static_assert(sizeof(MSG_C2S_PING) == 12,  "PING 레이아웃");
static_assert(sizeof(MSG_S2C_PONG) == 20,  "PONG 레이아웃");
