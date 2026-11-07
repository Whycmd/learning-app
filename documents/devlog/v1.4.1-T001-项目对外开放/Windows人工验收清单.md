# Windows 人工验收清单（v1.4.1-T001）

> 验收版本：**v1.4.1**（提交 `312ce9e`）
> 适用对象：Windows 10 / 11（64 位）
> 验收目标：确认「一条命令启动 → 浏览器完整使用 → 数据不丢 → 局域网不可见」
> 预计耗时：最小验收约 15 分钟；完整验收 40–60 分钟（首次镜像构建 3–10 分钟，取决于网络）
> 记录方式：每项打 `通过 / 不通过`，不通过的写下报错原文与截图
> 发放方式：发给同事本文件 + 仓库地址（或同目录的 ZIP 包）即可

---

## 零、开始前准备（约 10 分钟）

| # | 检查项 | 通过标准 |
|---|---|---|
| 0.1 | Windows 版本 | Win10 22H2 或 Win11，64 位 |
| 0.2 | 虚拟化已开启 | 任务管理器 → 性能 → CPU 显示「虚拟化：已启用」 |
| 0.3 | 安装 Docker Desktop | 官网下载 Windows 版，安装时勾选 WSL2 后端；**首次打开可点 Skip 跳过登录，不需要 Docker 账号** |
| 0.4 | WSL2 就绪 | 安装后重启；PowerShell 执行 `wsl --status` 能正常输出 |
| 0.5 | Docker 可用 | `docker --version` 与 `docker compose version` 均有版本号输出 |
| 0.6 | Docker 已启动 | 任务栏鲸鱼图标为 Running；`docker ps` 不报「Cannot connect」 |
| 0.7 | Git（可选） | 用 Git 克隆才需要；直接使用 ZIP 包可跳过 |
| 0.8 | 网络 | 能访问 GitHub 才能克隆；被墙时改用 ZIP 包，不影响后续验收 |
| 0.9 | 磁盘空间 | 至少留 15 GB（镜像 + 数据卷） |

> 说明 1：项目**没有 Windows 原生版**，Windows 上只走 Docker 这一条路径。
> 说明 2：根目录的 `dev` 是 bash 脚本，PowerShell 里不能直接运行；Windows 用户统一用
> `docker compose` 命令，或在 Git Bash / WSL 里用 `./dev`。
> 说明 3：**不需要**安装 Node.js、MySQL、C++ 编译器，这些都在镜像里。

---

## 一、获取代码与配置（约 5 分钟）

建议放在**纯英文路径**，例如 `C:\dev\learning-app`。中文路径、OneDrive 同步目录可能引发挂载与权限问题。

方式 A（能访问 GitHub）：

```powershell
cd C:\dev
git clone https://github.com/Whycmd/learning-app.git
cd learning-app
Copy-Item .env.example .env
notepad .env
```

方式 B（拿到 ZIP）：解压到 `C:\dev\learning-app`，同样需要有 `.env.example`，再执行 `Copy-Item .env.example .env`。

| # | 检查项 | 通过标准 |
|---|---|---|
| 1.1 | 仓库完整 | 能看到 `Dockerfile`、`docker-compose.yml`、`.env.example`、`dev`、`backend`、`frontend`、`README.md` |
| 1.2 | 版本正确 | `README.md` 顶部写「当前版本：v1.4.1」 |
| 1.3 | `.env` 已生成 | 与 `.env.example` 内容一致（未改的默认值即可直接跑） |
| 1.4 | AI Key（可选） | `.env` 中 `ZHIPU_API_KEY=` 后填入真实 Key；暂不填也能继续，只是 AI 功能不可用 |
| 1.5 | Key 格式 | 只含英文字母/数字，**不能有中文、空格、引号** |

> 不填 Key 也能完成本清单第零、一、二、四章，以及第三章里除 AI 相关的部分。
> 需要验收 AI 固化 / AI 出题 / 每日小测时，则需要一个免费 Key：https://open.bigmodel.cn

---

## 二、启动（约 3–10 分钟，首次较慢）

```powershell
docker compose up -d
docker compose ps
```

| # | 检查项 | 通过标准 |
|---|---|---|
| 2.1 | 拉镜像 | 日志出现 `docker.m.daocloud.io/library/mysql:8.0`、`node:20-slim`，无长时间卡死 |
| 2.2 | 编译后端 | 构建阶段出现 g++ 编译输出，最终 `docker compose up -d` 正常返回 |
| 2.3 | 容器状态 | `docker compose ps` 中 `elf-db` 为 `healthy`，`elf-app` 为 `Up` |
| 2.4 | 等就绪 | 首次启动 app 会等数据库建表，等待 20–60 秒后再访问；`docker compose logs app` 末行出现启动成功信息 |
| 2.5 | 首页可访问 | 浏览器打开 <http://127.0.0.1:8088> 显示登录/首页，不是报错页或空白页 |
| 2.6 | 端口只绑本机 | PowerShell `netstat -ano | findstr :8088` 显示监听地址为 `127.0.0.1:8088`（不是 `0.0.0.0:8088`） |
| 2.7 | 数据库未对外 | `netstat -ano | findstr :3306` 无结果（MySQL 端口不映射到宿主机） |

