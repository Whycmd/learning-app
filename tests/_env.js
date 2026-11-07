// 测试环境统一配置
//
// 目的：让同一套测试既能在 macOS 本机跑（MySQL 走 Unix Socket + 便携版客户端），
//      也能在 Docker 容器里跑（MySQL 走 TCP 连 db 服务）。
//
// 取值优先级：环境变量 > 默认值。
// 本机不需要设置任何环境变量，默认值就能用；
// 容器内由 docker-compose.yml 显式注入 MYSQL_BIN / MYSQL_PW_FILE / DB_HOST 等。
const os = require('os');
const fs = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '..');

// 空字符串也当作"未设置"，避免 compose 里写了 KEY= 却传空值时误判
function env(key, fallback) {
  const v = process.env[key];
  return v === undefined || v === '' ? fallback : v;
}

// macOS 本机的便携版 MySQL 安装在 ~/.local 下。
// 这里用 os.homedir() 推导而不是写死绝对路径，换用户名、换机器都不用改。
const LOCAL = path.join(os.homedir(), '.local');

const MYSQL_BIN = env('MYSQL_BIN', path.join(LOCAL, 'mysql/bin/mysql'));
const MYSQL_PW_FILE = env('MYSQL_PW_FILE', path.join(LOCAL, 'mysql/.root_pw'));
const SOCK = env('MYSQL_SOCKET', path.join(LOCAL, 'run/mysql.sock'));

const WORD_TMP = env('WORD_TMP', path.join(ROOT, 'word_tmp'));
const BACKUP_DIR = env('BACKUP_DIR', path.join(ROOT, 'backups'));

// 测试隔离：run_all.js 会注入指向独立测试库与独立后端的环境变量；
// 单独执行某个套件时回落到开发默认值，行为与改造前一致。
const DB_NAME = env('DB_NAME', env('TEST_DB_NAME', 'english_learning'));
const API_HOST = env('API_HOST', '127.0.0.1');
const API_PORT = Number(env('API_PORT', env('TEST_API_PORT', '8088')));
const AI_MOCK_MODE = env('AI_MOCK_MODE', env('ENG_AI_MOCK_MODE', 'off'));

// 是否运行在 Docker 容器里。
// 部分断言只在"本机开发环境"才成立（例如 clang++ 是否存在、端口是否只绑 127.0.0.1），
// 容器里这些条件天然不满足，需要跳过而不是判失败。
const IN_CONTAINER = fs.existsSync('/.dockerenv') || env('APP_ROOT', '') === '/app';

module.exports = {
  ROOT,
  IN_CONTAINER,
  MYSQL_BIN,
  MYSQL_PW_FILE,
  // stage14 的历史变量名是 PW_FILE，保留别名避免改测试逻辑
  PW_FILE: MYSQL_PW_FILE,
  SOCK,
  WORD_TMP,
  BACKUP_DIR,
  DB_NAME,
  API_HOST,
  API_PORT,
  AI_MOCK_MODE,
};
