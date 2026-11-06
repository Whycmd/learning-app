#!/bin/bash
# 启动 MySQL 8.0 本地服务（仅 127.0.0.1）
# 可覆盖的环境变量：MYSQL_HOME、APP_RUN_DIR、APP_LOG_DIR、DB_SOCKET
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
MYSQL_HOME="${MYSQL_HOME:-$HOME/.local/mysql}"
RUN_DIR="${APP_RUN_DIR:-$HOME/.local/run}"
LOG_DIR="${APP_LOG_DIR:-$HOME/.local/log}"
SOCK="${DB_SOCKET:-$RUN_DIR/mysql.sock}"
PID_FILE="$RUN_DIR/mysqld.pid"

mkdir -p "$RUN_DIR" "$LOG_DIR" "$MYSQL_HOME/data"

if [ -S "$SOCK" ] && "$MYSQL_HOME/bin/mysqladmin" --socket="$SOCK" -uroot -p"$(cat "$MYSQL_HOME/.root_pw")" ping >/dev/null 2>&1; then
  echo "[mysql] already running (socket=$SOCK)"
  exit 0
fi

if [ -f "$PID_FILE" ] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
  echo "[mysql] already running (pid=$(cat "$PID_FILE"))"
  exit 0
fi

nohup "$MYSQL_HOME/bin/mysqld" \
  --user="$(whoami)" \
  --datadir="$MYSQL_HOME/data" \
  --basedir="$MYSQL_HOME" \
  --socket="$SOCK" \
  --port=3306 \
  --bind-address=127.0.0.1 \
  --pid-file="$PID_FILE" \
  --log-error="$LOG_DIR/mysqld.err" \
  --lower_case_table_names=2 \
  --default-authentication-plugin=mysql_native_password \
  --character-set-server=utf8mb4 \
  --collation-server=utf8mb4_unicode_ci \
  --innodb_buffer_pool_size=128M \
  --max_connections=50 \
  > "$LOG_DIR/mysqld.out" 2>&1 &

echo "[mysql] starting pid=$!"

for i in $(seq 1 30); do
  if [ -S "$SOCK" ]; then
    echo "[mysql] ready at $SOCK"
    exit 0
  fi
  sleep 1
done
echo "[mysql] ERROR: socket not ready after 30s" >&2
exit 1
