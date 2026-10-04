// 生成《程序架构说明》Word 文档
// 用法: node gen_arch_doc.js <outDir>
// 文档内容基于对项目代码的实际盘点（路由、表结构、模块、脚本均已核实）
const fs = require('fs');
const path = require('path');
const { Document, Packer, Paragraph, TextRun, HeadingLevel, AlignmentType, BorderStyle } = require('docx');

const outDir = process.argv[2] || '.';

const C_TITLE = '1F4E79';
const C_H2 = '2E74B5';
const C_ACCENT = 'C00000';
const C_MUTED = '666666';

function empty(s) { return new Paragraph({ spacing: { after: s || 120 } }); }
function titleLine(t) {
  return new Paragraph({ heading: HeadingLevel.TITLE, alignment: AlignmentType.CENTER, spacing: { after: 240 },
    children: [new TextRun({ text: t, bold: true, size: 40, color: C_TITLE })] });
}
function h2(t) {
  return new Paragraph({ spacing: { before: 280, after: 120 },
    border: { bottom: { style: BorderStyle.SINGLE, size: 3, color: C_H2 } },
    children: [new TextRun({ text: t, bold: true, size: 28, color: C_TITLE })] });
}
function h3(t) {
  return new Paragraph({ spacing: { before: 180, after: 80 },
    children: [new TextRun({ text: t, bold: true, size: 24, color: C_H2 })] });
}
function p(t, opts) {
  opts = opts || {};
  return new Paragraph({ spacing: { after: opts.after || 60 }, indent: opts.indent ? { left: opts.indent } : undefined,
    children: [new TextRun({ text: t, size: opts.size || 21, bold: opts.bold, italics: opts.italics, color: opts.color })] });
}
function runs(arr, opts) {
  opts = opts || {};
  return new Paragraph({ spacing: { after: opts.after || 60 }, indent: opts.indent ? { left: opts.indent } : undefined, children: arr });
}
function bullet(t) {
  return new Paragraph({ spacing: { after: 40 }, bullet: { level: 0 },
    children: [new TextRun({ text: t, size: 21 })] });
}
function kv(k, v) {
  return runs([
    new TextRun({ text: k + '：', bold: true, size: 21, color: C_H2 }),
    new TextRun({ text: v, size: 21 }),
  ], { indent: 240 });
}

const children = [];

children.push(titleLine('英语学习应用 · 程序架构说明'));
children.push(p(`生成时间：${new Date().toLocaleString('zh-CN')}  ·  版本：1.0.0`, { color: C_MUTED, after: 200 }));
children.push(p('本文档内容基于对项目全部源代码、路由、数据库表结构、脚本的实际盘点编写，反映程序当前真实架构。', { color: C_MUTED, italics: true }));

// 一、项目定位
children.push(h2('一、项目定位'));
children.push(p('轻量化本地英语学习软件：学习数据（知识点、学情、文章）全部存储在本地 MySQL，AI 能力在线调用智谱 AI 大模型（GLM-4-Flash，免费），通过本地 Node.js 网关转发 HTTPS 请求。基于遗忘曲线实现定时复习推送、AI 知识点固化拓展、AI 出题与自动批改、学情统计。'));

