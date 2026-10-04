// 本地 AI 网关：转发到智谱AI BigModel 在线 API（OpenAI 兼容）
//
// 架构：C++ 后端 (ai_client.h, 仅支持 HTTP) → 本网关 (127.0.0.1:5340) → 智谱AI HTTPS API
// 网关职责：HTTP→HTTPS 转换、注入 API Key、请求转发、响应透传
//
// 智谱AI BigModel OpenAI 兼容端点：
//   POST https://open.bigmodel.cn/api/paas/v4/chat/completions
//   Header: Authorization: Bearer <ZHIPU_API_KEY>
//   免费模型：glm-4-flash（完全免费，无额度限制）/ glm-4-flashx / glm-4.7-flash
//
// 升级路径：更换 config/ai.json 中的 api_key 和 chat_model 即可切换模型

const http = require('http');
const https = require('https');
const fs = require('fs');
const path = require('path');

const PORT = parseInt(process.env.LOCALAI_PORT || '5340', 10);
const HOST = process.env.LOCALAI_HOST || '127.0.0.1';
const LOG_DIR = '/Users/junwang/.local/log';
const LOG_FILE = path.join(LOG_DIR, 'localai.out');

fs.mkdirSync(LOG_DIR, { recursive: true });
const logStream = fs.createWriteStream(LOG_FILE, { flags: 'a' });
function log(msg) {
  const line = `[${new Date().toISOString()}] ${msg}`;
  console.log(line);
  logStream.write(line + '\n');
}

// 加载配置（智谱AI API Key + 模型）
const AI_CONFIG_PATH = path.join(__dirname, '..', 'config', 'ai.json');
function loadConfig() {
  try {
    const raw = fs.readFileSync(AI_CONFIG_PATH, 'utf8');
    return JSON.parse(raw);
  } catch (e) {
    log(`[gateway] FATAL: cannot read ${AI_CONFIG_PATH}: ${e.message}`);
    return null;
  }
}

const REMOTE_HOST = 'open.bigmodel.cn';
const REMOTE_PATH = '/api/paas/v4/chat/completions';

function isPlaceholderKey(key) {
  return !key || key.startsWith('YOUR_') || key === 'local-offline-no-key';
}

const server = http.createServer((req, res) => {
  // 健康检查
  if (req.method === 'GET' && (req.url === '/readyz' || req.url === '/health')) {
    const cfg = loadConfig();
    const keyOk = cfg && !isPlaceholderKey(cfg.api_key);
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({
      status: keyOk ? 'ok' : 'no-api-key',
      engine: 'glm-online-proxy',
      model: cfg ? cfg.chat_model : 'unknown',
      remote: REMOTE_HOST,
      api_key_configured: keyOk,
      note: keyOk ? '' : '请在 config/ai.json 填入智谱AI BigModel API Key'
    }));
    return;
  }

  // 模型列表
  if (req.method === 'GET' && req.url === '/v1/models') {
    const cfg = loadConfig();
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ object: 'list', data: [{ id: cfg ? cfg.chat_model : 'glm-4-flash' }] }));
    return;
  }

  // 核心：转发 chat/completions
  if (req.method === 'POST' && req.url === '/v1/chat/completions') {
    const cfg = loadConfig();
    if (!cfg) {
      res.writeHead(503, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ error: { message: 'AI config not found' } }));
      return;
    }
    if (isPlaceholderKey(cfg.api_key)) {
      res.writeHead(401, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({
        error: { message: '智谱AI API Key 未配置，请在 config/ai.json 的 api_key 字段填入 BigModel API Key（注册 https://open.bigmodel.cn 获取，GLM-4-Flash 完全免费）' }
      }));
      return;
    }

    let body = '';
    req.on('data', c => body += c);
    req.on('end', () => {
      let responded = false;
      function sendResp(status, headers, data) {
        if (responded) return;
        responded = true;
        try { res.writeHead(status, headers); res.end(data); } catch(e) { log('[gateway] sendResp err: ' + e.message); }
      }
      const reqSize = Buffer.byteLength(body);
      const options = {
        hostname: REMOTE_HOST,
        port: 443,
        path: REMOTE_PATH,
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
          'Authorization': 'Bearer ' + cfg.api_key,
          'Content-Length': reqSize,
          'Connection': 'keep-alive'
        }
      };

      const t0 = Date.now();
      const proxy = https.request(options, remoteRes => {
        let data = '';
        remoteRes.on('data', c => data += c);
        remoteRes.on('end', () => {
          const elapsed = Date.now() - t0;
          const status = remoteRes.statusCode;
          const respHeaders = { 'Content-Type': remoteRes.headers['content-type'] || 'application/json' };
          sendResp(status, respHeaders, data);
          if (status !== 200) {
            log(`[gateway] ${status} ${elapsed}ms ${data.slice(0, 300)}`);
          } else {
            log(`[gateway] 200 ${elapsed}ms ${reqSize}B→${Buffer.byteLength(data)}B model=${cfg.chat_model}`);
          }
        });
      });

      proxy.on('error', e => {
        log(`[gateway] ERROR: ${e.message}`);
        sendResp(502, { 'Content-Type': 'application/json' }, JSON.stringify({
          error: { message: 'AI 网关转发失败: ' + e.message + '（请检查网络连接）' }
        }));
      });

      proxy.on('timeout', () => {
        log('[gateway] TIMEOUT');
        proxy.destroy();
        sendResp(504, { 'Content-Type': 'application/json' }, JSON.stringify({ error: { message: 'AI 请求超时' } }));
      });

      const timeout = (cfg.request_timeout_sec || 120) * 1000;
      proxy.setTimeout(timeout);

      proxy.write(body);
      proxy.end();
    });
    return;
  }

  res.writeHead(404, { 'Content-Type': 'text/plain' });
  res.end('Not found');
});

server.listen(PORT, HOST, () => {
  const cfg = loadConfig();
  const keyOk = cfg && !isPlaceholderKey(cfg.api_key);
  log(`[gateway] listening on http://${HOST}:${PORT}, forwarding to https://${REMOTE_HOST}, model=${cfg ? cfg.chat_model : '?'}, api_key=${keyOk ? 'configured' : 'MISSING'}`);
  if (!keyOk) {
    log('[gateway] WARNING: API Key 未配置，请在 config/ai.json 填入智谱AI BigModel API Key');
  }
});

process.on('SIGINT', () => { log('[gateway] shutdown'); process.exit(0); });
process.on('SIGTERM', () => { log('[gateway] shutdown'); process.exit(0); });
