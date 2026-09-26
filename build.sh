#!/bin/bash
#
# 构建 NovaServer
#
# 等价于：
#   cmake -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#
# 产物位于 build/server，需在项目根目录下运行。
#
set -e

cd "$(dirname "$0")"

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
