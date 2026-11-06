#!/bin/bash
# 编译 C++ 后端
# 用法：bash scripts/build_backend.sh
# 说明：新增 .cpp 源文件后无需修改本脚本（使用 src/*.cpp 通配）
# 可覆盖的环境变量：MYSQL_HOME（MySQL 安装目录）、CXX（指定编译器）
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BACKEND_DIR="$APP_DIR/backend"
MYSQL_HOME="${MYSQL_HOME:-$HOME/.local/mysql}"

# 编译器自适应：CXX 优先，其次 c++ / clang++ / g++
CXX_BIN="${CXX:-}"
if [ -z "$CXX_BIN" ]; then
  for c in c++ clang++ g++; do
    if command -v "$c" >/dev/null 2>&1; then CXX_BIN="$c"; break; fi
  done
fi
[ -n "$CXX_BIN" ] || { echo "[build] ERROR: no C++ compiler found" >&2; exit 1; }

# MySQL 头文件与库：优先本地便携版 MYSQL_HOME，其次 mysql_config
if [ -d "$MYSQL_HOME/include" ] && [ -d "$MYSQL_HOME/lib" ]; then
  MYSQL_INC="-I$MYSQL_HOME/include"
  MYSQL_LIB="-L$MYSQL_HOME/lib -lmysqlclient -Wl,-rpath,$MYSQL_HOME/lib"
elif command -v mysql_config >/dev/null 2>&1; then
  MYSQL_INC="$(mysql_config --include)"
  MYSQL_LIB="$(mysql_config --libs)"
else
  echo "[build] ERROR: libmysqlclient not found (set MYSQL_HOME or install mysql_config)" >&2
  exit 1
fi

cd "$BACKEND_DIR" || exit 1
mkdir -p build

$CXX_BIN -std=c++17 -O2 -Wall -Wno-unused-parameter \
  -Iinclude $MYSQL_INC \
  src/*.cpp -o build/eng_app_server \
  $MYSQL_LIB

echo "[build] OK -> $BACKEND_DIR/build/eng_app_server"
