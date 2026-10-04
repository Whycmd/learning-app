#!/bin/bash
# 编译 C++ 后端
# 用法：bash scripts/build_backend.sh
# 说明：新增 .cpp 源文件后无需修改本脚本（使用 src/*.cpp 通配）
set -e

APP_DIR="/Users/junwang/Desktop/王浩源/英语/英语学习应用"
BACKEND_DIR="$APP_DIR/backend"
MYSQL_HOME="/Users/junwang/.local/mysql"

cd "$BACKEND_DIR" || exit 1
mkdir -p build

clang++ -std=c++17 -O2 -Wall -Wno-unused-parameter \
  -Iinclude -I"$MYSQL_HOME/include" \
  src/*.cpp -o build/eng_app_server \
  -L"$MYSQL_HOME/lib" -lmysqlclient \
  -Wl,-rpath,"$MYSQL_HOME/lib"

echo "[build] OK -> $BACKEND_DIR/build/eng_app_server"