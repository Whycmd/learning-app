#!/bin/bash
# 英语学习应用 - 一键启动
# 双击此文件即可启动全部服务并打开浏览器
APP_DIR="/Users/junwang/Desktop/王浩源/英语/英语学习应用"
LOG_DIR="/Users/junwang/.local/log"
BACKEND_PORT=8088

cd "$APP_DIR" || exit 1
mkdir -p "$LOG_DIR" /Users/junwang/.local/run

echo "========================================"
echo "  英语学习应用 - 一键启动"
echo "========================================"
echo ""

# 1. 启动 MySQL
echo "[1/4] 启动 MySQL 数据库..."
bash "$APP_DIR/scripts/start_mysql.sh" || { echo "MySQL 启动失败，请查看日志: $LOG_DIR/mysqld.err"; read -n1 -r -p "按任意键退出..."; exit 1; }
echo ""

# 2. 启动 AI 网关（转发到智谱AI BigModel 在线 API）
echo "[2/4] 启动 AI 网关（智谱AI GLM-4-Flash）..."
bash "$APP_DIR/scripts/start_localai.sh" || { echo "AI 服务启动失败，请查看日志: $LOG_DIR/localai.out"; read -n1 -r -p "按任意键退出..."; exit 1; }
echo ""

# 3. 启动 C++ 后端
echo "[3/4] 启动后端服务..."
BACKEND_PID_FILE="/Users/junwang/.local/run/eng_app.pid"
if curl -fsS --max-time 2 "http://127.0.0.1:$BACKEND_PORT/api/health" >/dev/null 2>&1; then
  echo "[backend] already running at http://127.0.0.1:$BACKEND_PORT"
else
  nohup "$APP_DIR/backend/build/eng_app_server" \
    --root "$APP_DIR" \
    --port "$BACKEND_PORT" \
    --host "127.0.0.1" \
    > "$LOG_DIR/eng_app.out" 2>&1 &
  echo $! > "$BACKEND_PID_FILE"
  echo "[backend] starting pid=$(cat $BACKEND_PID_FILE) at http://127.0.0.1:$BACKEND_PORT"
  for i in $(seq 1 30); do
    if curl -fsS --max-time 2 "http://127.0.0.1:$BACKEND_PORT/api/health" >/dev/null 2>&1; then
      echo "[backend] ready"
      break
    fi
    sleep 1
    if [ $i -eq 30 ]; then
      echo "[backend] ERROR: not ready after 30s"
      tail -20 "$LOG_DIR/eng_app.out"
      read -n1 -r -p "按任意键退出..."
      exit 1
    fi
  done
fi
echo ""

# 4. 打开浏览器
echo "[4/4] 打开浏览器..."
sleep 1
open "http://127.0.0.1:$BACKEND_PORT/"

echo ""
echo "========================================"
echo "  启动完成！"
echo "  访问地址: http://127.0.0.1:$BACKEND_PORT/"
echo "  如需关闭服务，运行: $APP_DIR/scripts/stop_all.command"
echo "========================================"
echo ""
echo "此终端窗口保持运行可监控后端日志。"
echo "关闭窗口不影响服务运行；如需停止服务请使用 stop_all.command。"
echo ""
tail -f "$LOG_DIR/eng_app.out" 2>/dev/null