// 二、总体架构
children.push(h2('二、总体架构（分层）'));
children.push(h3('运行时四个进程'));
children.push(kv('MySQL 8.0 数据库', '本地单机运行，仅监听本机 Socket，端口不对外，存放全部业务数据（10 张表）'));
children.push(kv('AI 网关（Node.js）', 'scripts/local_ai_server.js，监听 127.0.0.1:5340，将 C++ 后端的 HTTP 请求转为 HTTPS 转发至智谱 open.bigmodel.cn，注入 API Key'));
children.push(kv('C++ 后端服务', 'backend/build/eng_app_server，监听 127.0.0.1:8088，提供全部 REST API、托管前端页面、调用 Node.js 子进程生成 Word'));
children.push(kv('定时清理任务（Node.js）', 'scripts/scheduler_cleanup.js，每小时触发一次临时 Word 清理（超过 1 小时的文件删除）'));
children.push(h3('数据流向'));
children.push(p('浏览器（5 个 HTML 页面）──HTTP──> C++ 后端（8088）──SQL──> 本地 MySQL', { indent: 240 }));
children.push(p('C++ 后端 ──HTTP──> AI 网关（5340）──HTTPS──> 智谱 GLM-4-Flash 在线 API', { indent: 240 }));
children.push(p('C++ 后端 ──spawn──> Node.js（gen_word_kp.js / gen_word_exercise.js）──> 生成 .docx 到 word_tmp/', { indent: 240 }));

// 三、技术栈
children.push(h2('三、技术栈'));
children.push(bullet('后端：C++17（clang++ 编译），纯 BSD Socket 手写 HTTP 服务器，无外部 Web 框架'));
children.push(bullet('数据库：MySQL 8.0，本地部署，无外键约束（关联一致性由代码保证），InnoDB 支持事务'));
children.push(bullet('AI：智谱 AI BigModel GLM-4-Flash（完全免费），OpenAI 兼容接口，模型与 Key 在 config/ai.json 配置'));
children.push(bullet('网关/脚本：Node.js（本地安装），仅用内置模块 http/https/fs；Word 生成使用 npm 的 docx 库'));
children.push(bullet('前端：原生 HTML + CSS + JavaScript，无框架，由 C++ 后端直接托管为静态文件'));
children.push(bullet('一键启动：start_all.command（桌面快捷方式「英语学习应用.command」双击启动全部服务并打开浏览器）'));

// 四、后端模块
children.push(h2('四、后端代码模块（backend/）'));
children.push(p('后端为纯头文件组织，入口 src/main.cpp（注册全部路由），8 个头文件各自独立：'));
children.push(kv('http_server.h', '手写 HTTP 服务器：路由注册（GET/POST/PUT/DELETE、路径参数）、JSON body 解析、CORS、线程模型、Connection: close'));
children.push(kv('db.h', 'MySQL 连接封装：查询/执行、事务 transaction()、自动重试（冲突重试 3 次）、转义工具'));
children.push(kv('json.h', '轻量 JSON 解析/序列化（对象、数组、类型转换、dump）'));
children.push(kv('ai_client.h', 'AI 客户端：读取 config/ai.json，调用网关 /v1/chat/completions，超时与重试（默认 2 次），从 choices[0].message.content 提取 JSON'));
children.push(kv('knowledge_service.h', '知识点服务：录入查重、单条固化 solidifyOne、批量固化 solidifyBatch（一次 AI 调用处理多个单词）、入库 applySolidify、人工合并/拆分、单个/批量 Word 导出'));
children.push(kv('learning_service.h', '学情服务：0-5 级熟练度、双模式遗忘曲线（按周/按日）、候选池排序、选题调整、答题记录、学情统计'));
children.push(kv('question_service.h', 'AI 出题（6 大固定题型）、客观题本地批改、主观题 AI 批改、批量批改、生成习题 Word'));
children.push(kv('maintenance_service.h', '维护服务：数据库备份导出（.sql）、单条/全量学情重置、启动时清理、临时 Word 清理'));

