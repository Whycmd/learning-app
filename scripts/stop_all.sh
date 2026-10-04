#!/bin/bash
# 停止 LocalAI + MySQL 本地服务
MYSQL_ADMIN="/Users/junwang/.local/mysql/bin/mysqladmin"
SOCK="/Users/junwang/.local/run/mysql.sock"
MYSQL_PW_FILE="/Users/junwang/.local/mysql/.root_pw"
LOCALAI_PID="/Users/junwang/.local/run/localai.pid"
MYSQL_PID="/Users/junwang/.local/run/mysqld.pid"

# 停止 LocalAI
if [ -f "$LOCALAI_PID" ] && kill -0 "$(cat $LOCALAI_PID)" 2>/dev/null; then
  kill "$(cat $LOCALAI_PID)" 2>/dev/null
  sleep 3
  if kill -0 "$(cat $LOCALAI_PID)" 2>/dev/null; then
    kill -9 "$(cat $LOCALAI_PID)" 2>/dev/null
  fi
  rm -f "$LOCALAI_PID"
  echo "[localai] stopped"
else
  echo "[localai] not running"
fi

# 停止 MySQL（优雅 shutdown）
if [ -S "$SOCK" ]; then
  "$MYSQL_ADMIN" --socket="$SOCK" -uroot -p"$(cat $MYSQL_PW_FILE)" shutdown 2>&1
  echo "[mysql] shutdown sent"
  for i in $(seq 1 15); do
    [ -S "$SOCK" ] || break
    sleep 1
  done
else
  echo "[mysql] socket not found, killing pid"
  if [ -f "$MYSQL_PID" ] && kill -0 "$(cat $MYSQL_PID)" 2>/dev/null; then
    kill "$(cat $MYSQL_PID)" 2>/dev/null
    sleep 3
    kill -9 "$(cat $MYSQL_PID)" 2>/dev/null
  fi
fi
rm -f "$SOCK" "$MYSQL_PID"
echo "[stop] all done"
