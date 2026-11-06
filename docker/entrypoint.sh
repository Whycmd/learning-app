#!/usr/bin/env bash
# ============================================================
# 容器启动脚本
#
# 它按顺序做四件事：
#   1. 根据环境变量生成 config/db.json（数据库连接配置）
#   2. 等数据库真正能连上
#   3. 启动 AI 网关（后台运行）
#   4. 启动 C++ 后端（前台运行，日志直接进 docker compose logs）
#
# 为什么要"启动时生成 db.json"，而不是直接放一个现成的文件？
#   因为容器里数据库的地址是 "db"（compose 里的服务名），不是 127.0.0.1；
#   数据库密码来自 .env 文件，每个人都不一样。
#   这两样东西在本地写死的话，换台机器就跑不起来，所以必须运行时生成。
# ============================================================
set -euo pipefail

APP_ROOT="${APP_ROOT:-/app}"
cd "$APP_ROOT"

# ---------- 1. 生成数据库配置 ----------
DB_HOST="${DB_HOST:-db}"
DB_PORT="${DB_PORT:-3306}"
DB_NAME="${MYSQL_DATABASE:-english_learning}"
DB_USER="${MYSQL_USER:-eng_app}"
# 冒号加问号表示"必须设置，否则报错退出"——避免密码为空时静默连不上数据库
DB_PASSWORD="${MYSQL_PASSWORD:?MYSQL_PASSWORD 未设置，请检查 .env 文件}"

# 后端读取密码的方式是"从 password_file 指向的文件里读第一行"。
# 所以这里把密码写进 config/db_password，并收紧权限（只有容器内的 root 能读）。
# 注意：这个路径必须是【相对 config/ 目录】的形式，后端只按相对路径解析。
mkdir -p config
printf '%s' "$DB_PASSWORD" > config/db_password
chmod 600 config/db_password

cat > config/db.json <<EOF
{
  "host": "$DB_HOST",
  "port": $DB_PORT,
  "user": "$DB_USER",
  "password_file": "db_password",
  "database": "$DB_NAME",
  "charset": "utf8mb4",
  "connect_timeout": 5,
  "retry_times": 3,
  "retry_interval_sec": 2,
  "query_timeout_sec": 10
}
EOF

echo "[entrypoint] 已生成 config/db.json（数据库地址 $DB_HOST:$DB_PORT / 库 $DB_NAME）"

# ---------- 2. 等数据库就绪 ----------
# compose 的 healthcheck 已经会等 MySQL 起来，这里再兜一层。
# 原因：MySQL 首次启动要先执行 scripts/sql/ 里的 8 个建表脚本，可能耗时几十秒。
echo "[entrypoint] 等待数据库 $DB_HOST:$DB_PORT ..."
ready=0
for i in $(seq 1 90); do
  # /dev/tcp 是 bash 内置的 TCP 探测，不需要额外安装工具
  if (echo > "/dev/tcp/$DB_HOST/$DB_PORT") 2>/dev/null; then
    ready=1
    break
  fi
  sleep 1
done
if [ "$ready" != "1" ]; then
  echo "[entrypoint] 错误：90 秒内数据库仍未就绪，请查看 db 容器的日志：docker compose logs db" >&2
  exit 1
fi
echo "[entrypoint] 数据库已就绪"

# ---------- 2.5 测试用 root 密码文件 ----------
# 测试套件（tests/*.test.js）约定从 MYSQL_PW_FILE 读取 root 密码。
# 容器里没有宿主那份 .root_pw，所以在启动时按同一约定生成一份，仅容器内可见。
if [ -n "${MYSQL_ROOT_PASSWORD:-}" ]; then
  printf '%s' "$MYSQL_ROOT_PASSWORD" > /tmp/.mysql_root_pw
  chmod 600 /tmp/.mysql_root_pw
fi

# ---------- 3. AI 配置兜底 ----------
# config/ai.json 是密钥文件，不会打进镜像。
# 两种情况需要用示例配置兜底：
#   ① 文件不存在
#   ② 文件里的 api_key 含非 ASCII 字符（例如误把中文说明文字粘进去）
#      —— 这种 Key 会被 Node 当成 HTTP 头内容，直接抛 ERR_INVALID_CHAR 把网关打崩，
#         所以启动时就替换掉，避免容器陷入"崩溃→重启→再崩溃"的循环。
if [ ! -f config/ai.json ]; then
  cp config/ai.example.json config/ai.json
  echo "[entrypoint] 未提供 config/ai.json，已用示例配置生成（AI 功能需填 Key 后才可用）"
elif ! node -e '
  const c = require(process.argv[1]);
  const k = typeof c.api_key === "string" ? c.api_key : "";
  process.exit(/^[\x21-\x7e]*$/.test(k) ? 0 : 1);
' "$APP_ROOT/config/ai.json" 2>/dev/null; then
  cp config/ai.example.json config/ai.json
  echo "[entrypoint] 检测到 config/ai.json 中的 api_key 含非法字符，已重置为示例配置（请通过 ZHIPU_API_KEY 环境变量提供 Key）"
fi

# 尝试从环境变量读取 Key：这是 Docker / Codespaces 下最方便的用法，
# 不用改文件，只要在 .env 里写 ZHIPU_API_KEY=xxx 即可（网关脚本本身也支持这个变量）。
if [ -n "${ZHIPU_API_KEY:-}" ]; then
  echo "[entrypoint] 已从环境变量读取 ZHIPU_API_KEY"
fi

# ---------- 4. 启动两个服务 ----------
export LOCALAI_LOG_DIR="${LOCALAI_LOG_DIR:-/app/logs}"

echo "[entrypoint] 启动 AI 网关（内部端口 5340）..."
node scripts/local_ai_server.js &
GATEWAY_PID=$!

echo "[entrypoint] 启动后端（对外端口 8088）..."
# --host 0.0.0.0：容器内必须监听所有网卡，否则宿主机访问不到
./backend/build/eng_app_server --root "$APP_ROOT" --host 0.0.0.0 --port 8088 &
BACKEND_PID=$!

# ---------- 5. 停止处理 ----------
# docker compose down 会给本脚本发 SIGTERM，这里负责把两个子进程一起带走，
# 避免网关变成孤儿进程继续占用端口。
stop_all() {
  echo "[entrypoint] 收到停止信号，正在关闭服务..."
  kill "$GATEWAY_PID" "$BACKEND_PID" 2>/dev/null || true
  wait 2>/dev/null || true
  exit 0
}
trap stop_all TERM INT

# 等待任意一个进程退出：只要有一个挂了，整个容器就退出，
# 这样 docker compose 能立刻看到异常，而不是"容器还在但服务其实死了"。
wait -n
echo "[entrypoint] 有服务退出，容器即将关闭"
stop_all