> 失败时先看日志：`docker compose logs --tail=100 app`，把最后 20 行贴进反馈。
> 若 2.5 页面空白/502，通常是 2.4 还没等够，先等 30 秒再刷新，不要急着重装。

---

## 三、核心功能验收（约 15–20 分钟）

依次按页面操作，每页确认「能打开、能提交、有正确反馈」。

> **前置判断**：如果 `.env` 里没有填 `ZHIPU_API_KEY`，3.2、3.3、3.5.3–3.5.5 无法验收（这是设计如此，
> 不算失败），请跳过这几项，改做第四章 4.5；其余项照常验收。

### 3.1 账号

| # | 操作 | 通过标准 |
|---|---|---|
| 3.1.1 | 注册新账号（用户名 6–100 位） | 注册成功并自动登录 |
| 3.1.2 | 退出后重新登录 | 登录成功；刷新页面仍是登录态 |
| 3.1.3 | 设置学习者身份（如「大学生」） | 保存成功，重新进入页面仍显示 |

### 3.2 知识点录入与固化（重点：秒表反馈）

| # | 操作 | 通过标准 |
|---|---|---|
| 3.2.1 | 录入单词 `apple` | 列表出现新词条，状态「未固化」 |
| 3.2.2 | 点击该词条「固化」 | **立即**出现「固化中… 已用 N 秒（AI 正在生成释义、词根、搭配、同义词等内容，通常 15~30 秒）」，秒数每秒递增 |
| 3.2.3 | 等待完成 | 提示「固化成功（X.X 秒）」，状态变为已固化 |
| 3.2.4 | 打开详情 | 六块内容齐全：核心含义、词根词源、同义/近义词、反义词、固定搭配、补充知识 |
| 3.2.5 | 精讲排版 | Markdown 正常渲染（标题、列表、加粗），没有出现 `##`、`**` 等原始符号 |
| 3.2.6 | 再录入同名词 | 不报错（未固化允许重复；已固化走查重/合并逻辑） |

> 3.2.2 是本版新增的关键体验点，必须确认秒表在**点击后立即出现**，而不是等十几秒后才出现。
> 单次固化 15–30 秒属正常（AI 一次生成六块内容），超过 60 秒请记录耗时并反馈。

### 3.3 批量固化（可选）

| # | 操作 | 通过标准 |
|---|---|---|
| 3.3.1 | 连续录入 3–5 个词 | 全部进入未固化列表 |
| 3.3.2 | 触发批量固化 | 立即返回任务 ID，页面轮询显示进度 |
| 3.3.3 | 完成后再进页面 | 能看到任务历史与成功/失败条数 |

### 3.4 Word 导出

| # | 操作 | 通过标准 |
|---|---|---|
| 3.4.1 | 对已固化词条点「导出 Word」 | 提示成功，宿主机仓库目录 `word_tmp\` 出现 `.docx` 文件（容器已挂载该目录） |
| 3.4.2 | 用 Microsoft Word 打开 | 能正常打开，无「文件已损坏」提示 |
| 3.4.3 | 检查内容 | 标题、六块内容、加粗、列表正常；含公式的词条公式可显示 |
| 3.4.4 | 批量导出 | 多词条一次导出成功，文件数量与选择一致 |

### 3.5 复习与答题

| # | 操作 | 通过标准 |
|---|---|---|
| 3.5.1 | 首页查看复习队列 | 显示按遗忘曲线安排的待复习词条 |
| 3.5.2 | 完成一次复习答题 | 答对后熟练度提升，下次复习间隔变长 |
| 3.5.3 | AI 出题 | 生成 6 种题型之一；客观题提交后**本地秒批**给出对错（不改动遗忘曲线） |
| 3.5.4 | 每日小测 | 生成 3 题已固化 + 1 题熟词生义 + 1 题新知；提交后给出结果 |
| 3.5.5 | 小测报告 | 能导出 Word 报告并打开 |

### 3.6 单词本 / 播放 / 点金圣殿

| # | 操作 | 通过标准 |
|---|---|---|
| 3.6.1 | 单词本加入生词 | 列表可见，可标记已掌握 |
| 3.6.2 | 单词本导入导出 | 导出文件可下载并重新导入 |
| 3.6.3 | 播放复习 | 点击播放能听到英文朗读（依赖浏览器语音，Chrome/Edge 正常） |
| 3.6.4 | 点金圣殿 | 行为有积分入账，金桂余额变化，可查看朱砂印/称谓 |

> 3.6.3 若无声：Windows 设置 → 时间和语言 → 语音，确认已安装英文语音包；
> 换 Edge / Chrome 再试，这属于浏览器能力，不是应用故障。

---

## 四、数据与安全验收（约 5 分钟）

| # | 操作 | 通过标准 |
|---|---|---|
| 4.1 | 重启后数据仍在 | 记录当前词条数 → `docker compose down` → `docker compose up -d` → 词条数、账号、熟练度全部保留 |
| 4.2 | 卷存在 | `docker volume ls` 能看到 `english-learning-app_mysql_data` |
| 4.3 | 局域网不可访问 | 手机连同一 Wi-Fi，访问 `http://<电脑IP>:8088`，应**连接失败或超时** |
| 4.4 | 敏感文件未泄露 | 仓库根目录与容器内都查不到真实 Key：`docker compose exec app cat config/ai.json` 中 `api_key` 为空；`git status` 看不到 `.env`（`.env`、`config/ai.json` 均不纳管） |
| 4.5 | 无 Key 也能用 | 把 `.env` 的 `ZHIPU_API_KEY` 清空后 `docker compose up -d --force-recreate app`：应用能正常启动，AI 相关功能提示未配置，录入/复习/单词本等照常可用 |
| 4.6 | 容器内无宿主 Key | `docker compose exec app grep ZHIPU_API_KEY -r config` 无输出（Key 只通过环境变量注入，不落文件） |