// 五、AI 调用机制
children.push(h2('五、AI 调用机制（四类功能）'));
children.push(p('严格遵守极低 AI 消耗：仅固化、出题、批改时调用；固化结果入库永久复用，复习习题实时生成不入库。'));
children.push(h3('输出格式分工'));
children.push(bullet('固化（JSON）：AI 返回 JSON，解析后精简写入 MySQL 各表（standard_content/difficulty/释义/搭配/近反义词/音标/词根/辨析/短语额外解析等）'));
children.push(bullet('拓展（Word）：固化数据汇总生成知识点拓展 Word；多个知识点可合并为一个文档导出（/api/kp/export-batch-word）'));
children.push(bullet('出题（Word）：AI 实时生成 6 大题型 JSON，由 gen_word_exercise.js 生成习题 Word，文件不入库'));
children.push(bullet('批改（JSON）：客观题本地代码比对；主观题（翻译/仿写）调 AI 返回得分、正误、解析、参考答案'));
children.push(h3('批量固化（减少对接次数）'));
children.push(p('一键固化时，所有未固化单词合并为一次 AI 请求（返回 JSON 数组），逐个解析入库；对较长短语要求 AI 额外解析其中包含的单词和子短语（phrase_analysis 字段）。'));

// 六、数据库表
children.push(h2('六、数据库表（10 张，无外键）'));
children.push(kv('knowledge_points', '知识点主表（录入内容、标准词条、难度、掌握要求 会读/会写、固化状态）'));
children.push(kv('kp_definitions', '释义子表（词性、英文释义、真题例句）'));
children.push(kv('kp_collocations', '固定搭配子表（搭配、例句）'));
children.push(kv('kp_synonyms / kp_antonyms', '近义词表 / 反义词表'));
children.push(kv('kp_extras', '扩展表（音标、使用频率、词根词源、重难点、同义词辨析、短语额外解析、语域、文化延伸）'));
children.push(kv('user_learning', '学情表（熟练度 0-5、复习次数、错误次数、下次复习时间、按周/按日模式）'));
children.push(kv('review_logs', '复习流水记录'));
children.push(kv('articles', '文章资源表（含匹配阈值字段，自适应匹配为后期迭代）'));
children.push(kv('ai_tasks', 'AI 任务记录表（任务类型、重试次数、状态）'));

// 七、API 路由清单
children.push(h2('七、REST API 清单（30 个，端口 8088）'));
children.push(h3('知识点（/api/kp）'));
children.push(p('POST /api/kp 录入 ｜ GET /api/kp 列表 ｜ GET /api/kp/:id 详情', { indent: 240 }));
children.push(p('POST /api/kp/solidify 一键批量固化 ｜ POST /api/kp/:id/solidify 单条固化', { indent: 240 }));
children.push(p('POST /api/kp/:id/export-word 单个导出 Word ｜ POST /api/kp/export-batch-word 批量合并导出 Word', { indent: 240 }));
children.push(p('POST /api/kp/:src/merge/:dst 合并 ｜ POST /api/kp/:id/split 拆分 ｜ PUT /api/kp/:id/standard 人工修正', { indent: 240 }));
children.push(h3('学情与复习（/api/learn）'));
children.push(p('GET /api/learn/candidates 待复习候选池 ｜ GET /api/learn/stats 学情统计', { indent: 240 }));
children.push(p('GET /api/learn/:id 单条学情 ｜ PUT /api/learn/:id/proficiency 熟练度 ｜ PUT /api/learn/:id/mode 复习模式', { indent: 240 }));
children.push(p('POST /api/learn/select 人工选题 ｜ POST /api/learn/:id/answer 答题记录 ｜ POST /api/learn/:id/complete-review 完成复习', { indent: 240 }));
children.push(h3('习题（/api/exercise）'));
children.push(p('POST /api/exercise/generate AI 出题（返回 Word 路径）', { indent: 240 }));
children.push(p('POST /api/exercise/grade 单题批改 ｜ POST /api/exercise/batch-grade 批量批改', { indent: 240 }));
children.push(h3('维护（/api/maint）'));
children.push(p('POST /api/maint/backup 数据备份 ｜ GET /api/maint/backups 备份列表', { indent: 240 }));
children.push(p('POST /api/maint/reset-learning/:id 单条重置 ｜ POST /api/maint/clear-all-learning 全量重置', { indent: 240 }));
children.push(p('POST /api/maint/cleanup-temp-word 临时文件清理 ｜ POST /api/maint/cleanup-startup 启动清理', { indent: 240 }));
children.push(h3('其他'));
children.push(p('GET /api/health 健康检查 ｜ GET /api/word/download Word 下载 ｜ GET / 与 /pages/* 前端静态页面', { indent: 240 }));

