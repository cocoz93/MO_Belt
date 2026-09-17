cmake_minimum_required(VERSION 3.25)
project(belt_server CXX)

# MMO 관례 계승: C++17 고정, 확장 금지
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MMO 에 없어서 새로 세운 것: 경고 전부 켠다
add_compile_options(-Wall -Wextra)
# MMO 에서 계승: backtrace 가 함수명을 찍게
add_link_options(-rdynamic)

# 주의: sim/ (결정론 시뮬) 번역단위에는 -ffast-math 류를 절대 붙이지 않는다.
# 붙는 순간 double 비트가 갈려 클라(logic.js)와의 해시 대조가 깨진다.

# 서버 토대는 사본을 두지 않고 ServerCore 서브모듈에서 본다 (github.com/cocoz93/ServerCore).
#   전에는 RingBuffer.h 를 복사해 뒀는데, 같은 수정을 세 저장소에 손으로 넣다가
#   조용히 갈라졌다 — 정본에 들어간 안전장치 5건이 이쪽엔 없었다.
#   ※ 헤더만 쓰므로 add_subdirectory 가 아니라 include 경로만 연다.
#     ServerCore 를 통째로 빌드하면 LockFree 까지 필요해지는데 여기선 안 쓴다.
set(SERVERCORE_BASE ${CMAKE_CURRENT_SOURCE_DIR}/../ServerCore/Base)

if(NOT EXISTS "${SERVERCORE_BASE}/RingBuffer.h")
    message(FATAL_ERROR
        "ServerCore 서브모듈이 비어 있습니다: ${SERVERCORE_BASE}\n"
        "  git submodule update --init")
endif()

add_executable(belt_server
    src/main.cpp
    src/EpollWorker.cpp
    src/GameWorker.cpp
    src/Metrics.cpp
    src/RoomManager.cpp
    src/SelfTest.cpp
    src/Sim.cpp
)
target_include_directories(belt_server PRIVATE src ${SERVERCORE_BASE})

find_package(Threads REQUIRED)
target_link_libraries(belt_server PRIVATE Threads::Threads)

# 부하 생성기 — 수치의 정본(설계 §12). WSL 내부에서 서버에 TCP 직결로 붙는다.
add_executable(belt_dummy
    ../dummy/dummy_main.cpp
    src/Metrics.cpp
)
target_include_directories(belt_dummy PRIVATE src ${SERVERCORE_BASE})
target_link_libraries(belt_dummy PRIVATE Threads::Threads)
