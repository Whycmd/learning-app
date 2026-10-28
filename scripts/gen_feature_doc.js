const {
  Document, Packer, Paragraph, TextRun, Table, TableRow, TableCell,
  Header, Footer, AlignmentType, LevelFormat,
  WidthType, BorderStyle, ShadingType,
  HeadingLevel, PageNumber, PageBreak
} = require('docx');
const fs = require('fs');

// ── 辅助函数 ──
const H1 = (text) => new Paragraph({ heading: HeadingLevel.HEADING_1, children: [new TextRun({ text, bold: true, size: 32 })] });
const H2 = (text) => new Paragraph({ heading: HeadingLevel.HEADING_2, children: [new TextRun({ text, bold: true, size: 28 })] });
const H3 = (text) => new Paragraph({ heading: HeadingLevel.HEADING_3, children: [new TextRun({ text, bold: true, size: 24 })] });
const P = (text, opts = {}) => new Paragraph({
  children: [new TextRun({ text, size: 22, ...opts })],
  spacing: { after: 120, line: 360 },
});
const Bullet = (text) => new Paragraph({
  bullet: { level: 0 }, children: [new TextRun({ text, size: 22 })],
  spacing: { after: 80, line: 340 },
});
const Empty = () => new Paragraph({ children: [], spacing: { after: 60 } });

// 表格行工厂
const tr = (cells, isHeader = false) =>
  new TableRow({
    children: cells.map(c => new TableCell({
      children: [new Paragraph({
        children: [new TextRun({
          text: String(c),
          size: 20,
          bold: isHeader,
          color: isHeader ? "FFFFFF" : "000000",
        })],
        spacing: { after: 60 },
      })],
      shading: isHeader ? { fill: "4A7FB5", type: ShadingType.CLEAR } : undefined,
      margins: { top: 60, bottom: 60, left: 100, right: 100 },
    }))
  });

// 页边距：左2.5cm 右2cm
const border = { style: BorderStyle.SINGLE, size: 1, color: "CCCCCC" };
const cellBorder = { top: border, bottom: border, left: border, right: border };

