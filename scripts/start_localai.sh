#!/bin/bash
# 启动本地 AI 网关（转发到智谱AI BigModel 在线 API）
# 架构：C++ 后端 → 本网关 (127.0.0.1:5340) → 智谱AI HTTPS API
# 网关负责 HTTP→HTTPS 转换、注入 API Key
# 可覆盖的环境变量：NODE_BIN、APP_RUN_DIR、APP_LOG_DIR
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
NODE_BIN="${NODE_BIN:-node}"
RUN_DIR="${APP_RUN_DIR:-$HOME/.local/run}"
LOG_DIR="${APP_LOG_DIR:-$HOME/.local/log}"
PID_FILE="$RUN_DIR/localai.pid"
HOST="127.0.0.1"
PORT=5340

mkdir -p "$RUN_DIR" "$LOG_DIR"

if curl -fsS --max-time 2 "http://$HOST:$PORT/readyz" >/dev/null 2>&1; then
  echo "[ai-gateway] already running at http://$HOST:$PORT"
  exit 0
fi

if [ -f "$PID_FILE" ] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
  echo "[ai-gateway] already running (pid=$(cat "$PID_FILE"))"
  exit 0
fi

export LOCALAI_HOST="$HOST"
export LOCALAI_PORT="$PORT"
export LOCALAI_LOG_DIR="$LOG_DIR"

nohup "$NODE_BIN" "$APP_DIR/scripts/local_ai_server.js" \
  >> "$LOG_DIR/localai.out" 2>&1 &

echo $! > "$PID_FILE"
echo "[ai-gateway] starting pid=$(cat "$PID_FILE") at http://$HOST:$PORT"

for i in $(seq 1 15); do
  if curl -fsS --max-time 2 "http://$HOST:$PORT/readyz" >/dev/null 2>&1; then
    echo "[ai-gateway] ready at http://$HOST:$PORT"
    curl -fsS --max-time 3 "http://$HOST:$PORT/readyz" 2>&1
    exit 0
  fi
  sleep 1
done
echo "[ai-gateway] ERROR: not ready after 15s" >&2
tail -20 "$LOG_DIR/localai.out" >&2
exit 1
