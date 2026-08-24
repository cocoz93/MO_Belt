#include "SelfTest.h"

#include <cstdio>
#include <cstring>
#include <thread>

// gcc 의 -Warray-bounds 는 링버퍼의 All-or-Nothing 가드(공간 부족이면 memcpy 전에 반환)를
// 증명하지 못해, 경계를 일부러 찌르는 이 테스트 파일에서만 오탐을 낸다(-O1/-O2 가 자리만 다름).
// 테스트 번역단위 한정으로 끈다 — 실코드에는 적용하지 않는다.
// (-Wstringop-overflow 도 같은 뿌리 — fortify 가 가드 앞 memcpy 를 실행되는 걸로 본다)
#pragma GCC diagnostic ignored "-Warray-bounds"
#pragma GCC diagnostic ignored "-Wstringop-overflow"

#include "RingBuffer.h"

// 검사 하나 = CHECK 한 줄. 실패해도 계속 돌아 전체 그림을 보여 준다.
static int s_failed = 0;
static int s_total  = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++s_total;                                                         \
        if (!(cond)) {                                                     \
            ++s_failed;                                                    \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                  \
    } while (0)

namespace {

// ── ST 링: 기본 계약 ─────────────────────────────────────────────
void TestRingBasic()
{
    CRingBufferST r;
    CHECK(r.Init(16));
    CHECK(r.IsValid());
    CHECK(r.GetFreeSize() == 15);          // 1칸 예약형 — 실사용 capacity-1

    char big[20] = {};
    CHECK(r.Enqueue(big, 20) == 0);        // All-or-Nothing: 부족하면 통째 실패
    CHECK(r.GetDataSize() == 0);

    const char* msg = "hello";
    CHECK(r.Enqueue(msg, 5) == 5);
    CHECK(r.GetDataSize() == 5);

    char out[8] = {};
    CHECK(r.Peek(out, 5) == 5);            // Peek 은 소비하지 않는다
    CHECK(std::memcmp(out, "hello", 5) == 0);
    CHECK(r.GetDataSize() == 5);

    std::memset(out, 0, sizeof(out));
    CHECK(r.Dequeue(out, 5) == 5);
    CHECK(std::memcmp(out, "hello", 5) == 0);
    CHECK(r.GetDataSize() == 0);
    CHECK(r.Dequeue(out, 1) == 0);         // 빈 링에서 All-or-Nothing 실패
}

// ── ST 링: 랩어라운드 데이터 무결성 ──────────────────────────────
void TestRingWrap()
{
    CRingBufferST r;
    CHECK(r.Init(16));

    char a[10];
    for (int i = 0; i < 10; ++i) a[i] = static_cast<char>(i);
    CHECK(r.Enqueue(a, 10) == 10);
    CHECK(r.Consume(6) == 6);              // read=6, write=10

    char b[9];
    for (int i = 0; i < 9; ++i) b[i] = static_cast<char>(100 + i);
    CHECK(r.Enqueue(b, 9) == 9);           // 6바이트 직선 + 3바이트 랩

    char out[13] = {};
    CHECK(r.Dequeue(out, 13) == 13);       // a[6..9] + b[0..8]
    for (int i = 0; i < 4; ++i) CHECK(out[i] == static_cast<char>(6 + i));
    for (int i = 0; i < 9; ++i) CHECK(out[4 + i] == static_cast<char>(100 + i));
}

// ── ST 링: Init 재호출 (Belt 에서 추가한 방어) ───────────────────
void TestRingReinit()
{
    CRingBufferST r;
    CHECK(r.Init(16));
    const char* msg = "abcd";
    CHECK(r.Enqueue(msg, 4) == 4);

    CHECK(r.Init(32));                     // 재호출 — 이전 버퍼 해제 + 위치 리셋
    CHECK(r.GetFreeSize() == 31);
    CHECK(r.GetDataSize() == 0);
    CHECK(r.Enqueue(msg, 4) == 4);
    char out[4] = {};
    CHECK(r.Dequeue(out, 4) == 4);
    CHECK(std::memcmp(out, "abcd", 4) == 0);
}

// ── MT 링: 두 스레드 생산/소비 (tsan 프리셋에서 돌릴 값어치) ─────
void TestRingMt()
{
    CRingBufferMT ring(1024);
    constexpr int kMsgs = 20000;

    std::thread producer([&] {
        for (int i = 0; i < kMsgs;)
        {
            int payload = i;
            if (ring.Enqueue(&payload, sizeof(payload)) == sizeof(payload))
                ++i;
            else
                std::this_thread::yield();  // 가득 — 소비를 기다린다
        }
    });

    long long sum = 0;
    for (int got = 0; got < kMsgs;)
    {
        int payload = 0;
        if (ring.Dequeue(&payload, sizeof(payload)) == sizeof(payload))
        {
            sum += payload;
            ++got;
        }
        else
        {
            std::this_thread::yield();
        }
    }
    producer.join();

    const long long expect = static_cast<long long>(kMsgs - 1) * kMsgs / 2;
    CHECK(sum == expect);
    CHECK(ring.GetDataSize() == 0);
}

} // namespace

int RunSelfTest()
{
    TestRingBasic();
    TestRingWrap();
    TestRingReinit();
    TestRingMt();

    std::printf("selftest: %d checks, %d failed\n", s_total, s_failed);
    return s_failed == 0 ? 0 : 1;
}