const doc = new Document({
  styles: {
    default: { document: { run: { font: "微软雅黑", size: 22 } } },
    paragraphStyles: [
      { id: "Heading1", name: "Heading 1", basedOn: "Normal", next: "Normal", quickFormat: true,
        run: { size: 32, bold: true, font: "微软雅黑", color: "2C5F8D" },
        paragraph: { spacing: { before: 360, after: 200 }, outlineLevel: 0 } },
      { id: "Heading2", name: "Heading 2", basedOn: "Normal", next: "Normal", quickFormat: true,
        run: { size: 28, bold: true, font: "微软雅黑", color: "3A7CB8" },
        paragraph: { spacing: { before: 280, after: 160 }, outlineLevel: 1 } },
      { id: "Heading3", name: "Heading 3", basedOn: "Normal", next: "Normal", quickFormat: true,
        run: { size: 24, bold: true, font: "微软雅黑", color: "4A90C4" },
        paragraph: { spacing: { before: 200, after: 120 }, outlineLevel: 2 } },
    ]
  },
  numbering: {
    config: [
      { reference: "bullets",
        levels: [{ level: 0, format: LevelFormat.BULLET, text: "\u2022", alignment: AlignmentType.LEFT,
          style: { paragraph: { indent: { left: 720, hanging: 360 } } } }] },
    ]
  },
  sections: [{
    properties: {
      page: { margin: { top: 1440, right: 1260, bottom: 1440, left: 1440 } }
    },
    headers: {
      default: new Header({
        children: [new Paragraph({
          children: [new TextRun({ text: "英语学习应用 · 功能设计说明书", size: 18, color: "999999", italics: true })],
          alignment: AlignmentType.RIGHT,
        })],
      })
    },
    footers: {
      default: new Footer({
        children: [new Paragraph({
          children: [
            new TextRun({ text: "第 ", size: 18 }),
            new TextRun({ children: [PageNumber.CURRENT], size: 18 }),
            new TextRun({ text: " 页", size: 18 }),
          ],
          alignment: AlignmentType.CENTER,
        })],
      })
    },
    children: [
      // ─────────────────────────────────────────────
      // 封面
      // ─────────────────────────────────────────────
      new Paragraph({ alignment: AlignmentType.CENTER, spacing: { before: 2000 }, children: [] }),
      new Paragraph({ alignment: AlignmentType.CENTER, spacing: { after: 200 },
        children: [new TextRun({ text: "英语学习应用", size: 56, bold: true, font: "微软雅黑" })] }),
      new Paragraph({ alignment: AlignmentType.CENTER, spacing: { after: 120 },
        children: [new TextRun({ text: "功能设计说明书", size: 36, color: "4A7FB5", font: "微软雅黑" })] }),
      new Paragraph({ alignment: AlignmentType.CENTER, spacing: { after: 600 },
        children: [new TextRun({ text: "English Learning Application — Feature Design Specification", size: 20, color: "888888" })] }),
      new Paragraph({ alignment: AlignmentType.CENTER, spacing: { after: 200 }, children: [] }),
      new Paragraph({ alignment: AlignmentType.CENTER, spacing: { after: 80 },
        children: [new TextRun({ text: "版本：v1.4.0（目标）", size: 24 })] }),
      new Paragraph({ alignment: AlignmentType.CENTER, spacing: { after: 80 },
        children: [new TextRun({ text: "日期：2026-10-07", size: 24 })] }),
      new Paragraph({ alignment: AlignmentType.CENTER, spacing: { after: 80 },
        children: [new TextRun({ text: "适用对象：合作开发者、新手引导", size: 24, color: "666666" })] }),
      new Paragraph({ alignment: AlignmentType.CENTER, spacing: { after: 80 },
        children: [new TextRun({ text: "技术栈：C++17 + 原生HTML/CSS/JS + MySQL 8.0", size: 24, color: "666666" })] }),
      new Paragraph({ children: [new PageBreak()] }),

      // ─────────────────────────────────────────────
      // 目录
      // ─────────────────────────────────────────────
      H1("目录"),
      Bullet("项目概述与定位"),
      Bullet("技术架构"),
      Bullet("数据库设计"),
      Bullet("已有功能详解（v1.3.0）"),
      Bullet("正在开发功能（v1.4.0）"),
      Bullet("待开发功能规划"),
      Bullet("AI 系统"),
      Bullet("前端页面说明"),
      Bullet("测试体系"),
      Bullet("部署与运维"),
      Empty(),

      // ─────────────────────────────────────────────
      // 一、项目概述
      // ─────────────────────────────────────────────
      H1("一、项目概述"),
      H2("1.1 产品定位"),
      P("本地轻量化英语学习软件，所有学习数据 100% 存储在本地 MySQL，AI 能力通过本地网关转发到智谱 GLM-4-Flash（免费），学习数据不上云。用户通过浏览器访问 http://127.0.0.1:8088 使用所有功能。"),
      H2("1.2 核心学习流程"),
      P("知识点录入 → AI 固化生成六块知识 → 遗忘曲线调度复习 → 智能出题练习 → 学情反馈 → 每日小测"),
      P("遗忘曲线公式：", [{ text: "周复习", bold: true }, " 2×(1+2^熟练度) 天；", { text: "日复习", bold: true }, " 0.4×(1+3^熟练度) 天"]),
      H2("1.3 核心价值"),
      Bullet("零数据上云：所有学情、单词本、文章数据本地存储"),
      Bullet("知识点资产沉淀：固化后的知识可被他人复用，形成共享知识池"),
      Bullet("遗忘曲线专用算法：针对词汇记忆场景调优，非通用背诵系统"),
      Bullet("游戏化激励闭环：金桂货币 + 朱砂印 + 称谓等级三重驱动"),
      Bullet("轻量部署：C++ 单体二进制，无需 Python/Node 运行环境"),

      // ─────────────────────────────────────────────
      // 二、技术架构
      // ─────────────────────────────────────────────
      H1("二、技术架构"),
      H2("2.1 技术栈"),
      new Table({
        width: { size: 9360, type: WidthType.DXA },
        columnWidths: [2400, 1800, 5160],
        rows: [
          tr(["层级", "技术", "说明"], true),
          tr(["后端", "C++17（Apple clang）", "手写 HTTP 服务器（嵌入 Crow 源码），mysql-8.0 C API"]),
          tr(["网关", "Node.js v20 LTS", "本地代理转发 HTTPS 请求到智谱 AI BigModel，端口 5340"]),
          tr(["数据库", "MySQL 8.0", "本地单机，InnoDB，utf8mb4，不设外键（代码保障一致性）"]),
          tr(["前端", "原生 HTML + CSS + JS", "无框架、无构建工具，后端托管静态文件"]),
          tr(["TTS", "Web Speech API", "浏览器原生文字转语音，无需额外服务"]),
          tr(["Word 生成", "Node.js docx 库", "本地离线生成 .docx，60 分钟后自动清理临时文件"]),
          tr(["字体", "Fraunces + Karla", "woff2 格式离线可用，存放于 frontend/static/fonts/"]),
        ]
      }),
      Empty(),
      H2("2.2 目录结构"),
      P("以下为主要目录和文件（精简版）："),
      Bullet("backend/src/main.cpp — 路由注册 + 主入口（约 1300 行）"),
      Bullet("backend/include/*.h — 服务层头文件（db.h / knowledge_service.h / question_service.h / learning_service.h / quiz_service.h / articles_service.h / ai_client.h 等）"),
      Bullet("frontend/pages/*.html — 9 个功能页面"),
      Bullet("frontend/static/style.css — 全局样式（:root 设计令牌集中定义）"),
      Bullet("frontend/static/kp-detail.js — 知识点详情页交互逻辑"),
      Bullet("scripts/sql/*.sql — 数据库建表/迁移脚本（按序号执行）"),
      Bullet("scripts/gen_word_kp.js / gen_word_exercise.js — Word 生成脚本"),
      Bullet("tests/stage*.test.js — 全量回归测试套件（13 套，约 350 项）"),
      Bullet("config/app.json / config/db.json / config/ai.json / config/rewards.json — 配置文件"),
      Empty(),
      H2("2.3 配置文件说明"),
      new Table({
        width: { size: 9360, type: WidthType.DXA },
        columnWidths: [2400, 1800, 5160],
        rows: [
          tr(["文件", "用途", "需关注字段"], true),
          tr(["config/app.json", "应用主配置", "backend_port（8088）、version"]),
          tr(["config/db.json", "数据库连接", "database、socket 路径；密码通过 password_file 引用外部文件"]),
          tr(["config/ai.json", "AI 网关", "base_url（http://127.0.0.1:5340/v1）、chat_model（glm-4-flash）、api_key"]),
          tr(["config/rewards.json", "点金经济参数", "income 收入项、expense 支出项、seals 印章阈值、titles 称谓等级"]),
        ]
      }),
      Empty(),

      // ─────────────────────────────────────────────
      // 三、数据库设计
      // ─────────────────────────────────────────────
      H1("三、数据库设计"),
      H2("3.1 知识点模块（6 张表）"),
      H3("knowledge_points — 知识点主表"),
      new Table({
        width: { size: 9360, type: WidthType.DXA },
        columnWidths: [2200, 1400, 5760],
        rows: [
          tr(["字段名", "类型", "说明"], true),
          tr(["id", "INT PK", "主键，自增"]),
          tr(["user_id", "INT", "用户ID（默认1=游客）"]),
          tr(["user_input_content", "VARCHAR(255)", "用户原始录入内容"]),
          tr(["standard_content", "VARCHAR(255)", "AI固化后的最简标准词（查重依据）"]),
          tr(["difficulty", "TINYINT", "难度等级 1-5"]),
          tr(["master_require", "VARCHAR(20)", "掌握要求：会读 / 会写"]),
          tr(["remark", "VARCHAR(255)", "用户备注"]),
          tr(["is_solidified", "TINYINT(1)", "是否固化（0=未固化，1=已固化）"]),
          tr(["create_time / solidify_time", "DATETIME", "录入时间 / AI固化完成时间"]),
        ]
      }),
      Empty(),
      H3("kp_definitions — 核心含义与例句"),
      new Table({
        width: { size: 9360, type: WidthType.DXA },
        columnWidths: [2200, 1400, 5760],
        rows: [
          tr(["字段", "类型", "说明"], true),
          tr(["id", "INT PK", "主键"]),
          tr(["knowledge_id", "INT", "关联主表ID"]),
          tr(["pos", "VARCHAR(20)", "词性（n./v./adj./adv.）"]),
          tr(["meaning", "VARCHAR(500)", "英文核心释义"]),
          tr(["example", "VARCHAR(500)", "配套经典例句"]),
          tr(["display_order", "INT", "展示顺序"]),
        ]
      }),
      Empty(),
      H3("kp_collocations — 固定搭配"),
      P("字段：id / knowledge_id / collocation（固定搭配）/ example（例句）/ display_order"),
      Empty(),
      H3("kp_synonyms — 同源/近义词"),
      P("字段：id / knowledge_id / synonym（同源词/近义词）/ usage_diff（用法差异）/ display_order"),
      Empty(),
      H3("kp_antonyms — 反义词"),
      P("字段：id / knowledge_id / antonym / display_order"),
      Empty(),
      H3("kp_extras — 扩展知识（词根/典故/语域等）"),
      P("字段：id / knowledge_id / extra_type / content / display_order"),
      P("extra_type 枚举值：root / etymology / allusion / key_points / register / frequency / cultural / phrase_analysis / markdown_content"),
      P("注意：v1.4.0-T010 将新增 markdown_content 类型，将多块讲解合并为单个 Markdown 文档存储。"),
      Empty(),
      H2("3.2 学情模块（2 张表）"),
      H3("user_learning — 用户学情记录"),
      P("核心字段：knowledge_id / proficiency_level（熟练度 0-5）/ review_times / error_times / review_mode（daily/weekly）/ next_review_time（下次复习时间）"),
      H3("review_logs — 复习日志"),
      P("记录每次答题：user_id / knowledge_id / exercise_type / is_correct / score / review_time"),
      Empty(),
      H2("3.3 文章模块（5 张表）"),
      H3("articles — 文章主表"),
      P("核心字段：title / content（英文原文，<p>标签分隔）/ content_cn（中文译文）/ cefr_level（A1/A2/B1/B2/C1）/ import_method / ai_analysis（JSON）/ read_at"),
      H3("article_sentences — 分句表"),
      P("字段：article_id / sentence_no / sentence_en / sentence_cn / cefr_level / analysis_json（长难句拆解缓存）"),
      H3("article_vocabulary — 词汇难度表"),
      P("字段：article_id / word / position / occurrence / cefr_level / is_new_word / user_id"),
      H3("article_quizzes — 阅读理解题"),
      P("字段：article_id / question_no / question_type / question_text / options（JSON）/ correct_answer / explanation / cefr_level / created_by（ai/manual）"),
      H3("article_recommend_queue — AI推荐队列"),
      P("字段：user_id / topic / cefr_level / source_url / title / summary / status（pending/fetched/processed/failed）"),
      Empty(),
      H2("3.4 激励模块（4 张表）"),
      P("treasury 前缀表：user_coin（金桂余额+累计消耗）/ user_coin_txn（流水账）/ user_seal（朱砂印收藏）/ user_title（称谓等级）/ user_weekly_goal（本周课规）"),
      P("朱砂印 seal_code 枚举：mastery_100 / mastery_500 / mastery_1000 / goal_done / streak_3 / streak_10 / streak_50"),
      P("称谓 title_code 枚举：scholar_1（书童）/ scholar_2（秀才）/ scholar_3（举人）/ scholar_4（进士）/ scholar_5（状元）"),
      Empty(),
      H2("3.5 认证模块（2 张表）"),
      P("users（id/username/password_hash/learner_identity/role/create_time）/ user_sessions（token/session_data/expiry）"),
      P("learner_identity 注入 5 套 AI 提示词模板，影响出题风格和知识点难度。"),
      Empty(),
      H2("3.6 AI 任务表"),
      P("ai_tasks（task_type/ref_id/status/payload/result/error_msg/retry_times）— 用于异常兜底和重试调度。"),
      Empty(),

      // ─────────────────────────────────────────────
      // 四、已有功能详解
      // ─────────────────────────────────────────────
      H1("四、已有功能详解（v1.3.0）"),
      H2("4.1 知识点录入与固化"),
      P("功能路径：/pages/kp-entry.html → POST /api/kp → POST /api/kp/solidify"),
      H3("业务流程"),
      Bullet("用户在输入框输入单词/短语"),
      Bullet("调用 POST /api/kp 写入 knowledge_points 主表（is_solidified=0）"),
      Bullet("用户点击\u201c固化\u201d按钮，触发 POST /api/kp/solidify"),
      Bullet("KnowledgeService::solidifyOne 调用 AI 生成 JSON，写入 5 张子表"),
      Bullet("is_solidified 置 1，solidify_time 记录时间"),
      H3("AI 输出内容（六块）"),
      Bullet("核心含义：英文核心释义 + 经典例句"),
      Bullet("词根词源：词根拆解与词源追溯"),
      Bullet("同源词汇：同根/同词族词汇"),
      Bullet("固定搭配：常见搭配 + 例句"),
      Bullet("同义词及辨析：近义词列表 + 用法差异"),
      Bullet("补充知识：使用频率、语域、文化延伸、典故俗语"),
      H3("批量固化"),
      P("POST /api/kp/solidify/batch — 一次传入多个知识点 ID，AI 返回 JSON 数组，逐条 applySolidify。"),
      Empty(),
      H2("4.2 知识点详情"),
      P("功能路径：/pages/kp-detail.html → GET /api/kp/:id/detail"),
      P("返回完整的知识点 JSON（含 definitions/collocations/synonyms/antonyms/extras 数组），前端按卡片分组渲染。"),
      P("支持：编辑（updateStandardContent）、合并（merge）、拆分（split）、删除。"),
      Empty(),
      H2("4.3 学情与复习"),
      P("功能路径：/pages/review.html → GET /api/learn/candidates → POST /api/learn/:id/answer"),
      H3("核心算法"),
      Bullet("按周模式：next_review_time = solidify_time + 2×(1+2^proficiency) 天"),
      Bullet("按日模式：next_review_time = solidify_time + 0.4×(1+3^proficiency) 天"),
      Bullet("candidates 查询：WHERE is_active=1 AND next_review_time <= NOW() ORDER BY next_review_time ASC"),
      Bullet("答对：proficiency += 1；答错：proficiency -= 1（最低 0）"),
      H3("出题与批改"),
      P("QuestionService::generateExercises：实时调用 AI 生成习题（JSON），不入库"),
      P("QuestionService::batchGrade：客观题本地秒批（字符串比较），主观题调用 AI 批改"),
      P("页数限制公式：按周 max(3,min(5,ceil(kp_count×1.0)))；按日 max(1,min(2,ceil(kp_count×0.5)))"),
      Empty(),
      H2("4.4 六大题型"),
      new Table({
        width: { size: 9360, type: WidthType.DXA },
        columnWidths: [2400, 2400, 4560],
        rows: [
          tr(["题型标识", "名称", "说明"], true),
          tr(["word_match", "词义匹配综合题", "提供短文 passage，考查目标词在语境中的含义"]),
          tr(["word_form_fill", "词形填空", "给出词根，要求填写正确词形（如 teach→taught）"]),
          tr(["context_blank_fill", "语境选词填空", "挖空句 + 四选一选项"]),
          tr(["cn_to_en_translation", "中译英翻译", "给出中文，要求写出英文（主观题，AI 批改）"]),
          tr(["cloze_choice", "词汇辨析完形", "短文挖空 + 四选项，考查同义/近义辨析"]),
          tr(["sentence_imitation", "例句仿写", "给出例句结构，要求仿写（主观题，AI 批改）"]),
        ]
      }),
      Empty(),
      H2("4.5 每日小测"),
      P("功能路径：/pages/daily-quiz.html → POST /api/quiz/daily → POST /api/quiz/daily/submit"),
      H3("题目结构（3+1+1）"),
      Bullet("第 1-3 题（category=solidified）：从已固化知识点随机抽取"),
      Bullet("第 4 题（category=familiar_meaning）：熟词生义，考查一词多义中不常见义项"),
      Bullet("第 5 题（category=new_kp）：新知拓展，自选一个陌生常用词"),
      H3("答题与报告"),
      P("所有题目均为客观选择题，本地秒批。提交后生成 Word 报告（scripts/gen_word_kp.js），包含答题记录、得分、错题库。"),
      Empty(),
      H2("4.6 单词本"),
      P("功能路径：/pages/wordbook.html → GET/POST /api/wordbook/*"),
      P("功能：生词收集（导入 / 手动添加）/ 已掌握标记 / 导出 Word。"),
      Empty(),
      H2("4.7 播放复习"),
      P("功能路径：/pages/review.html → 播放按钮 → Web Speech API TTS"),
      P("按复习队列逐句朗读，含进度控制（暂停/下一句/重播）。"),
      P("tts.js：封装 SpeechSynthesisUtterance，支持设置 lang/voice/rate。"),
      Empty(),
      H2("4.8 点金圣殿经济体系"),
      P("功能路径：首页仪表盘右侧显示金桂余额和称谓等级"),
      H3("收入来源（金桂入账）"),
      Bullet("quiz_done：完成每日小测"),
      Bullet("quiz_perfect：满分完成小测"),
      Bullet("review_done：完成一轮复习"),
      Bullet("goal_done：达成周课规目标"),
      Bullet("streak_bonus：连续学习奖励（3/10/50天）"),
      H3("支出项目（金桂扣费）"),
      Bullet("solidify_cost：AI 固化扣费"),
      Bullet("lookup_spend：点词查义扣费"),
      Bullet("wordbook_spend：导出单词本扣费"),
      H3("朱砂印（7 枚，纯荣誉）"),
      P("mastery_100 / mastery_500 / mastery_1000 / goal_done / streak_3 / streak_10 / streak_50"),
      H3("称谓等级（5 级）"),
      P("scholar_1（书童）→ scholar_2（秀才）→ scholar_3（举人）→ scholar_4（进士）→ scholar_5（状元），按金桂累计消耗和印章数量升级。"),
      Empty(),
      H2("4.9 多用户认证"),
      P("POST /api/auth/login → POST /api/auth/register → GET /api/auth/me"),
      P("游客（user_id=1）永久保留，注册用户 ID≥2。注销时级联删除全部关联数据。"),
      P("thread_local RequestContext::currentUserId() 隔离跨用户上下文。"),
      Empty(),
      H2("4.10 文章模块阶段 1"),
      P("功能路径：/pages/articles.html → 7 条文章 CRUD API"),
      Bullet("内置 EFLLex 13,546 词条（A1~C1）作兜底推荐词库"),
      Bullet("文章 CRUD：POST /api/article（创建）/ GET /api/article/list（列表）/ GET /api/article/:id（详情）/ PUT /api/article/:id（更新）/ DELETE /api/article/:id"),
      Bullet("CEFR 难度分级：A1/A2/B1/B2/C1"),
      Bullet("规则分句：按标点切分英文段落为 sentence"),
      Empty(),

      // ─────────────────────────────────────────────
      // 五、正在开发功能
      // ─────────────────────────────────────────────
      H1("五、正在开发功能（v1.4.0）"),
      H2("5.1 文章阅读核心功能（阶段 2，进行中）"),
      H3("功能范围"),
      Bullet("分句展示：逐句显示文章，高亮当前朗读句"),
      Bullet("点词查义：点击文中单词，弹出释义浮层（查 kp_definitions）"),
      Bullet("生词高亮：is_new_word=1 的词汇以不同颜色标注"),
      Bullet("TTS 朗读：点击句子播放发音"),
      Bullet("中英对照：content_cn 字段展示中文译文"),
      H3("涉及文件"),
      Bullet("frontend/pages/articles.html — 阅读界面重构"),
      Bullet("frontend/static/articles.js — 阅读交互逻辑（新增）"),
      Bullet("backend/include/articles_service.h — splitAndStoreSentences / analyzeAndStoreVocabulary"),
      Empty(),
      H2("5.2 知识点固化 Markdown 重构（v1.4.0-T010，计划中）"),
      H3("背景"),
      P("当前 AI 固化输出硬编码 JSON schema（KP_JSON_SCHEMA），字段死板、容错性差。新方案将讲解类内容改为 Markdown 格式存储。"),
      H3("变更范围"),
      new Table({
        width: { size: 9360, type: WidthType.DXA },
        columnWidths: [2800, 6560],
        rows: [
          tr(["改动", "说明"], true),
          tr(["backend/include/prompts.h", "新增 KP_HYBRID_JSON_SCHEMA + KP_MARKDOWN_RULES"], false),
          tr(["backend/include/knowledge_service.h", "applySolidify 解析 markdown_content 字段，合并为单条 extras 记录"], false),
          tr(["frontend/pages/kp-detail.html", "引入 marked.js CDN，extras 渲染增加 Markdown 分支"], false),
          tr(["tests/stage2_kp_solidify.test.js", "新增 markdown_content 存在性断言"], false),
          tr(["frontend/static/style.css", "Markdown 排版样式（标题/列表/代码块/引用）"], false),
        ]
      }),
      P("kp_definitions / kp_collocations / kp_synonyms / kp_antonyms 仍保持结构化子表不变，仅其他讲解类内容改为 Markdown。"),
      Empty(),
      H2("5.3 文章背景知识与推荐文章（阶段 2 依赖）"),
      P("基于已阅读文章和用户学情，AI 推荐适配 CEFR 难度的延伸文章。"),
      P("涉及表：article_recommend_queue。"),
      Empty(),
      H2("5.4 生词本联动（阶段 2 依赖）"),
      P("阅读文章中遇到的生词，一键加入单词本（wordbook_entries），并与知识点库联动显示释义。"),
      Empty(),

      // ─────────────────────────────────────────────
      // 六、待开发功能规划
      // ─────────────────────────────────────────────
      H1("六、待开发功能规划"),
      H2("6.1 文章图片导入 + AI 生成（P2）"),
      P("用户上传文章截图/照片，AI 提取文本并自动导入为文章记录。"),
      P("涉及表：article_import_records。"),
      Empty(),
      H2("6.2 文章阅读理解题预生成（T010 之后）"),
      P("文章导入时同步调用 AI 生成 article_quizzes，避免阅读时实时等待。"),
      Empty(),
      H2("6.3 点词查义功能（P1，文章阶段 2 内）"),
      P("阅读文章中点击单词，弹出释义浮层（查 kp_definitions + kp_extras）。需要网络。"),
      Empty(),
      H2("6.4 暗色主题（P3）"),
      P("在 style.css 的 :root 中新增深色设计令牌，前端通过 data-theme 属性切换。设计稿已有方案。"),
      Empty(),
      H2("6.5 前端个人中心身份修改入口（P2）"),
      P("后端 PUT /api/user/me/profile API 已就绪，前端 pages/learning-center.html 尚未接入。"),
      Empty(),
      H2("6.6 单词本共享复用（远期）"),
      P("优质词条标记 is_public=1，其他用户导入时优先匹配 public 词条，避免重复调用 AI。"),
      Empty(),

      // ─────────────────────────────────────────────
      // 七、AI 系统
      // ─────────────────────────────────────────────
      H1("七、AI 系统"),
      H2("7.1 AI 网关架构"),
      P("Node.js 本地服务（scripts/local_ai_server.js），监听端口 5340，作为 OpenAI 兼容接口代理。"),
      P("config/ai.json 配置：{ \"base_url\": \"http://127.0.0.1:5340/v1\", \"chat_model\": \"glm-4-flash\", \"api_key\": \"zlp-...\" }"),
      H2("7.2 提示词体系（prompts.h）"),
      new Table({
        width: { size: 9360, type: WidthType.DXA },
        columnWidths: [2800, 6560],
        rows: [
          tr(["常量", "用途"], true),
          tr(["KP_SOLIDIFY_SYSTEM", "知识点固化 system prompt，约束只输出 JSON"], false),
          tr(["KP_SOLIDIFY_ONE_USER", "单词固化 user prompt，含六板块说明 + JSON schema"], false),
          tr(["KP_SOLIDIFY_BATCH_USER", "批量固化 user prompt，输出 JSON 数组"], false),
          tr(["EXERCISE_GENERATE_SYSTEM", "复习习题生成 system prompt"], false),
          tr(["EXERCISE_GENERATE_USER", "习题生成 user prompt，含 kp_json / 模式 / 页数限制"], false),
          tr(["SUBJECTIVE_GRADE_SYSTEM", "主观题批改 system prompt"], false),
          tr(["SUBJECTIVE_GRADE_USER", "主观题批改 user prompt，输入用户答案和参考答案"], false),
          tr(["DAILY_QUIZ_SYSTEM", "每日小测 system prompt"], false),
          tr(["DAILY_QUIZ_USER", "小测生成 user prompt，3+1+1 结构硬性约束"], false),
        ]
      }),
      Empty(),
      H2("7.3 学习者身份注入"),
      P("learner_identities 字典表存储身份枚举：高中生 / 大学生 / CET-4 / CET-6 / 考研 / 雅思 / 托福 / GRE。"),
      P("AI 生成时动态替换 {LEARNER_IDENTITY} 占位符，影响出题难度和讲解侧重方向。"),
      H2("7.4 AI 重试机制（T009 已完成）"),
      P("AiClient::postChat 内置指数退避重试（base×2^(attempt-1)），最多 retry_times 次。"),
      P("失败时写入 ai_tasks 表，status=failed，供后续人工或定时任务重试。"),
      Empty(),
      H2("7.5 Word 生成脚本"),
      new Table({
        width: { size: 9360, type: WidthType.DXA },
        columnWidths: [2800, 6560],
        rows: [
          tr(["脚本", "用途"], true),
          tr(["scripts/gen_word_kp.js batch <json_path> <tmp_dir>", "知识点批量导出 Word（多词条合并）"], false),
          tr(["scripts/gen_word_exercise.js <ex_id> <json_path> <tmp_dir>", "习题 Word 生成（多题型混合排版）"], false),
          tr(["scripts/gen_word_daily.js", "每日小测 Word 报告"], false),
        ]
      }),
      P("临时文件存放于 word_tmp/ 目录，由 scripts/scheduler_cleanup.js 每 60 分钟清理。"),
      Empty(),

      // ─────────────────────────────────────────────
      // 八、前端页面说明
      // ─────────────────────────────────────────────
      H1("八、前端页面说明"),
      new Table({
        width: { size: 9360, type: WidthType.DXA },
        columnWidths: [2800, 2000, 4560],
        rows: [
          tr(["页面", "路径", "功能说明"], true),
          tr(["仪表盘", "/（index.html）", "今日待办、金桂余额、称谓等级、周进度、快速入口导航"], false),
          tr(["知识点录入", "/pages/kp-entry.html", "输入单词/短语，触发 AI 固化；支持批量录入"], false),
          tr(["知识点详情", "/pages/kp-detail.html", "查看六块知识内容，支持编辑/合并/拆分/删除"], false),
          tr(["每日复习", "/pages/review.html", "按遗忘曲线调度做题；含播放 TTS 功能"], false),
          tr(["每日小测", "/pages/daily-quiz.html", "3+1+1 结构化小测，提交后下载 Word 报告"], false),
          tr(["单词本", "/pages/wordbook.html", "生词收集/已掌握标记/批量导入/导出 Word"], false),
          tr(["学情中心", "/pages/learning-center.html", "熟练度统计/复习日历/错题分析/身份设置"], false),
          tr(["文章阅读", "/pages/articles.html", "文章库浏览/阅读练习（v1.4.0 开发中）"], false),
          tr(["登录/注册", "/pages/login.html", "用户认证；游客无需登录可使用基础功能"], false),
        ]
      }),
      Empty(),
      H2("8.1 关键前端模块"),
      H3("style.css"),
      P("全局样式，所有设计令牌（颜色/圆角/阴影/字体）集中在 :root 伪类中定义，便于统一修改和主题切换。"),
      H3("nav.js"),
      P("全局导航组件，注入登录态（用户头像/称谓等级），渲染侧边栏菜单。"),
      H3("theme.js"),
      P("深色/浅色主题切换，通过 <html data-theme=\u201cdark\u201d> 属性控制。"),
      H3("tts.js"),
      P("Web Speech API 封装，提供 playText(text, lang) 和 stop() 方法。"),
      H3("kp-detail.js"),
      P("知识点详情页交互：加载详情、渲染各板块卡片、处理合并/拆分操作。"),
      Empty(),

      // ─────────────────────────────────────────────
      // 九、测试体系
      // ─────────────────────────────────────────────
      H1("九、测试体系"),
      P("全量回归套件位于 tests/ 目录，使用 Node.js 编写，通过 curl 调用后端 API，输出 ✓/✗/~ 三种状态。"),
      H2("9.1 测试套件列表"),
      new Table({
        width: { size: 9360, type: WidthType.DXA },
        columnWidths: [2600, 2600, 4160],
        rows: [
          tr(["套件文件", "覆盖功能", "项数"], true),
          tr(["stage1_kp.test.js", "知识点 CRUD / 查重 / 合并 / 拆分", "~31"], false),
          tr(["stage2_kp_solidify.test.js", "AI 固化流程 / 子表写入 / extras 结构", "~30"], false),
          tr(["stage3_review.test.js", "复习候选查询 / 熟练度更新", "~26"], false),
          tr(["stage4_question.test.js", "出题 / 批改 / 题型 / 页数限制", "~30"], false),
          tr(["stage5_frontend.test.js", "页面 HTML 结构 / 静态资源存在性", "~28"], false),
          tr(["stage6_maint.test.js", "备份 / 清理 / 配置校验", "~16"], false),
          tr(["stage7_wordbook.test.js", "单词本 CRUD / 导出", "~28"], false),
          tr(["stage8_ai_retry.test.js", "AI 失败重试 / ai_tasks 记录", "~9"], false),
          tr(["stage9_auth.test.js", "登录 / 注册 / session / 注销级联删除", "~41"], false),
          tr(["stage10_playback.test.js", "TTS 播放队列 / 进度控制", "~14"], false),
          tr(["stage11_daily_quiz.test.js", "3+1+1 小测生成 / 批改 / Word 报告", "~37"], false),
          tr(["stage12_treasury.test.js", "金桂入账/扣费 / 印章 / 称谓 / 课规", "~55"], false),
          tr(["stage13_articles.test.js", "文章 CRUD / CEFR 分级 / 分句", "待补充"], false),
        ]
      }),
      Empty(),
      H2("9.2 运行方式"),
      P("全量：node frontend/tests/run_all.js"),
      P("单个套件：node frontend/tests/stage2_kp_solidify.test.js"),
      P("输出说明：✓ 通过 / ✗ 失败 / ~ 跳过（网络依赖或条件不满足）"),
      Empty(),

      // ─────────────────────────────────────────────
      // 十、部署与运维
      // ─────────────────────────────────────────────
      H1("十、部署与运维"),
      H2("10.1 一键启动"),
      P("bash scripts/start_all.sh — 依次启动 MySQL → AI 网关 → C++ 后端"),
      P("或双击桌面 start_all.command"),
      H2("10.2 手动启动"),
      Bullet("bash scripts/start_mysql.sh（MySQL socket: ~/.local/run/mysql.sock）"),
      Bullet("node scripts/local_ai_server.js（AI 网关端口 5340）"),
      Bullet("./backend/build/eng_app_server（后端端口 8088）"),
      H2("10.3 停止服务"),
      P("bash scripts/stop_all.sh"),
      H2("10.4 编译后端"),
      P("bash scripts/build_backend.sh（需 cmake 和 make）"),
      H2("10.5 数据库备份"),
      P("POST /api/maint/backup — 触发备份，文件存于 backups/ 目录"),
      P("启动时自动备份（scripts/start_mysql.sh 内嵌）"),
      H2("10.6 常见问题"),
      Bullet("AI 调用失败：检查 config/ai.json 中 api_key 是否正确（需从 https://open.bigmodel.cn 注册获取）"),
      Bullet("数据库连接失败：确认 MySQL 运行中，密码文件 ~/.local/mysql/.root_pw 存在"),
      Bullet("全量测试失败：AI 相关失败（~）通常为网络抖动，可重跑"),
      Empty(),

      // ─────────────────────────────────────────────
      // 十一、版本变更记录
      // ─────────────────────────────────────────────
      H1("十一、版本变更记录"),
      new Table({
        width: { size: 9360, type: WidthType.DXA },
        columnWidths: [1600, 1200, 6560],
        rows: [
          tr(["版本", "类型", "变更记录"], true),
          tr(["v1.3.0", "MINOR", "UI 颠覆性重设计；文章模块阶段 1（CRUD+CEFR）；点金圣殿经济体系；播放器复习；每日小测 3+1+1 结构"], false),
          tr(["v1.2.0", "PATCH", "每日小测独立功能；播放复习 TTS 集成"], false),
          tr(["v1.1.0", "PATCH", "多用户认证；学习者身份注入；AI 重试机制"], false),
          tr(["v1.0.0", "MINOR", "基础框架：知识点录入/固化/详情；遗忘曲线复习算法；六题型出题；单词本"], false),
          tr(["v1.4.0", "MINOR（进行中）", "文章阅读核心功能（分句/点词查义/生词高亮）；知识点固化 Markdown 重构（T010）"], false),
        ]
      }),
      Empty(),
      H2("11.1 版本号规范"),
      P("格式：vMAJOR.MINOR.PATCH-T{序号}"),
      Bullet("MAJOR：破坏性变更（删表/删接口）"),
      Bullet("MINOR：新增功能（加表/加接口）"),
      Bullet("PATCH：缺陷修复"),
      Bullet("T 序号：版本号内任务序号，从 T001 起递增"),
      Empty(),
      H2("11.2 开发规范"),
      Bullet("需求不明先问，不臆测"),
      Bullet("先计划后开发，先设计后编码"),
      Bullet("每次回答必须读取 documents/skills/ 下对应工作手册"),
      Bullet("所有结论必须基于测试结果，禁止伪造功能"),
      Bullet("每轮只完成一个任务（用户明确豁免的除外）"),
      Empty(),
    ]
  }]
});

Packer.toBuffer(doc).then(buffer => {
  const outPath = "/Users/junwang/Desktop/王浩源/英语/英语学习应用/documents/英语学习应用_功能设计说明书_v1.4.0.docx";
  fs.writeFileSync(outPath, buffer);
  console.log("文档已生成：" + outPath);
}).catch(err => {
  console.error("生成失败：", err);
  process.exit(1);
});
