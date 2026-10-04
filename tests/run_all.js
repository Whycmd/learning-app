#!/usr/bin/env node
// 全量测试运行器（Node.js 原生，无需 jest）
const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');

const TEST_DIR = path.resolve(__dirname);
const testFiles = fs.readdirSync(TEST_DIR)
  .filter(f => f.endsWith('.test.js'))
  .sort();

async function runTest(file) {
  return new Promise((resolve) => {
    const proc = spawn(process.execPath, [path.join(TEST_DIR, file)], {
      stdio: ['inherit', 'inherit', 'inherit']
    });
    proc.on('close', (code) => {
      resolve(code === 0);
    });
  });
}

async function main() {
  console.log(`\n=== 运行 ${testFiles.length} 个测试套件 ===\n`);
  let passed = 0, failed = 0;
  for (const f of testFiles) {
    process.stdout.write(`▶ ${f} ... `);
    const ok = await runTest(f);
    if (ok) {
      passed++;
      console.log('✓');
    } else {
      failed++;
      console.log('✗');
    }
  }
  console.log(`\n=== 结果: ${passed} 通过, ${failed} 失败, 共 ${testFiles.length} 套件 ===\n`);
  process.exit(failed > 0 ? 1 : 0);
}

main().catch(e => { console.error(e); process.exit(1); });
