#!/usr/bin/env bash
# 容器内测试专用的 mysql 客户端包装
#
# 为什么需要它：
#   测试代码调用 mysql 时写的是 `--socket=<本地 socket 路径>`，
#   但容器里的 MySQL 是独立服务（db 容器），只能走 TCP，没有共享 socket 文件。
#
# 这个脚本做的事：把 `--socket=xxx` 参数丢掉，换成 `-h <host> -P <port>`，
# 其余参数（用户名、密码、库名、-e SQL）原样透传给真正的 mysql 客户端。
# 这样 15 个测试套件一行代码都不用改，就能在容器里跑。
set -euo pipefail

args=()
for a in "$@"; do
  case "$a" in
    --socket=*) ;;               # 丢弃 socket 参数
    *) args+=("$a") ;;
  esac
done

# 客户端默认字符集必须显式指定 utf8mb4。
# 原因：mariadb-client 在容器里默认用 latin1 输出，中文会被压成 "??"，
#      导致 JS 端读到错误的字符串（实际库里存的是正确 UTF-8）。
exec mysql --default-character-set=utf8mb4 -h "${DB_HOST:-db}" -P "${DB_PORT:-3306}" "${args[@]}"
