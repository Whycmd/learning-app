# Windows 适配 · 实施流程

> 版本：draft-v0.1 | 创建：2026-10-04
> 本文档为**操作流程指南**，不写具体代码，列出每一步要做的事情与交付物。

---

## 一、目标与边界

**目标**：让现有软件（C++ 后端 + Node.js 网关 + 原生前端 + MySQL）能在 Windows 10/11（x64）上正常运行，无需 WSL。

**边界**：
- 功能与行为与 macOS 版完全一致（同一套测试套件通过）
- 配置文件格式不变（JSON）
- 不引入跨平台框架（保持原生，不迁移到 Qt/Electron 等）

---

## 二、整体流程总览

```
阶段 1：环境准备
    ↓
阶段 2：构建系统改造（CMake）
    ↓
阶段 3：C++ 代码跨平台适配
    ↓
阶段 4：脚本平台化（PowerShell / .bat）
    ↓
阶段 5：配置动态化（路径参数化）
    ↓
阶段 6：Windows 专属测试套件
    ↓
阶段 7：安装包打包 + 文档更新
```

---

## 三、各阶段详细步骤

---

### 阶段 1：环境准备

| 步骤 | 内容 | 交付物 |
|------|------|--------|
| 1-1 | 确认目标 Windows 版本（Win10/11 x64）与系统要求 | 记录在流程文件首行 |
| 1-2 | 准备 Windows 开发环境：安装 Visual Studio Build Tools（含 MSVC）或 MinGW-w64 | 确认 `cl.exe` 或 `g++.exe` 可用 |
| 1-3 | 确认 Node.js v18+ 在 Windows 上可运行（下载官网安装包） | `node -v` 输出正常 |
| 1-4 | 确认 MySQL 8.0 Windows 版可安装并本地启动 | 获取 Windows MSI 安装包链接 |
| 1-5 | 确认 MySQL C 客户端库（`libmysql.lib` / `libmysql.dll`）可在 Windows 上使用 | 下载 Connector/C for Windows |
| 1-6 | 检查项目是否已有 `CMakeLists.txt`；若无则创建最小可用版本 | 判断是否需要新建 |

---

### 阶段 2：构建系统改造

| 步骤 | 内容 | 交付物 |
|------|------|--------|
| 2-1 | 分析现有 `scripts/build_backend.sh` 中所有编译参数（含 `clang++` 专属选项） | 列出需替换的参数 |
| 2-2 | 编写 `backend/CMakeLists.txt`，支持 MSVC 与 GCC/Clang 两种工具链 | 一份 CMakeLists 文件 |
| 2-3 | CMakeLists 中定义平台条件宏（`_WIN32` 分支）处理 socket/mkdir 等差异 | 条件编译逻辑 |
| 2-4 | 验证 CMake 在 Windows 上可生成 `.sln` 或命令行构建命令 | 本地测试一次 |
| 2-5 | 保留原有 `build_backend.sh` 作为 macOS/Linux 入口，CMakeLists 作为通用入口 | 双入口并存 |

---

### 阶段 3：C++ 代码跨平台适配

| 步骤 | 内容 | 交付物 |
|------|------|--------|
| 3-1 | 梳理所有 `#include <unistd.h>`、`#include <sys/socket.h>`、`#include <netinet/in.h>` 等 POSIX 头文件 | 列出需 `#ifdef _WIN32` 的文件清单 |
| 3-2 | 为每个需适配的文件添加平台守卫宏（`_WIN32` / `__linux__` / `__APPLE__`） | 各文件顶部新增宏块 |
| 3-3 | socket 相关：将 `ai_provider.cpp` 中的 socket/send/recv 改为使用 Winsock2（WSAStartup）或保留 POSIX 接口但通过宏隔离 | 统一 Socket 抽象层 |
| 3-4 | 文件系统操作：`mkdir` 调用改为跨平台版本（Windows 用 `_wmkdir` 或 `CreateDirectoryW`） | 封装 `file_utils.h` |
| 3-5 | 随机数生成：`/dev/urandom` 替换为 Windows CryptoAPI（`CryptGenRandom`）或 Portable 开源实现（如 OpenSSL RAND） | 统一 `random_utils.h` |
| 3-6 | SHA-256：`CommonCrypto`（macOS）替换为跨平台实现（OpenSSL 或 BearSSL） | 替换 crypto 依赖 |
| 3-7 | 确认 `main.cpp` 中硬编码的绝对路径（约 4 处）全部改为从配置读取，不在此阶段修改路径逻辑 | 标记为待阶段 5 处理 |
| 3-8 | 验证 C++ 代码在 Windows 上编译无报错 | 编译产物 `eng_app_server.exe` |

---

### 阶段 4：脚本平台化

| 步骤 | 内容 | 交付物 |
|------|------|--------|
| 4-1 | 盘点全部 `.sh` 脚本（共 4 个）：`build_backend.sh`、`stop_all.sh`、`start_mysql.sh`、`start_localai.sh` | 脚本清单 |
| 4-2 | 为每个 `.sh` 创建对应 `.ps1`（PowerShell）版本，路径分隔符统一用 `Join-Path` | 4 个 `.ps1` 文件 |
| 4-3 | 同步创建 `.bat` 版本（兼容无 PowerShell 环境）或仅保留 `.ps1` + `start_all.bat`（调用 PowerShell） | 至少一套 Windows 可执行脚本 |
| 4-4 | 验证 `.ps1` 在 Windows 上执行正常（设置执行策略 `Set-ExecutionPolicy`） | 测试截图或记录 |
| 4-5 | `scripts/start_all.sh`（一键启动）新增对当前 OS 的判断，自动调用对应脚本 | 修改 `start_all.sh` 或新增 `start_all.bat` |

