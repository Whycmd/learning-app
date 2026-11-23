// v1.5.0-T001：旧套件统一改用系统账号（user_id=1）会话
//
// 未登录一律视为无账号（uid=0）后，原先依赖共享游客号写入的套件需要显式登录。
// run_all.js 会在测试库预置会话并注入 ENG_TEST_SYSTEM_TOKEN；
// 单独运行某个套件时，这里按需自建会话并在进程退出时清理。
const crypto = require('crypto');
const fs = require('fs');
const { spawnSync } = require('child_process');
const { MYSQL_BIN, MYSQL_PW_FILE, SOCK, DB_NAME } = require('./_env');

function runMysql(sql) {
  const pw = fs.readFileSync(MYSQL_PW_FILE, 'utf8').trim();
  return spawnSync(
    MYSQL_BIN,
    ['--socket=' + SOCK, '-uroot', '-p' + pw, '--default-character-set=utf8mb4', DB_NAME, '-e', sql],
    { encoding: 'utf8', timeout: 15000 }
  );
}

let token = process.env.ENG_TEST_SYSTEM_TOKEN || '';

if (!token) {
  token = crypto.randomBytes(32).toString('hex');
  const r = runMysql(
    `INSERT INTO user_sessions (token,user_id,expires_at) VALUES ('${token}',1,DATE_ADD(NOW(),INTERVAL 1 DAY));`
  );
  if (r.status !== 0) throw new Error('系统账号会话创建失败：' + (r.stderr || r.stdout || '').trim());
  process.on('exit', () => runMysql(`DELETE FROM user_sessions WHERE token='${token}';`));
}

// 给请求头附加系统账号会话
function withSystemCookie(headers) {
  headers['Cookie'] = 'eng_session=' + token;
  return headers;
}

module.exports = { token, withSystemCookie };
