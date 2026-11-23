# 交接说明（2026-11-21，总任务接续 COMPETE-READY 收尾）

> 用途：新对话接续「比赛交付」主任务。先读本文，再读 `当前版本迭代记录/v1.5.3-T002-COMPETE-READY/开发计划.md` 与 `部署验收记录.md`。
> 本文不含任何密码、API Key。

## 一、状态快照

| 项 | 值 |
|---|---|
| 运行时版本 | `1.5.3`（`config/app.json` / `package.json` 一致） |
| 编号批次 | v1.5.3；`v1.5.3-T002`（COMPETE-READY）已收口（2026-11-23） |
| 任务状态 | T1 生产端口与 gzip ✅、T2 演示账号与数据 ✅、T3 公网部署与验收 ✅、T4 申报材料包 ✅、T5 全量回归与收口 ✅（2026-11-23 完成） |
| 线上地址 | `http://123.56.205.221/`（公网 80 → 容器 8088，HTTP，纯 IP） |
| 演示账号 | `wordninja`（主推）/ `templecat` / `goldfox`（2026-11-23 由 `demo1` / `demo2` / `demo3` 改名，密码为评审期公开体验口令） |
| 云主机 | 阿里云 ECS `ecs.e-c1m1.large`，2 核 2G / 40G / 3M 固定带宽 / Ubuntu 24.04.5，到期 2027-11-21 |
| 全量回归基线 | 37 套件 / 0 失败（2026-11-23 宿主机实测；开发库行数未变化） |
| 代码状态 | 2026-11-23 已将工作区全量提交并推送 GitHub `main`（v1.5.3）；此前 HEAD 长期停在 v1.4.1，服务器部署的是工作区副本 |
| 文档待审 | `待审查登记.md` 中 Q-075～Q-093 待用户逐项审查 |

## 二、本轮已完成（2026-11-20 ～ 11-21）

- T1：`docker-compose.prod.yml`（80→8088、`restart: always`、日志 10m×3、3306 不对外）、`.env.prod.example`、`Dockerfile` 预压缩 `.gz`、`handleStatic()` 按 `Accept-Encoding` 返回压缩体并设 `Cache-Control`（css/js 仍为 `no-cache`，T003 已改，勿回退）。
- T2：`scripts/seed_demo.js` 幂等创建 3 个演示账号与演示数据，`--verify` 三账号全部 PASS。
- T3：服务器安装 Docker 29.1.3 + Compose v2.40.3、2G swap、`.env.prod`（600）、构建并启动；线上主链路实测通过（首页 0.16s、gzip、3 账号登录、AI 讲解 L1 秒回 + L2 真实生成 15s、答题、小测判分与错因、错题本、历史、词币）；手机 4G 实测通过。
- 文档收尾：新增 `部署验收记录.md`；同步 `开发计划.md`、`06-部署.md`、`开发经验总结.md`（ENV-04～07）、`核心参数.md`、`产品现状.md` §30、`01-未完成任务与优先级.md`、`待审查登记.md`。

## 三、本轮用户裁决（D1–D8，来自开发计划 §六）

| 编号 | 结论 |
|---|---|
| D1 | 比赛 11/23 截止，MOBILE-LITE 顺延，插入 COMPETE-READY |
| D2 | 体验链接用 80 端口、纯公网 IP、HTTP |
| D3 | 不买域名、不做 HTTPS、不做 ICP 备案 |
| D4 | 阿里云 99 元 ECS（2 核 2G / 3M / 40G / Ubuntu 24.04） |
| D5 | 带宽保持 3M 固定，不升配 |
| D6 | 加 gzip 与缓存头 |
| D7 | 3 个独立演示账号，各预置同一套数据 |
| D8 | 后续加站不锁死：用 Nginx 反向代理，本应用退内网端口，代码不动（本期不做） |

## 四、剩余工作（按顺序）

### 4.1 用户侧（必须先做）

1. 删除安全组 RDP `3389/3389` 入方向规则，仅保留 22、80。
2. 评审前保持服务器运行；每天检查 `GET /api/health`。
3. 审查 Q-075～Q-081。

### 4.2 T4 申报材料包（需用户提供素材）

