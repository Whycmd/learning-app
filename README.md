# 轻量化本地英语学习软件

> GitHub: [Whycmd/learning-app](https://github.com/Whycmd/learning-app)
> 贡献指南：见 [CONTRIBUTING.md](CONTRIBUTING.md)

## 项目说明
基于遗忘曲线的智能化英语知识点复习、AI 资源固化拓展、自动答题批改、学情数据统计。
学习数据 100% 本地存储于 MySQL，AI 能力在线调用中国国产免费大模型，极低 Token 消耗。

## 团队协作
本项目已转为团队项目。新成员加入方式详见 [CONTRIBUTING.md](CONTRIBUTING.md)，核心流程：

1. **获取权限**：Owner 在 GitHub → Settings → Collaborators 中添加你的 GitHub 用户名
2. **克隆仓库**：`git clone https://github.com/Whycmd/learning-app.git`
3. **配置身份**：`git config user.name "你的名字" && git config user.email "你的邮箱"`
4. **开分支开发**：`git checkout -b feature/v1.5.0-T001-任务简述`
5. **推送并提 PR**：`git push -u origin feature/v1.5.0-T001-任务简述`，然后在 GitHub 创建 Pull Request

## 技术栈
- 后端: C++ (Apple clang 14) + 手写 HTTP 服务器（嵌入 Crow 源码）+ mysql-8.0 客户端库
- 运行时: Node.js v20 LTS（Word 生成、定时任务、链路适配）
- 数据库: MySQL 8.0 本地单机（仅 127.0.0.1 访问，Socket 连接）
- AI 引擎: 智谱 AI BigModel（GLM-4-Flash，完全免费，OpenAI 兼容 API）
  - 通过本地 Node.js 网关（端口 5340）转发 HTTPS 请求
  - 学习数据不外泄，仅本地网关处理网络调用
- 前端: 原生 HTML + JavaScript 硬编码（无框架、无构建工具）
- Word 生成: Node.js `docx` 库（本地离线，生成标准 .docx）
- 设计系统: Fraunces（衬线标题）+ Karla（正文），4 个 woff2 字体离线可用

## 目录结构
```
英语学习应用/
├── backend/           C++ 后端源码
│   ├── src/          源文件
│   ├── include/      头文件（服务层）
│   └── build/        编译产物
├── frontend/         原生 HTML+JS 前端
│   ├── pages/        6 个功能页面
│   └── static/       样式、字体、脚本
├── scripts/          启动/停止/部署脚本、Word 生成、定时清理
├── tests/            各阶段测试套件（7 个，共 205 项）
├── config/           配置文件（数据库、AI、应用）
├── backups/          数据库备份（.sql）
├── logs/             运行日志
├── word_tmp/         习题/知识点 Word 临时目录（自动清理）
└── documents/        产品需求与迭代规范文档
```

## 环境路径
- Node.js: `/Users/junwang/.local/bin/{node,npm,npx}`
- MySQL: `/Users/junwang/.local/mysql/`
- MySQL data: `/Users/junwang/.local/mysql/data`
- MySQL sock: `/Users/junwang/.local/run/mysql.sock`
- MySQL root 密码: `/Users/junwang/.local/mysql/.root_pw`
- AI 网关: `http://127.0.0.1:5340/v1` → 智谱 AI HTTPS API
- AI 模型: `glm-4-flash`（完全免费）
- 项目数据库: `english_learning`
- 项目用户: `eng_app`（同 root 密码）

## 一键启动
```bash
# 方式一：双击运行
open start_all.command

# 方式二：手动启动（依次执行）
bash scripts/start_mysql.sh
node scripts/local_ai_server.js
./backend/build/eng_app_server
```

启动后浏览器自动打开 `http://127.0.0.1:8088`

## 停止服务
```bash
bash scripts/stop_all.sh
# 或双击桌面「停止英语学习.command」
```

## 测试运行
```bash
# 全量回归（7 套件，205 项）
npm test

# 单个套件
npx jest tests/stage2_kp_solidify.test.js
```

## 当前版本
**v1.4.0** — 知识点 AI 固化、Markdown 精讲、异步批量固化、Word 导出、遗忘曲线复习、AI 出题批改、每日小测、点金圣殿、多用户认证；14 套件 471 项测试通过。
文章阅读阶段 2 待后续排期。
