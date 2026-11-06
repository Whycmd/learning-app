// 阶段十六：对外开源资产（Docker / Codespaces / 物料）静态校验
//
// 本套件不启动容器，只检查"文件在不在、关键配置对不对"。
// 真正跑起来由 Taskfile / README 里的 docker compose up 人工验证。
const fs = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '..');

// 本套件校验的是"源码仓库里的开源资产"。
// Docker 运行镜像只包含运行需要的文件（不含 Dockerfile/README/LICENSE），
// 所以在容器内跳过，否则会全部误报为"文件缺失"。
const { IN_CONTAINER } = require('./_env');
if (IN_CONTAINER) {
  console.log('  ~ 容器内跳过：源码资产不在运行镜像中，本套件仅在本机执行');
  console.log('\n阶段十六（对外资产）: 通过 0 / 失败 0 / 跳过 1');
  process.exit(0);
}

let pass = 0, fail = 0, skipped = 0;
const failures = [];

function ok(name, msg = '') { pass++; console.log(`  ✓ ${name}${msg ? ' — ' + msg : ''}`); }
function ko(name, msg) { fail++; failures.push({ name, msg }); console.log(`  ✗ ${name} — ${msg}`); }
function skip(name, msg = '') { skipped++; console.log(`  ~ ${name}${msg ? ' — ' + msg : ''}`); }

function read(rel) {
  const p = path.join(ROOT, rel);
  if (!fs.existsSync(p)) throw new Error(`文件不存在: ${rel}`);
  return fs.readFileSync(p, 'utf8');
}

function exists(rel) { return fs.existsSync(path.join(ROOT, rel)); }

// ---------- 1. 必需文件存在 ----------
const REQUIRED = [
  'Dockerfile',
  'docker-compose.yml',
  'docker/entrypoint.sh',
  '.env.example',
  '.dockerignore',
  '.devcontainer/devcontainer.json',
  'README.md',
  'LICENSE',
  'CONTRIBUTING.md',
];

for (const f of REQUIRED) {
  if (exists(f)) ok(`存在 ${f}`);
  else ko(`存在 ${f}`, '文件缺失');
}

// ---------- 2. Issue 模板 ----------
const ISSUE_DIR = '.github/ISSUE_TEMPLATE';
if (exists(ISSUE_DIR)) {
  const files = fs.readdirSync(path.join(ROOT, ISSUE_DIR)).filter(f => f.endsWith('.md') || f.endsWith('.yml'));
  if (files.length >= 3) ok(`Issue 模板数量`, `${files.length} 个`);
  else ko(`Issue 模板数量`, `期望 ≥3，实际 ${files.length}`);
}
else ko('Issue 模板目录', `${ISSUE_DIR} 不存在`);

// ---------- 3. .dockerignore 必须排除密钥 ----------
try {
  const di = read('.dockerignore');
  const lines = di.split('\n').map(s => s.trim()).filter(s => s && !s.startsWith('#'));
  if (lines.includes('config/ai.json')) ok('.dockerignore 排除密钥', 'config/ai.json');
  else ko('.dockerignore 排除密钥', '未找到 config/ai.json 规则');

  if (lines.includes('node_modules/') || lines.includes('node_modules')) ok('.dockerignore 排除 node_modules');
  else ko('.dockerignore 排除 node_modules', '未找到对应规则');
}
catch (e) { ko('.dockerignore 检查', e.message); }

// ---------- 4. Dockerfile 关键指令 ----------
try {
  const df = read('Dockerfile');
  const fromCount = (df.match(/^FROM /gm) || []).length;
  if (fromCount >= 2) ok('Dockerfile 多阶段构建', `${fromCount} 个 FROM`);
  else ko('Dockerfile 多阶段构建', `期望 ≥2，实际 ${fromCount}`);

  if (/^EXPOSE 8088$/m.test(df)) ok('Dockerfile 暴露 8088');
  else ko('Dockerfile 暴露 8088', '未找到 EXPOSE 8088');

  if (/COPY tests\/ /m.test(df)) ok('Dockerfile 打包测试套件', '支持容器内回归');
  else ko('Dockerfile 打包测试套件', '未 COPY tests/');

  if (/mariadb-client/.test(df)) ok('Dockerfile 安装 mysql 客户端', '容器内可执行 SQL');
  else ko('Dockerfile 安装 mysql 客户端', '缺少 mariadb-client');
}
catch (e) { ko('Dockerfile 检查', e.message); }

