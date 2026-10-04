#!/bin/bash
# 英语学习应用 - 一键停止全部服务
APP_DIR="/Users/junwang/Desktop/王浩源/英语/英语学习应用"
BACKEND_PID_FILE="/Users/junwang/.local/run/eng_app.pid"
LOCALAI_PID="/Users/junwang/.local/run/localai.pid"
MYSQL_PID="/Users/junwang/.local/run/mysqld.pid"
SOCK="/Users/junwang/.local/run/mysql.sock"
MYSQL_ADMIN="/Users/junwang/.local/mysql/bin/mysqladmin"
MYSQL_PW_FILE="/Users/junwang/.local/mysql/.root_pw"

echo "========================================"
echo "  停止全部服务"
echo "========================================"

# 停止 C++ 后端
if [ -f "$BACKEND_PID_FILE" ] && kill -0 "$(cat $BACKEND_PID_FILE)" 2>/dev/null; then
  kill "$(cat $BACKEND_PID_FILE)" 2>/dev/null
  sleep 2
  kill -0 "$(cat $BACKEND_PID_FILE)" 2>/dev/null && kill -9 "$(cat $BACKEND_PID_FILE)" 2>/dev/null
  rm -f "$BACKEND_PID_FILE"
  echo "[backend] stopped"
else
  echo "[backend] not running"
fi

# 停止 AI 服务
if [ -f "$LOCALAI_PID" ] && kill -0 "$(cat $LOCALAI_PID)" 2>/dev/null; then
  kill "$(cat $LOCALAI_PID)" 2>/dev/null
  sleep 3
  kill -0 "$(cat $LOCALAI_PID)" 2>/dev/null && kill -9 "$(cat $LOCALAI_PID)" 2>/dev/null
  rm -f "$LOCALAI_PID"
  echo "[localai] stopped"
else
  echo "[localai] not running"
fi

# 停止 MySQL
if [ -S "$SOCK" ]; then
  "$MYSQL_ADMIN" --socket="$SOCK" -uroot -p"$(cat $MYSQL_PW_FILE)" shutdown 2>&1
  echo "[mysql] shutdown sent"
  for i in $(seq 1 15); do [ -S "$SOCK" ] || break; sleep 1; done
else
  echo "[mysql] socket not found"
  if [ -f "$MYSQL_PID" ] && kill -0 "$(cat $MYSQL_PID)" 2>/dev/null; then
    kill "$(cat $MYSQL_PID)" 2>/dev/null; sleep 3
    kill -9 "$(cat $MYSQL_PID)" 2>/dev/null
  fi
fi
rm -f "$SOCK" "$MYSQL_PID"
echo "[mysql] stopped"

echo ""
echo "========================================"
echo "  全部服务已停止"
echo "========================================"
sleep 2
