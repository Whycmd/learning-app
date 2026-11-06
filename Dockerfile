# ============================================================
# 英语学习应用 · Docker 镜像
#
# 这个文件做两件事，分成两个阶段：
#
#   阶段 1（builder）：装 C++ 编译器，把 backend/src/*.cpp 编译成可执行文件
#   阶段 2（runtime）：只装运行必需的东西，把编译好的程序和页面拷进来
#
# 为什么分两阶段？
#   编译器 + 开发头文件大约 500 MB，程序跑起来完全用不到它们。
#   分开之后，最终镜像小很多，别人下载和启动都更快。
#
# 基础镜像 node:20-slim 是 Debian 12（bookworm）的精简版，自带 Node.js 20。
# AI 网关（scripts/local_ai_server.js）和 Word 导出（docx / markdown-docx）
# 都靠这个 Node 环境运行。
# ============================================================

# ---------- 阶段 1：编译 C++ 后端 ----------
# 基础镜像地址可以通过 --build-arg 覆盖（见 docker-compose.yml 的 build.args）。
# 默认带国内加速器前缀，原因：Docker Hub 官方仓库在国内经常连不上，
# 直接写 node:20-slim 会导致 docker build 卡在拉取镜像这一步。
ARG BASE_IMAGE=docker.m.daocloud.io/library/node:20-slim

FROM ${BASE_IMAGE} AS builder

# 只装编译需要的三样东西：
#   g++                        C++ 编译器
#   make                       编译工具
#   default-libmysqlclient-dev MySQL 客户端库的头文件，并提供 mysql_config 查询编译参数
# 装完立刻删除 apt 缓存，避免把无用的包列表打进镜像
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      g++ make default-libmysqlclient-dev \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /build

# 先只拷"后端源码 + 构建脚本"。
# 这样以后只改前端页面时，这一层缓存不会失效，重新构建会快很多。
COPY backend/ ./backend/
COPY scripts/build_backend.sh ./scripts/

# 直接调用项目自带的构建脚本，而不是在这里重写一遍编译命令。
# 好处：Docker 里的编译参数和 macOS 本机完全一致，不会出现"两边编译方式不同"的问题。
RUN bash scripts/build_backend.sh

# ---------- 阶段 2：运行时 ----------
# 和阶段 1 用同一个基础镜像，保证两边的系统库版本一致，
# 避免出现"编译时链接的库在运行时找不到"这类问题。
FROM ${BASE_IMAGE}

# 运行时只需要两个库：
#   libmariadb3  MySQL 客户端动态库（阶段 1 编译时链接的就是它）
#   curl         健康检查、调试时用
RUN apt-get update \
 && apt-get install -y --no-install-recommends libmariadb3 curl mariadb-client unzip \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# ---- 安装 Node 依赖 ----
# package-lock.json 如果存在，就用 npm ci：版本完全锁定，每次构建结果一致。
# 如果不存在（比如 clone 下来没有 lock 文件），退回 npm install。
COPY package.json package-lock.json* ./
RUN if [ -f package-lock.json ]; then npm ci --omit=dev; else npm install --omit=dev; fi

# ---- 拷贝程序和数据 ----
# 编译好的后端可执行文件，从阶段 1 直接搬过来
COPY --from=builder /build/backend/build/eng_app_server ./backend/build/eng_app_server

# 后端启动时读取的 EFLLex 词表（13,546 条）
COPY backend/data/ ./backend/data/
# 前端页面与静态资源（字体、样式、脚本）
COPY frontend/ ./frontend/
# 脚本：AI 网关、Word 导出、数据库巡检等
COPY scripts/ ./scripts/
# 测试套件：随镜像一起进去，方便在容器内跑 node tests/run_all.js 做多平台一致性验证
COPY tests/ ./tests/
# 配置文件模板（config/ai.json 被 .dockerignore 排除，必须由使用者自己提供 Key）
COPY config/ ./config/
# 容器启动脚本
COPY docker/entrypoint.sh /usr/local/bin/entrypoint.sh

# 建两个运行时目录：
#   word_tmp  导出的 Word 文件临时落脚处
#   logs      日志目录
RUN mkdir -p /app/word_tmp /app/logs \
 && chmod +x /usr/local/bin/entrypoint.sh /app/tests/_mysql_tcp_shim.sh

# 告诉后端"项目根目录在哪"。后端靠 APP_ROOT 找到 config/、frontend/、word_tmp/。
ENV APP_ROOT=/app

# 容器内监听 8088。
# 注意：后端默认只听 127.0.0.1，容器内必须改成 0.0.0.0 才能被端口映射转发出来。
# 对外只暴露到宿主机的 127.0.0.1（由 docker-compose.yml 控制），不会上公网。
EXPOSE 8088

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