> 4.4 说明：容器内 `config/ai.json` 是启动脚本生成的**空 Key 模板**，属正常现象；
> 真正判断标准是里面没有你的真实 Key。

---

## 五、可选：容器内回归（约 10 分钟）

给愿意多花 10 分钟的验收人：

```powershell
docker compose exec app node tests/run_all.js
```

| # | 通过标准 |
|---|---|
| 5.1 | 17 个套件全部通过（`stage16` 在容器内整组跳过属正常，另有个别「无测试账号」类跳过也属正常） |
| 5.2 | AI 相关用例失败先重跑一次；仍失败再记录具体套件名与报错原文 |

---

## 六、结果反馈模板

把下面内容填好发回即可：

```text
验收人：
Windows 版本：
Docker Desktop 版本：
Docker Compose 版本：
浏览器与版本：
代码路径：C:\...
是否填写 AI Key：是 / 否
首次构建耗时：

[零] 准备：通过 / 不通过（备注）
[一] 配置：通过 / 不通过（备注）
[二] 启动：通过 / 不通过（备注）
[三] 功能：通过 / 不通过（逐条列出不通过项）
[四] 数据与安全：通过 / 不通过（备注）
[五] 容器内回归：通过 / 未执行

Windows 独有问题（如有）：
报错原文（最后 20 行日志）：
截图：
```

---

## 七、Windows 上已知的坑（先看再报错）

| 现象 | 处理 |
|---|---|
| `docker compose` 报 `project name must not be empty` | 确认 `docker-compose.yml` 顶部的 `name: english-learning-app` 没有被删 |
| 拉镜像极慢或失败 | 检查 `.env` 的 `MYSQL_IMAGE` / `BASE_IMAGE` 是否仍是 daocloud 加速地址；公司网络可能需代理 |
| 首次打开页面空白 / 502 | 数据库还在建表，等 20–60 秒；用 `docker compose logs app` 确认已启动完成 |
| `docker ps` 报 Cannot connect | Docker Desktop 没启动完，等鲸鱼图标变 Running 再执行 |
| 端口 8088 被占用 | `netstat -ano | findstr :8088` 找到进程并结束，或改 `.env` 的 `APP_PORT` |
| Word 文件打不开 | 确认文件在 `word_tmp\` 且已完整生成；先用记事本确认不是 0 字节 |
| 中文路径报挂载错误 | 把仓库移到 `C:\dev\learning-app` 这类纯英文路径后重试 |
| PowerShell 不能运行 `./dev` | `dev` 是 bash 脚本；用 Git Bash / WSL，或直接使用 `docker compose` |
| 杀毒软件拦截 | 将 Docker 数据目录加入白名单后重启 Docker Desktop |
| 朗读没有声音 | 安装 Windows 英文语音包，换 Edge/Chrome 重试；属浏览器 TTS 能力 |
| 想清空重来 | `docker compose down -v` 会**删除数据卷（学习数据全丢）**，确认后再执行 |

---

## 八、常用命令（收藏备用）

```powershell
docker compose up -d          # 启动
docker compose ps             # 看容器状态
docker compose logs -f app    # 跟踪日志（Ctrl+C 退出）
docker compose restart app    # 只重启应用容器
docker compose down           # 停止并删除容器（数据保留）
docker compose down -v        # 停止并删除数据卷（数据清空，慎用）
```