| 材料 | 来源 | 说明 |
|---|---|---|
| 3 个关键任务 TRAE Session ID | 用户 | AI 讲解与精讲固化、课规与学情周报、错题归因与重练 |
| 开发过程截图 ≥3 张 | 用户 | 保存后把路径登记进材料清单 |
| 产品截图 1–3 张 | 用户 | 首页、AI 讲解、错题本或周报 |
| 演示视频 1–5 分钟 | 用户录屏 | 分镜与旁白可由 AI 起草，覆盖「查词讲解 → 加学习 → 复习小测 → 错题重练 → 周报」 |
| 一页说明 | AI 可产 | 体验链接、3 组账号、已知限制（无 HTTPS、AI 依赖网络、移动端为桌面版兜底） |
| 材料清单 | AI 可产 | 落在本任务目录，逐项写路径/链接 |

### 4.3 T5 全量回归与收口

1. 本地 `node tests/run_all.js`（需本机 MySQL 与 AI 网关可用；AI 类失败先区分网络抖动再重跑）。
2. 线上复验：health、首页首屏、3 账号登录、AI 讲解一次、小测一次、错题本。
3. 文档同步：`README.md`、`开发者使用指南.md`、`操作指南/人工修改渠道集合.md`、`产品现状.md`、`核心参数.md`，并在 `待审查登记.md` 留痕。
4. 版本号：按 `skills/06-部署.md` 判定；本任务为新增功能（部署编排、脚本、测试），预计 MINOR 位，需用户确认后升位并同步全部登记位置。
5. 收口后删除 `当前版本迭代记录/v1.5.3-T002-COMPETE-READY/`（或按规范保留，需用户裁定）。

## 五、服务器运维要点

- 代码目录 `/opt/english-app`；启动：
  `docker compose --env-file .env.prod -f docker-compose.prod.yml up -d --build`
- 查看状态：`docker compose --env-file .env.prod -f docker-compose.prod.yml ps`；日志 `docker logs --tail 100 elf-app`。
- 种子上限：`scripts/seed_demo.js` 幂等，可重复跑；`--verify` 校验。
- **镜像源补丁只在服务器**：`Dockerfile` 两阶段 `apt-get update` 前把 `deb.debian.org` 换成 `mirrors.aliyun.com`。**仓库内未改**，换机或在别处重建会再次卡住，属遗留 L2。
- SSH 上禁止 `pkill -f "docker compose"`（会断开自身连接）；用 `pgrep -f "[d]ocker compose"` 或 `fuser -k 80/tcp`。
- 临时部署公钥 `trae-deploy` 仍在 `/root/.ssh/authorized_keys`，赛后清理（`sed -i '/trae-deploy/d' /root/.ssh/authorized_keys`）。
- 本地临时私钥目录 `/tmp/eng_deploy/`（赛后删除）；服务器也可用阿里云 Workbench 登录，不依赖该密钥。

## 六、已知坑与经验（详见 `skills/开发经验总结.md`）

- ENV-04：云镜像未预装 Docker，`systemctl enable --now docker` 会报 Unit 不存在。
- ENV-05：容器构建卡 `deb.debian.org`，需换 `mirrors.aliyun.com`，且必须回写仓库。
- ENV-06：SSH 上 `pkill -f` 自匹配会断连（exit 255）。
- ENV-07：智谱 Key 49 位含点；51 位无点会 401「令牌已过期或验证不正确」。
- 3M 带宽：首屏已预压缩，评审多人同时访问会慢，属预期。
- AI 讲解首次生成约 10–20 秒，页面轮询属正常，不是卡死。

## 七、文件索引

| 文件 | 用途 |
|---|---|
| `当前版本迭代记录/v1.5.3-T002-COMPETE-READY/开发计划.md` | 任务准绳：T1–T5、验收标准、D1–D8 |
| `当前版本迭代记录/v1.5.3-T002-COMPETE-READY/部署验收记录.md` | T3 实际部署、问题处理、实测数据、遗留 L1–L5 |
| `documents/skills/06-部署.md` | 云主机容器化部署必做与检查清单 |
| `documents/skills/开发经验总结.md` | 环境部署 ENV-01～ENV-07 |
| `documents/devlog/核心参数.md` | 生产地址、云主机、安全组、演示账号 |
| `documents/devlog/产品现状.md` §30 | 对外可陈述的部署与验收结论 |
| `documents/devlog/待审查登记.md` | Q-075～Q-081 待审查 |
| `scripts/seed_demo.js` | 演示账号与数据，支持 `--verify` |
| `docker-compose.prod.yml` / `.env.prod.example` | 生产编排与变量模板 |

## 八、接续第一步

1. 读 `开发计划.md` §三 T4/T5 与 §四验收标准。
2. 与用户确认 T4 素材是否齐备；齐备则整理材料清单，否则先产出「一页说明 + 视频分镜脚本」。
3. T5 全量回归在 T4 素材确认后执行；未完成 T4/T5 前不升版本号。
