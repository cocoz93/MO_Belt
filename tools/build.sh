#!/bin/sh
# 사용: tools/build.sh <release|debug|tsan>   (WSL 안에서 실행)
# cmake 프리셋으로 configure + build 를 한 번에 한다.
set -e
preset="${1:-release}"
cd "$(dirname "$0")/../server"
cmake --preset "$preset"
cmake --build --preset "$preset"
