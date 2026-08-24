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

add_executable(belt_server
    src/main.cpp
    src/EpollWorker.cpp
    src/GameWorker.cpp
    src/Metrics.cpp
    src/SelfTest.cpp
)
target_include_directories(belt_server PRIVATE src)

find_package(Threads REQUIRED)
target_link_libraries(belt_server PRIVATE Threads::Threads)