// ---------- 5. docker-compose.yml 关键约束 ----------
try {
  const dc = read('docker-compose.yml');
  if (dc.includes('127.0.0.1:') && dc.includes('8088')) ok('compose 端口只绑本机', '127.0.0.1:8088');
  else ko('compose 端口只绑本机', '未发现 127.0.0.1 绑定');

  if (/condition: service_healthy/.test(dc)) ok('compose 依赖数据库健康检查');
  else ko('compose 依赖数据库健康检查', '缺少 service_healthy');

  if (/mysql_data/.test(dc)) ok('compose 数据卷持久化', 'mysql_data');
  else ko('compose 数据卷持久化', '缺少 mysql_data 卷');

  if (/MYSQL_BIN/i.test(dc)) ok('compose 注入测试环境变量', 'MYSQL_BIN / DB_HOST');
  else ko('compose 注入测试环境变量', '缺少 MYSQL_BIN');
}
catch (e) { ko('docker-compose 检查', e.message); }

// ---------- 6. entrypoint 关键步骤 ----------
try {
  const ep = read('docker/entrypoint.sh');
  if (/APP_ROOT/.test(ep)) ok('entrypoint 使用 APP_ROOT');
  else ko('entrypoint 使用 APP_ROOT', '未使用 APP_ROOT');

  if (/db\.json/.test(ep)) ok('entrypoint 运行时渲染 db.json');
  else ko('entrypoint 运行时渲染 db.json', '未发现 db.json 处理');

  if (/ai\.json/.test(ep)) ok('entrypoint 处理 AI 配置');
  else ko('entrypoint 处理 AI 配置', '未发现 ai.json 处理');

  if (/mysql_root_pw/.test(ep)) ok('entrypoint 生成测试密码文件');
  else ko('entrypoint 生成测试密码文件', '未生成 /tmp/.mysql_root_pw');
}
catch (e) { ko('entrypoint 检查', e.message); }

// ---------- 7. 测试环境参数化 ----------
try {
  const env = require('./_env');
  if (path.isAbsolute(env.MYSQL_BIN)) ok('_env 提供 MYSQL_BIN', env.MYSQL_BIN);
  else ko('_env 提供 MYSQL_BIN', 'MYSQL_BIN 不是绝对路径');

  if (exists('tests/_mysql_tcp_shim.sh')) ok('存在 TCP 兼容包装脚本', '_mysql_tcp_shim.sh');
  else ko('存在 TCP 兼容包装脚本', '文件缺失');
}
catch (e) { ko('_env 加载', e.message); }

// ---------- 8. devcontainer 配置合法 ----------
try {
  // devcontainer.json 用的是 JSONC（允许 // 注释），标准 JSON.parse 不认，
  // 所以先去掉整行注释再解析。
  const raw = read('.devcontainer/devcontainer.json');
  const cleaned = raw.split('\n').filter(l => !/^\s*\/\//.test(l)).join('\n');
  const dc = JSON.parse(cleaned);
  if (dc.name) ok('devcontainer 配置可解析', dc.name);
  else ko('devcontainer 配置可解析', '缺少 name 字段');
  if (JSON.stringify(dc).includes('8088')) ok('devcontainer 暴露 8088');
  else ko('devcontainer 暴露 8088', '未提及 8088');
}
catch (e) { ko('devcontainer 检查', e.message); }

// ---------- 9. 对外文档与许可证 ----------
try {
  const lic = read('LICENSE');
  if (/MIT License/i.test(lic)) ok('LICENSE 为 MIT');
  else ko('LICENSE 为 MIT', '未识别到 MIT 文本');
}
catch (e) { ko('LICENSE 检查', e.message); }

try {
  const rdm = read('README.md');
  if (/docker compose up/.test(rdm)) ok('README 含 Docker 启动指引');
  else ko('README 含 Docker 启动指引', '未找到 docker compose up');

  if (/127\.0\.0\.1:8088|localhost:8088/.test(rdm)) ok('README 含访问地址');
  else ko('README 含访问地址', '未找到 8088 地址');
}
catch (e) { ko('README 检查', e.message); }

// ---------- 汇总 ----------
console.log(`\n阶段十六（对外资产）: 通过 ${pass} / 失败 ${fail} / 跳过 ${skipped}`);
if (fail > 0) {
  console.log('\n失败明细:');
  for (const f of failures) console.log(`  ✗ ${f.name} — ${f.msg}`);
  process.exit(1);
}
process.exit(0);
