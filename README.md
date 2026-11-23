# 轻量化本地英语学习软件（Learning App）

> GitHub：[Whycmd/learning-app](https://github.com/Whycmd/learning-app)
> 贡献指南：[CONTRIBUTING.md](CONTRIBUTING.md) ｜ 许可证：[MIT](LICENSE)
> 当前版本：v1.5.3（v1.5.2 批次已收口：开发者看板与反馈沉淀；v1.5.1 批次已收口：课规与本周任务卡、错题归因与重练、学情周报、首页今日概览与全站 AI 向导；v1.5.0 功能批次已收口：匿名会话、AI 分层队列、词表库、AI 讲解/定制练习、导出中心、运维基线）

## 项目定位

基于遗忘曲线的英语知识点学习工具：输入英文单词，AI 生成结构化精讲并固化入库，按遗忘曲线安排复习，配合 AI 出题、批改、每日小测与单词本。

学习数据 100% 存在本机 MySQL，AI 能力通过本地网关调用智谱 GLM-4-Flash（免费），用户数据不落第三方。

## 功能（截至 v1.5.3）

| 模块 | 能力 |
|------|------|
| 知识点系统 | 新词录入 → AI 生成六块结构化知识 → 固化入库；Markdown 精讲本地渲染；单个/批量 Word 导出（含 LaTeX 公式） |
| 异步批量固化 | `POST /api/kp/solidify/async` 立即返回任务 ID；分片并发、进度轮询、任务历史、断点续跑 |
| AI 分层队列 | L1 同词按身份复用秒回；L2 排队并返回队列位置与预计等待；失败退避重试、任务超时保留已完成子项；主备模型自动切换 |
| 智能复习 | 遗忘曲线双模式（按周 / 按日）；熟练度越高间隔越长 |
| AI 出题与批改 | 6 种题型；客观题本地秒批，不改遗忘曲线 |
| 每日小测 | 3 题已固化知识 + 1 题熟词生义 + 1 题新知，本地批改并生成 Word 报告 |
| 课规与本周任务 | 课规 CRUD（同时仅 1 个正在执行、到期自动延长）；每周 3 条可判定指标任务，目标可改、完成发放金桂，单次学习事件只计入一个课规 |
| 错题本与重练 | 答错即按拼写/词义/搭配/语法归因（规则先判、AI 异步补充）；错题按词聚合、按错因折叠；五项操作 + 批量加入导出；重练自动排期、连续 3 次正确或 90 天未错自动归档，完成 +5 金桂（日上限 10） |
| 学情周报 | 学情中心「概览 / 学情周报」页签；默认上周、可切换近 4 周；课规完成率、7 天复习趋势、错题分布（按词去重、按错因，点错因直达错题本）、词表进度开关、规则建议与 AI 学习教练建议；首页仅上周有记录时提示 |
| 单词本 | 收集生词、导入导出、标记已掌握、进入复习队列 |
| 播放复习 | Web Speech API 逐句朗读复习队列 |
| 点金圣殿 | 行为积分 → 金桂货币 → 朱砂印 / 称谓等级解锁 |
| 多用户 | 账号登录、会话隔离、学习者身份注入（AI 讲解据此调难度） |
| 文章库（阶段 1） | CEFR 分级文章 CRUD、规则分句与难度分析；阶段 2（点词查义、生词高亮、中英对照）待排期 |

## 技术栈

- **后端**：C++17 + 自研轻量 HTTP 服务器（header-only），端口 `8088`
- **数据库**：MySQL 8.0 本机单机，库名 `english_learning`
- **运行时**：Node.js 20（AI 网关、Word 生成、定时清理）
- **AI**：本地 Node 网关（端口 `5340`）转发智谱 BigModel（`glm-4-flash`）
- **前端**：原生 HTML + CSS + JavaScript，无框架、无构建工具
- **字体**：Fraunces / Karla，woff2 离线可用

## 目录结构

```
英语学习应用/
├── backend/            C++ 后端（src/ 源文件、include/ 服务层、build/ 产物）
├── frontend/           原生前端（index.html、pages/ 12 个页面、static/ 样式与脚本）
├── scripts/            启动/停止/构建、AI 网关、Word 生成、定时清理、SQL 初始化
│   └── sql/            建表脚本 19 个（初始化用；编号 01–20，缺 07）
├── tests/              全量回归测试 37 个套件
├── config/             配置（数据库、AI、课规指标、奖励、错题规则、周报规则、日志、运维）
├── documents/          产品与迭代文档（含 skills/ 执行手册、devlog/）
├── backups/            数据库备份
├── logs/               运行日志
├── word_tmp/           Word 临时目录（自动清理）
├── docker-compose.prod.yml 生产编排（80 端口、重启自愈、日志限额）
├── .env.prod.example   生产环境变量模板（不入库）
├── start_all.command   一键启动（macOS）
└── scripts/stop_all.sh 一键停止
```

## 快速上手（./dev）

所有常用操作收敛到一个入口，不必记多个脚本名：

| 命令 | 作用 |
|---|---|
| `./dev up` | 启动 Docker 容器（默认方式） |
| `./dev up --host` | 启动本机 MySQL + AI 网关 + 后端 |
| `./dev down` | 停止全部服务 |
| `./dev debug` | 调试模式：数据库与网关后台运行，后端日志直接打屏 |
| `./dev logs` | 聚合查看日志（自动判断容器模式 / 本机模式） |
| `./dev doctor` | 环境自检：编译器、Node、配置、AI Key、数据库、端口、Docker |
| `./dev test` | 全量回归（37 套件） |
| `./dev test 9` | 只跑 stage9（秒级反馈） |

`./dev doctor` 能提前发现常见故障，例如 Command Line Tools 损坏、AI Key 含中文、端口被占用。

## 运行方式

### 方式一：Docker 一键启动（推荐，三平台通用）

需要先安装 Docker Desktop（首次启动可跳过登录）。

```bash
git clone https://github.com/Whycmd/learning-app.git
cd learning-app
cp .env.example .env        # 填入 ZHIPU_API_KEY；留空也能启动，只是 AI 功能不可用
docker compose up -d        # 首次约 3-5 分钟（建库建表 + 拉镜像）
```

打开 http://127.0.0.1:8088

数据保存在 Docker 卷 `mysql_data`，`docker compose down` 不会丢失；`down -v` 才会清空。

### 方式二：macOS 本地运行

> 说明：脚本与 `config/db.json` 目前仍以作者本机的 `~/.local/...` 为默认路径，
> 可通过环境变量 `APP_RUN_DIR`、`APP_LOG_DIR`、`MYSQL_HOME` 覆盖。

**前置**：macOS + Xcode Command Line Tools（Apple clang）、Node.js 20、MySQL 8.0、智谱 API Key。

```bash
git clone https://github.com/Whycmd/learning-app.git
cd learning-app

# 1. 配置 AI Key（免费申请：https://open.bigmodel.cn）
cp config/ai.example.json config/ai.json   # 保持 api_key 留空
bash scripts/ai_key_store.sh set           # 按提示输入 Key，写入 ~/.config/eng-app/ai.key（0600）

# 2. 编译后端
bash scripts/build_backend.sh

# 3. 一键启动（MySQL + AI 网关 + 后端，并打开浏览器）
open start_all.command
```

访问 `http://127.0.0.1:8088`。

手动分步启动：

```bash
bash scripts/start_mysql.sh      # MySQL（127.0.0.1:3306，Socket 连接）
bash scripts/start_localai.sh    # AI 网关（127.0.0.1:5340）
./backend/build/eng_app_server --root "$PWD" --port 8088
```

停止服务：

```bash
bash scripts/stop_all.sh
```

### 方式三：GitHub Codespaces

仓库已带 `.devcontainer/devcontainer.json`，复用同一份 `docker-compose.yml`。在 Codespaces 中打开后会自动构建并转发 8088 端口；AI Key 通过仓库 Secrets 或 `.env` 注入。

### 方式四：生产部署（公网演示）

面向公网评审/演示：80 端口、纯 IP + HTTP，不做域名与 HTTPS。

```bash
cp .env.prod.example .env.prod     # 填数据库强密码与 ZHIPU_API_KEY；文件不入库
docker compose --env-file .env.prod -f docker-compose.prod.yml up -d --build

# 演示账号与演示数据（幂等，可重复执行）
docker compose --env-file .env.prod -f docker-compose.prod.yml exec app node scripts/seed_demo.js
docker compose --env-file .env.prod -f docker-compose.prod.yml exec app node scripts/seed_demo.js --verify
```

- 与默认 `docker-compose.yml` 的区别只有三处：应用端口改为 `80:8088`、`restart: always`、容器日志限 `10m × 3`；MySQL 3306 不对宿主机开放。
- 种子脚本创建 3 个独立演示账号并预置同一套演示数据；重复执行只更新账号与词条，不重置已有学习进度。演示账号统一使用评审期公开体验口令，登录页已预填主推账号；正式使用请注册独立账号。
- 本机模式可直接 `node scripts/seed_demo.js`（加 `--verify` 校验）。
- 服务器前置、安全组、验收与排障见《[开发者使用指南](documents/开发者使用指南.md)》与 `documents/skills/06-部署.md`。

> 更细的命令、目录说明、排障方法见《[开发者使用指南](documents/开发者使用指南.md)》。

## AI Key 配置

1. 注册智谱 AI 开放平台：https://open.bigmodel.cn
2. 创建 API Key（`glm-4-flash` 完全免费）
3. 执行 `bash scripts/ai_key_store.sh set`，按提示输入 Key（写入 `~/.config/eng-app/ai.key`，权限 600）

读取优先级：密钥文件 → 环境变量 `ZHIPU_API_KEY` → `config/ai.json` 的 `api_key`。
`config/ai.json` 的 `api_key` 保持留空，真实 Key 不再写入配置文件；Docker 模式继续用 `.env` 的 `ZHIPU_API_KEY`。
未配置 Key 时 AI 功能返回明确提示，其余功能照常可用。

## 测试

```bash
node tests/run_all.js   # 全量回归（37 个套件）
npm test                # 等价
node tests/stage2_kp_solidify.test.js   # 单个套件
```

AI 相关用例依赖真实网络；失败时请先重跑一次，区分网络抖动与代码缺陷。

## 参与贡献

- **提建议 / 报 Bug**：使用 Issue 模板（`.github/ISSUE_TEMPLATE/`）：功能建议 / Bug 报告 / 体验反馈
- **提 PR**：分支命名、提交信息与审查要求见 [CONTRIBUTING.md](CONTRIBUTING.md)
- **开发规范**：迭代流程与文档体系见 [AGENTS.md](AGENTS.md)，分阶段执行手册见 `documents/skills/`

## 许可证

[MIT](LICENSE)

---

## English Overview

A lightweight, self-hosted English vocabulary learning app.

- **What it does**: turns a single English word into structured AI-generated study notes, stores them in a local MySQL database, schedules spaced-repetition reviews, and provides AI-generated exercises, local grading, daily quizzes, a wordbook and a gamified reward system.
- **Privacy**: learning data stays in your own MySQL instance; only AI requests go through a local Node gateway to Zhipu BigModel (`glm-4-flash`). No user data is sent to third-party services.
- **Stack**: C++17 backend, native HTML/CSS/JS frontend, MySQL 8.0, Node.js 20 gateway.
- **Status**: v1.5.3. The v1.5.2 batch is closed: an admin-only developer dashboard plus AI-guide intake for feature requests and bug reports. The v1.5.3 batch adds onboarding tours, production port 80 deployment and three demo accounts. The v1.5.1 batch is closed: weekly study rules with task cards, error attribution with a mistake notebook and scheduled retry, a weekly study report with rule-based plus AI coaching suggestions, a home dashboard and a site-wide AI guide (floating draggable entry, jump actions, plan proposals, 7-day conversation). The v1.5.0 feature batch is also closed: anonymous-session identity, tiered AI task queue (reuse / queueing with ETA / retries / provider failover), word lists, AI explanations and custom exercises, export center and ops baseline (daily backup, AI call logging). Article reading is partially implemented (phase 1: article CRUD + CEFR difficulty analysis).
- **Platform support**: macOS / Linux / Windows via Docker Desktop (`docker compose up`), plus GitHub Codespaces. A macOS-native path (`./dev up --host`) is also available.
- **Developer guide**: [documents/开发者使用指南.md](documents/开发者使用指南.md) (Chinese).
- **License**: MIT. Contributions and issues are welcome.
