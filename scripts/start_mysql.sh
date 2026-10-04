#!/bin/bash
# 启动 MySQL 8.0 本地服务（仅 127.0.0.1）
set -e
MYSQL_HOME="/Users/junwang/.local/mysql"
SOCK="/Users/junwang/.local/run/mysql.sock"
PID_FILE="/Users/junwang/.local/run/mysqld.pid"

mkdir -p /Users/junwang/.local/run /Users/junwang/.local/log /Users/junwang/.local/mysql/data

if [ -S "$SOCK" ] && "$MYSQL_HOME/bin/mysqladmin" --socket="$SOCK" -uroot -p"$(cat $MYSQL_HOME/.root_pw)" ping >/dev/null 2>&1; then
  echo "[mysql] already running (socket=$SOCK)"
  exit 0
fi

if [ -f "$PID_FILE" ] && kill -0 "$(cat $PID_FILE)" 2>/dev/null; then
  echo "[mysql] already running (pid=$(cat $PID_FILE))"
  exit 0
fi

nohup "$MYSQL_HOME/bin/mysqld" \
  --user=junwang \
  --datadir="$MYSQL_HOME/data" \
  --basedir="$MYSQL_HOME" \
  --socket="$SOCK" \
  --port=3306 \
  --bind-address=127.0.0.1 \
  --pid-file="$PID_FILE" \
  --log-error="/Users/junwang/.local/log/mysqld.err" \
  --lower_case_table_names=2 \
  --default-authentication-plugin=mysql_native_password \
  --character-set-server=utf8mb4 \
  --collation-server=utf8mb4_unicode_ci \
  --innodb_buffer_pool_size=128M \
  --max_connections=50 \
  > /Users/junwang/.local/log/mysqld.out 2>&1 &

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
