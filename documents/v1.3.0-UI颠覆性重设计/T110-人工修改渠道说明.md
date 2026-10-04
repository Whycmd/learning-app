# 人工修改渠道说明文档

**任务编号**：v1.3.0-T110
**版本**：v1.3.0
**最后更新**：2026-10-02

> 本文档说明如何在不改动业务逻辑的前提下，通过调整变量和少量样式快速定制外观。

---

## 一、全局定制（影响全站）

### 1.1 颜色与主题

**文件**：`frontend/static/style.css`，第 1–120 行 `:root` 块

| 变量 | 用途 | 示例修改 |
|------|------|---------|
| `--accent` | 主色调（金色） | `#c9a84c` → `#4a90d9`（改蓝） |
| `--accent-glow` | 焦点环/高光 | `#c9a84c` → 与 `--accent` 同步 |
| `--surface` | 卡片背景 | `#fefdf8` → `#f5f5f5`（灰白） |
| `--ink` | 正文文字色 | `#1a1a2e` → `#222` |
| `--line` / `--line-strong` | 分割线深浅 | 同步微调保持对比度 |
| `--gold` / `--gold-dark` | 金色系变体 | 调整金品质感 |
| `--danger` / `--listen` / `--read` / `--write` | 技能色 | 分别对应听/说/读/写 |

**暗色主题**：在同一文件 `html[data-theme="dark"]` 块（约第 130–170 行）覆盖上述变量，无需改 JS。

### 1.2 字体

**文件**：
- `frontend/static/fonts.css` — `@font-face` 声明
- `frontend/static/style.css` `:root` — `--font-display`（标题）、`--font-body`（正文）

替换字体 woff2 文件后，更新 `fonts.css` 中的 `src: url('...woff2')` 路径即可。

### 1.3 圆角与间距

| 变量 | 默认值 | 修改建议 |
|------|--------|---------|
| `--r-sm` | `6px` | 微调卡片圆角 |
| `--r-md` | `12px` | 抽屉、弹窗圆角 |
| `--r-lg` | `20px` | 大卡片、英雄区 |
| `--r-pill` | `999px` | 按钮、标签圆角 |
| `--sp-xs` / `--sp-sm` / `--sp-md` / `--sp-lg` | `4/8/16/24px` | 全局间距基准 |

### 1.4 动效速度

| 变量 | 默认值 | 用途 |
|------|--------|------|
| `--dur-fast` | `120ms` | 按钮 hover、chip 切换 |
| `--dur-mid` | `240ms` | 卡片 hover、抽屉开合 |
| `--dur-slow` | `400ms` | 进度环、分布条 |
| `--ease-out` | `cubic-bezier(.22,.61,.36,1)` | 通用缓出曲线 |

---

## 二、分页面定制

### 2.1 首页（`pages/index.html`）

**关键构件**：`narr`、`hall`、`caisson`、`stat-grid`、`skill-grid`

- 调整仪表盘尺寸：`style.css` 中 `.caisson`（约第 380 行）修改 `width` / `height`
- 调整技能图标大小：`.skill-btn`（约第 430 行）修改 `font-size`
- 修改统计格列数：`stat-grid { grid-template-columns: ... }`（`900px` 断点内）

### 2.2 知识点录入 / 详情（`pages/kp-entry.html` / `kp-detail.html`）

**关键构件**：`section-title`、`wing-card`、`narr`

- 修改板块标题样式：`.section-title`（`components.css` 第 15 行附近）
- 修改卡片内边距：`.wing-card`（`components.css` 第 20 行附近）

### 2.3 每日复习（`pages/review.html`）

**关键构件**：`review-layout`、`review-card`

- 调整答题卡宽度：`.review-card`（`review.html` 下方 style 块）

### 2.4 每日小测（`pages/daily-quiz.html`）

**关键构件**：`quiz-score`、`quiz-option`

- 调整得分环尺寸：`.quiz-score`（`components.css` 约第 450 行）

### 2.5 单词本（`pages/wordbook.html`）

**关键构件**：`drawer-overlay`、`drawer-panel`

- 调整抽屉宽度：`.drawer-panel`（`components.css` 约第 125 行）修改 `width`
- 修改遮罩透明度：`.drawer-overlay` 修改 `background` 的 `alpha`

### 2.6 学情中心（`pages/learning-center.html`）

**关键构件**：`lc-grid`、`lc-chart-wrap`、`lc-dist-row`

- 修改图表容器宽度：`.lc-chart-wrap`（`components.css` 约第 770 行）
- 调整分布条高度：`.lc-dist-row__fill`（`components.css` 第 809 行）

### 2.7 登录注册（`pages/login.html`）

**关键构件**：`auth-wrap`、`auth-card`、`auth-tabs`、`seal-deco`

- 调整印章大小：`.seal-deco`（`login.html` 内部 style）修改 `width` / `height`
- 修改 Tab 分段宽度：`.auth-tabs` 子元素修改 `flex` / `width`

### 2.8 文章阅读（`pages/articles.html`）

**关键构件**：`filter-chip`、`art-card`、`drawer-overlay`

- 调整筛选 chip 内边距：`.filter-chip`（`components.css` 第 961 行）
- 修改卡片缩略图尺寸：`.art-card__thumb`（`components.css` 约第 990 行）

---

## 三、定制流程示例

**示例：将主色调从金色改为蓝色**

1. 打开 `frontend/static/style.css`
2. 找到 `:root` 块（第 1 行附近）
3. 修改：
   ```css
   --accent: #4a90d9;      /* 原 #c9a84c */
   --accent-glow: #4a90d9; /* 原 #c9a84c */
   --gold: #4a90d9;        /* 可选：同步金色系 */
   --gold-dark: #2d6a9f;   /* 暗金→暗蓝 */
   ```
4. 刷新浏览器，全站颜色同步更新
5. 如需保留金色作为辅助色，可只改 `--accent`，不改 `--gold`

---

## 四、规范提醒

- **禁止在 `.html` 中添加 inline style**；如需局部覆盖，添加至对应 CSS 文件
- **禁止硬编码时间值**（如 `.3s`、`200ms`）在 transition 属性中，使用 `--dur-*` 令牌
- **断点统一使用 `900px` 和 `640px`**，不引入新断点
- **字体文件放 `frontend/static/fonts/`，通过 `fonts.css` 引用**

---

**维护责任**：修改颜色变量时，同步检查暗色主题块中的对应变量是否也需要调整。
