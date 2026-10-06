# 贡献指南

> 适用：所有团队成员 | 最后更新：2026-10-06

---

## 一、成为团队成员（一次性）

### 方式 A：添加为 Collaborator（推荐，个人仓库）

1. 打开 [https://github.com/Whycmd/learning-app/settings/collaborators](https://github.com/Whycmd/learning-app/settings/collaborators)
2. 点击 **Add people**，输入成员的 GitHub 用户名或邮箱
3. 成员会收到邀请邮件，点击接受后即可获得推送权限

### 方式 B：Fork + Pull Request（无协作者权限时）

若成员暂时没有协作者权限，可先 Fork 本仓库，在自己的 Fork 里开发，完成后发 Pull Request。

### 团队角色约定

| 角色 | 权限 | 职责 |
|------|------|------|
| Owner | 全部权限 | 架构决策、合并 PR、管理协作者 |
| Team Member | push 到指定分支 | 按任务开发、提 PR |

---

## 二、日常工作流

```
┌─────────────────────────────────────────────────────┐
│  每天开始工作前                                       │
│  git pull origin main                                │
└─────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────┐
│  开新分支（以任务编号命名）                            │
│  git checkout -b feature/v1.5.0-T001-team          │
└─────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────┐
│  开发 + 本地测试（npm test 全量通过）                  │
└─────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────┐
│  提交（commit message 格式：type: 简述）              │
│  git commit -m "feat: 实现团队列表接口"               │
└─────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────┐
│  推送到远程                                           │
│  git push -u origin feature/v1.5.0-T001-team        │
└─────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────┐
│  在 GitHub 上创建 Pull Request                       │
│  → 填写 PR 描述（做了什么、如何测试）                 │
│  → 请求 Owner 审查合并                                │
└─────────────────────────────────────────────────────┘
```

### 分支命名规范

| 类型 | 格式 | 示例 |
|------|------|------|
| 功能分支 | `feature/v{版本}-T{序号}-{简述}` | `feature/v1.5.0-T001-team-list` |
| 修复分支 | `fix/v{版本}-T{序号}-{简述}` | `fix/v1.4.0-T003-fix-login` |
| 文档分支 | `docs/{简述}` | `docs/contributing-guide` |

### Commit Message 格式

```
<type>: <subject>

<body>（可选）

<footer>（可选）
```

| type | 含义 |
|------|------|
| `feat` | 新功能 |
| `fix` | Bug 修复 |
| `docs` | 文档变更 |
| `refactor` | 重构（不改功能） |
| `test` | 测试相关 |
| `chore` | 构建/工具/杂项 |

---

## 三、上传文件的两种方式

### 方式 1：git push（有协作者权限）

```bash
# 克隆仓库（首次）
git clone https://github.com/Whycmd/learning-app.git
cd learning-app

# 配置你的身份信息（一次性）
git config user.name "你的名字"
git config user.email "你的邮箱@xx.com"

# 创建分支、开发、提交、推送
git checkout -b feature/v1.5.0-T001-team-list
# ... 开发 ...
git add .
git commit -m "feat: 实现团队列表接口"
git push -u origin feature/v1.5.0-T001-team-list
```

### 方式 2：GitHub Web 上传（适合小文件/单个文件）

1. 打开 [https://github.com/Whycmd/learning-app](https://github.com/Whycmd/learning-app)
2. 进入目标文件夹（如 `backend/src/`）
3. 点击 **Add file** → **Upload files**
4. 拖拽文件到页面，填写提交信息，点击 **Commit changes**

> ⚠️ Web 上传适合单个或少量文件；大量文件请用 git push。

### 方式 3：Fork + PR（无协作者权限）

```bash
# 1. 在 GitHub 上点 Fork 按钮，fork 到自己的账号
# 2. 克隆自己的 fork
git clone https://github.com/你的用户名/learning-app.git
cd learning-app

# 3. 添加原仓库为 upstream（便于同步最新代码）
git remote add upstream https://github.com/Whycmd/learning-app.git

# 4. 开发并提交到你的 fork
git checkout -b feature/v1.5.0-T001-team-list
# ... 开发 ...
git add .
git commit -m "feat: 实现团队列表接口"
git push -u origin feature/v1.5.0-T001-team-list

# 5. 在原仓库页面创建 Pull Request
#    → New pull request → Compare forks → Create pull request
```

---

## 四、多人协作注意事项

### 避免冲突

- **每天开工前先 `git pull`**，拉取其他人的最新提交
- 分支粒度尽量小，功能完成尽快提 PR，减少合并冲突
- 修改同一文件前，先和该文件的主要维护者沟通

### 文件规范

- 所有文本文件统一 LF 换行（已在 `.gitattributes` 中配置）
- 不提交 `config/ai.json`（含密钥）、`backups/`、`node_modules/`、编译产物
- 新增依赖在 `package.json` 中声明，不手动上传 `node_modules/`

### 代码审查

- 每个 PR 至少需要 **1 人审查** 后才能合并
- 审查关注点：功能正确性、测试覆盖、是否符合现有架构、无硬编码路径

---

## 五、快速检查清单（提 PR 前）

- [ ] 已从 `main` 拉取最新代码并合并
- [ ] `npm test` 全量通过（205 项测试）
- [ ] 未提交任何密钥、密码、本地路径
- [ ] Commit message 符合规范
- [ ] PR 描述说明了改动内容和测试方式
