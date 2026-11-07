#!/usr/bin/env bash
# API Key 安全存储维护脚本（Linux/macOS 通用）
# 默认密钥文件：~/.config/eng-app/ai.key（权限 600，仓库外）
# 可用 ENG_AI_KEY_FILE 覆盖路径
# 用法：
#   bash scripts/ai_key_store.sh set        # 从标准输入读取 Key（交互模式会提示）
#   bash scripts/ai_key_store.sh check      # 检查路径、权限、长度（不输出 Key）
#   bash scripts/ai_key_store.sh delete     # 删除密钥文件
#   bash scripts/ai_key_store.sh migrate    # 把 config/ai.json 的明文 Key 迁入文件并清空配置
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG_FILE="${AI_CONFIG_FILE:-$ROOT_DIR/config/ai.json}"
KEY_FILE="${ENG_AI_KEY_FILE:-$HOME/.config/eng-app/ai.key}"

die() { echo "错误：$*" >&2; exit 1; }

valid_key() {
  local key="$1"
  [ -n "$key" ] || return 1
  case "$key" in
    YOUR_*|local-offline-no-key) return 1 ;;
  esac
  # 与网关一致：仅允许可打印 ASCII（不含空格）
  printf '%s' "$key" | LC_ALL=C grep -Eq '^[!-~]+$' || return 1
  return 0
}

mode_of() {
  stat -c %a "$1" 2>/dev/null || stat -f %Lp "$1" 2>/dev/null || echo "unknown"
}

write_key() {
  local key="$1"
  local dir tmp
  dir="$(dirname "$KEY_FILE")"
  umask 077
  mkdir -p "$dir"
  tmp="$(mktemp "$dir/.ai.key.XXXXXX")"
  trap 'rm -f "$tmp"' RETURN
  printf '%s\n' "$key" > "$tmp"
  chmod 600 "$tmp"
  mv -f "$tmp" "$KEY_FILE"
  trap - RETURN
}

cmd_set() {
  local key=""
  if [ -t 0 ]; then
    printf '请输入 API Key（输入不回显）：' >&2
    IFS= read -rs key || true
    printf '\n' >&2
  else
    IFS= read -r key || true
    key="${key%$'\r'}"
  fi
  valid_key "$key" || die "Key 为空或包含非法字符（仅允许可打印 ASCII，不含空格）"
  write_key "$key"
  echo "已写入：${KEY_FILE}（权限 600，长度 ${#key}）"
}

cmd_check() {
  if [ ! -f "$KEY_FILE" ]; then
    echo "状态：未配置（文件不存在：${KEY_FILE}）"
    exit 1
  fi
  local mode key
  mode="$(mode_of "$KEY_FILE")"
  key="$(cat "$KEY_FILE" | tr -d '\r' | head -n 1)"
  echo "路径：$KEY_FILE"
  echo "权限：$mode"
  if [ "$mode" != "600" ]; then
    echo "状态：权限不合规，网关将忽略该文件；请执行 chmod 600 \"$KEY_FILE\""
    exit 1
  fi
  if ! valid_key "$key"; then
    echo "状态：内容为空或非法"
    exit 1
  fi
  echo "状态：已配置（长度 ${#key}）"
}

cmd_delete() {
  if [ ! -e "$KEY_FILE" ]; then
    echo "文件不存在：$KEY_FILE"
    return 0
  fi
  rm -f "$KEY_FILE"
  echo "已删除：$KEY_FILE"
}

cmd_migrate() {
  [ -f "$CONFIG_FILE" ] || die "配置文件不存在：$CONFIG_FILE"
  local key
  key="$(node -e '
    const fs = require("fs");
    const p = process.argv[1];
    const j = JSON.parse(fs.readFileSync(p, "utf8"));
    process.stdout.write(String(j.api_key || "").trim());
  ' "$CONFIG_FILE")"
  valid_key "$key" || die "配置中的 api_key 为空或非法，无法迁移；可直接使用 set"
  write_key "$key"
  node -e '
    const fs = require("fs");
    const p = process.argv[1];
    const j = JSON.parse(fs.readFileSync(p, "utf8"));
    j.api_key = "";
    fs.writeFileSync(p, JSON.stringify(j, null, 2) + "\n");
  ' "$CONFIG_FILE"
  echo "已迁移：$CONFIG_FILE → ${KEY_FILE}（权限 600，长度 ${#key}）；配置中的 api_key 已置空"
}

case "${1:-}" in
  set) cmd_set ;;
  check) cmd_check ;;
  delete) cmd_delete ;;
  migrate) cmd_migrate ;;
  *)
    echo "用法：bash scripts/ai_key_store.sh {set|check|delete|migrate}" >&2
    exit 2
    ;;
esac