// 八、前端页面
children.push(h2('八、前端页面（frontend/pages/，由 8088 端口托管）'));
children.push(kv('kp-entry.html', '知识点录入页：单条录入、一键批量 AI 固化、批量导出拓展 Word、人工修正入口'));
children.push(kv('kp-detail.html', '知识点详情页：词条、释义、搭配、近反义词、熟练度、复习记录'));
children.push(kv('review.html', '每日复习页：候选池、人工选题、AI 出题（下载 Word）、答题提交与批改反馈'));
children.push(kv('learning-center.html', '学情中心：统计、熟练度分布、数据备份、学情重置'));
children.push(kv('articles.html', '文章页：目前为手动录入文章正文的简易页面（AI 文章自适应匹配属后期迭代功能）'));

// 九、Word 文档机制
children.push(h2('九、Word 文档与临时文件管理'));
children.push(bullet('生成方式：C++ 后端写临时 JSON 到 word_tmp/，spawn 调用 Node.js + docx 库生成 .docx，返回路径供下载'));
children.push(bullet('拓展文档：gen_word_kp.js，支持单个（knowledge_point_<id>.docx）与批量合并（knowledge_points_batch.docx）两种模式'));
children.push(bullet('习题文档：gen_word_exercise.js（exercise_<id>.docx），含会读/会写分类、6 大题型、满分词汇总结'));
children.push(bullet('清理规则：软件启动时清空历史习题文件；运行中每小时扫描删除超过 1 小时的文件；临时文件不入库'));

// 十、启动与部署
children.push(h2('十、启动方式'));
children.push(bullet('双击桌面「英语学习应用.command」：依次启动 MySQL → AI 网关 → C++ 后端，自动打开浏览器访问 http://127.0.0.1:8088/'));
children.push(bullet('双击桌面「停止英语学习.command」：优雅停止全部服务'));
children.push(bullet('配置文件：config/db.json（数据库）、config/ai.json（AI 模型与 Key）、config/app.json（端口、复习模式等）'));
children.push(bullet('备份：数据备份导出为 .sql 文件存于 backups/ 目录'));

// 十一、关键算法
children.push(h2('十一、关键业务算法'));
children.push(bullet('遗忘曲线（按周模式）：下次复习时间 = 当日 + 2×(1+2^熟练度等级) 天'));
children.push(bullet('遗忘曲线（按日模式）：下次复习时间 = 当日 + 0.4×(1+3^熟练度等级) 天'));
children.push(bullet('掌握程度自动判定：难度 ≥ 5（雅思水平）标记「会读」，否则「会写」，由代码判定（不信任 AI 返回值）'));
children.push(bullet('固化查重：按标准词条语义等价判断，重复录入自动合并；支持人工合并/拆分知识点'));
children.push(bullet('6 大固定题型：词义匹配（阅读短文）、词性转换填空、语境选词填空、中译英翻译、词汇辨析完形选择、例句仿写'));

children.push(empty(300));
children.push(new Paragraph({ alignment: AlignmentType.CENTER,
  children: [new TextRun({ text: '— 本架构说明基于程序当前实际代码生成 —', size: 18, color: C_MUTED, italics: true })] }));

const doc = new Document({ sections: [{ children }] });

(async () => {
  try {
    fs.mkdirSync(outDir, { recursive: true });
    const buf = await Packer.toBuffer(doc);
    const outPath = path.join(outDir, '程序架构说明.docx');
    fs.writeFileSync(outPath, buf);
    console.log(outPath);
    process.exit(0);
  } catch (e) {
    console.error('docx generation failed:', e.message);
    process.exit(3);
  }
})();
