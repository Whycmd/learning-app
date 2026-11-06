# 轻量化本地英语学习软件（Learning App）

> GitHub：[Whycmd/learning-app](https://github.com/Whycmd/learning-app)
> 贡献指南：[CONTRIBUTING.md](CONTRIBUTING.md) ｜ 许可证：[MIT](LICENSE)
> 当前版本：v1.4.1（阶段一「向开发者开放」已完成）

## 项目定位

基于遗忘曲线的英语知识点学习工具：输入英文单词，AI 生成结构化精讲并固化入库，按遗忘曲线安排复习，配合 AI 出题、批改、每日小测与单词本。

学习数据 100% 存在本机 MySQL，AI 能力通过本地网关调用智谱 GLM-4-Flash（免费），用户数据不落第三方。

## 功能（v1.4.1 已完成）

| 模块 | 能力 |
|------|------|
| 知识点系统 | 新词录入 → AI 生成六块结构化知识 → 固化入库；Markdown 精讲本地渲染；单个/批量 Word 导出（含 LaTeX 公式） |
| 异步批量固化 | `POST /api/kp/solidify/async` 立即返回任务 ID；分片并发、进度轮询、任务历史、断点续跑 |
| 智能复习 | 遗忘曲线双模式（按周 / 按日）；熟练度越高间隔越长 |
| AI 出题与批改 | 6 种题型；客观题本地秒批，不改遗忘曲线 |
| 每日小测 | 3 题已固化知识 + 1 题熟词生义 + 1 题新知，本地批改并生成 Word 报告 |
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
├── frontend/           原生前端（index.html、pages/ 8 个页面、static/ 样式与脚本）
├── scripts/            启动/停止/构建、AI 网关、Word 生成、定时清理、SQL 初始化
│   └── sql/            建表脚本 8 个（初始化用）
├── tests/              全量回归测试 17 个套件
├── config/             配置（db.json、ai.json、app.json、rewards.json）
├── documents/          产品与迭代文档（含 skills/ 执行手册、devlog/）
├── backups/            数据库备份
├── logs/               运行日志
├── word_tmp/           Word 临时目录（自动清理）
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
| `./dev test` | 全量回归（17 套件） |
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
cp config/ai.example.json config/ai.json   # 编辑填入 api_key

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

> 更细的命令、目录说明、排障方法见《[开发者使用指南](documents/开发者使用指南.md)》。

## AI Key 配置

1. 注册智谱 AI 开放平台：https://open.bigmodel.cn
2. 创建 API Key（`glm-4-flash` 完全免费）
3. 填入 `config/ai.json` 的 `api_key` 字段

`config/ai.json` 已在 `.gitignore` 中，**不会被提交**；模板见 `config/ai.example.json`。
未配置 Key 时 AI 功能返回明确提示，其余功能照常可用。

## 测试

```bash
node tests/run_all.js   # 全量回归（17 个套件）
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
- **Status**: v1.4.1. Core learning modules are complete; article reading is partially implemented (phase 1: article CRUD + CEFR difficulty analysis).
- **Platform support**: macOS / Linux / Windows via Docker Desktop (`docker compose up`), plus GitHub Codespaces. A macOS-native path (`./dev up --host`) is also available.
- **Developer guide**: [documents/开发者使用指南.md](documents/开发者使用指南.md) (Chinese).
- **License**: MIT. Contributions and issues are welcome.