---

### 阶段 5：配置动态化

| 步骤 | 内容 | 交付物 |
|------|------|--------|
| 5-1 | 定位所有硬编码的 macOS 绝对路径（已知约 17 处，分布在 4 个 `.sh` + `main.cpp`） | 路径清单表格 |
| 5-2 | 后端启动时从 `config/app.json` 读取项目根目录，不再硬编码 | 修改 `main.cpp` |
| 5-3 | `config/db.json` 新增 `data_dir`、`log_dir`、`sock_path` 字段，默认值兼容 Windows | 修改 `db.json` 结构说明 |
| 5-4 | Node.js 网关脚本（`local_ai_server.js`）改用相对路径或环境变量，不再硬编码 `~/.local` | 修改 `scripts/local_ai_server.js` |
| 5-5 | 临时目录（`word_tmp/`）改为从 `config/app.json` 读取 | 新增字段 + 代码同步 |
| 5-6 | 验证在 Windows 上配置文件修改后启动不报错 | 测试记录 |

---

### 阶段 6：Windows 专属测试套件

| 步骤 | 内容 | 交付物 |
|------|------|--------|
| 6-1 | 现有 12 套测试在 Windows 上运行一次，记录失败项 | 失败项清单 |
| 6-2 | 针对平台差异新增 `stage_windows.test.js`：验证路径读取、端口绑定、服务启动 | 新测试文件 |
| 6-3 | 并发测试（当前使用串行 curl）在 Windows 上用 PowerShell 并行发起请求验证 | 并发验证报告 |
| 6-4 | AI 网关在 Windows 上运行一次完整固化/出题/批改流程 | 功能验证记录 |
| 6-5 | 浏览器走查：Windows 上打开 http://127.0.0.1:8088 验证所有页面正常加载 | 截图存档 |

---

### 阶段 7：安装包打包 + 文档更新

| 步骤 | 内容 | 交付物 |
|------|------|--------|
| 7-1 | 编写 `installer/` 目录：包含 MySQL 安装包链接、Node.js 安装包链接、后端 exe、前端静态文件 | 安装介质包 |
| 7-2 | 编写 Windows 版 `README-WINDOWS.md`：环境要求、安装步骤、常见问题 | 新文档 |
| 7-3 | 更新 `AGENTS.md` 第十节常用命令，补充 Windows 版本（`.ps1` / `.bat` 后缀） | AGENTS.md 变更 |
| 7-4 | 更新 `config/app.json` 版本号（升 PATCH 或 MINOR，视改动规模而定） | app.json 变更 |
| 7-5 | 更新 devlog 任务记录 `v{版本}-T{序号}-Windows适配/` | 新 devlog 目录 |

---

## 四、平台差异速查表（供开发参考）

以下列出本项目在 Windows 上可能遇到的已知差异点，实施时按此逐项确认：

| 项目 | macOS/Linux | Windows | 处理方式 |
|------|------------|---------|---------|
| 编译器 | `clang++` | `cl.exe`（MSVC）或 `g++.exe`（MinGW） | CMake 自动选择 |
| 路径分隔符 | `/` | `\` | 统一使用 `std::filesystem::path` |
| 行结束符 | LF | CRLF | 编辑时注意，Git `autocrlf=true` |
| 随机数源 | `/dev/urandom` | Win32 `CryptGenRandom` | 抽象接口替换 |
| 密码学库 | CommonCrypto | OpenSSL / BearSSL | 抽象接口替换 |
| Socket | POSIX socket | Winsock2（需 WSAStartup） | `#ifdef _WIN32` 隔离 |
| 目录创建 | `::mkdir(path, mode)` | `_wmkdir` 或 `CreateDirectoryW` | 封装统一函数 |
| 进程启动脚本 | `.sh`（bash） | `.ps1` 或 `.bat` | 新增 PowerShell 版本 |
| MySQL 客户端库 | `libmysqlclient.so` | `libmysql.lib` + `libmysql.dll` | CMake 指定库路径 |
| 日志目录 | `~/.local/log/` | `%APPDATA%\learning-app\log\` | 配置动态化 |
| PID 文件目录 | `~/.local/run/` | `%TEMP%\learning-app\` | 配置动态化 |

---

## 五、里程碑与验收标准

| 里程碑 | 验收标准 |
|--------|---------|
| M1：编译通过 | Windows 上生成 `eng_app_server.exe`，无编译错误 |
| M2：服务启动 | 双击 `start_all.bat` 后 MySQL + AI 网关 + 后端均正常运行，端口 8088 可访问 |
| M3：功能正常 | 所有 12 套现有测试 + stage_windows 全部通过 |
| M4：文档齐全 | `README-WINDOWS.md` + AGENTS.md 更新完毕 |
| M5：安装包可用 | 提供可分发的压缩包，用户按文档步骤安装后首次启动成功 |

---

## 六、风险评估

| 风险 | 可能性 | 影响 | 缓解措施 |
|------|--------|------|---------|
| MySQL Windows 版与 macOS 版数据格式不兼容 | 低 | 中 | 统一使用 utf8mb4，迁移时用 SQL 文件导出/导入 |
| Winsock2 与现有 socket 代码冲突 | 中 | 高 | 提前在阶段 3-3 做统一抽象，避免混用 |
| CMake 工具链配置复杂导致构建失败 | 中 | 中 | 提供预编译的 `CMakeSettings.json` 供 Visual Studio 直接打开 |
| PowerShell 执行策略限制脚本运行 | 低 | 低 | 提供 `.bat` 备用方案；文档中写明执行策略设置步骤 |
| DLL 依赖（libmysql.dll）找不到 | 中 | 高 | 打包时将 DLL 与 exe 放同目录，或加入 PATH |
