#!/bin/bash
# 停止 LocalAI + MySQL 本地服务
# 可覆盖的环境变量：MYSQL_HOME、APP_RUN_DIR
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MYSQL_HOME="${MYSQL_HOME:-$HOME/.local/mysql}"
RUN_DIR="${APP_RUN_DIR:-$HOME/.local/run}"
MYSQL_ADMIN="$MYSQL_HOME/bin/mysqladmin"
SOCK="${DB_SOCKET:-$RUN_DIR/mysql.sock}"
MYSQL_PW_FILE="$MYSQL_HOME/.root_pw"
LOCALAI_PID="$RUN_DIR/localai.pid"
MYSQL_PID="$RUN_DIR/mysqld.pid"
LOG_ROTATE_PID="$RUN_DIR/log_rotate.pid"

# 停止日志轮转守护
if [ -f "$LOG_ROTATE_PID" ] && kill -0 "$(cat "$LOG_ROTATE_PID")" 2>/dev/null; then
  kill "$(cat "$LOG_ROTATE_PID")" 2>/dev/null
  sleep 1
  if kill -0 "$(cat "$LOG_ROTATE_PID")" 2>/dev/null; then
    kill -9 "$(cat "$LOG_ROTATE_PID")" 2>/dev/null
  fi
  echo "[log-rotate] stopped"
else
  echo "[log-rotate] not running"
fi
rm -f "$LOG_ROTATE_PID"

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
