#!/usr/bin/env node
// 定时任务：临时 Word 文件清理
// - 软件启动时已由后端清理
// - 运行中每小时扫描一次，删除生成超过1小时的习题 Word
// 用法: node scheduler_cleanup.js (前台运行，每小时执行一次)
const http = require('http');

const BACKEND_PORT = 8088;
const BACKEND_HOST = '127.0.0.1';

function callCleanup() {
  const req = http.request({
    host: BACKEND_HOST, port: BACKEND_PORT, method: 'POST',
    path: '/api/maint/cleanup-temp-word?max_age=3600',
    headers: { 'Content-Type': 'application/json', 'Content-Length': 0 }
  }, res => {
    let chunks = [];
    res.on('data', c => chunks.push(c));
    res.on('end', () => {
      try {
        const j = JSON.parse(Buffer.concat(chunks).toString('utf8'));
        const ts = new Date().toISOString();
        if (j.deleted > 0) {
          console.log(`[${ts}] 清理临时文件: 删除 ${j.deleted} 个, 跳过 ${j.skipped} 个`);
        }
      } catch(e) {}
    });
  });
  req.on('error', () => {});
  req.end();
}

console.log(`[scheduler] 临时 Word 清理任务启动, 每小时执行一次`);
callCleanup();
setInterval(callCleanup, 60 * 60 * 1000);

process.on('SIGINT', () => { console.log('[scheduler] 退出'); process.exit(0); });
process.on('SIGTERM', () => { console.log('[scheduler] 退出'); process.exit(0); });
