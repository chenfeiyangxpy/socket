# Tiny FTP Server

基于 **epoll I/O 多路复用** + **Reactor 模式**从零实现的轻量级 FTP 服务器，遵循 **RFC 959** 协议标准。同时附带一个交互式命令行 FTP 客户端。

---

## 功能特性

### 核心功能
- **epoll 事件驱动** — 单线程 + epoll Reactor 架构，支持高并发
- **RFC 959 协议** — 完整实现 24 个标准 FTP 命令
- **双数据连接模式** — 支持 **PASV（被动）** 和 **PORT（主动）** 两种数据传输方式
- **断点续传** — `REST` 命令配合 `RETR` / `STOR` 实现文件续传
- **虚拟用户系统** — 基于文件的用户管理，独立家目录
- **路径安全** — 使用 `realpath` + 前缀检查防止目录越狱（`../`）

### 服务端功能
| 命令 | 功能 |
|------|------|
| USER / PASS | 用户登录认证 |
| QUIT | 断开连接 |
| PWD / CWD / CDUP | 目录浏览与切换 |
| LIST / NLST | 列出目录内容（LIST 含权限/大小/时间） |
| RETR / STOR | 文件上传与下载 |
| PASV / PORT | 被动/主动数据连接模式 |
| TYPE A / I | ASCII / 二进制传输模式 |
| REST | 断点续传位置标记 |
| MKD / RMD / DELE | 目录创建、删除、文件删除 |
| RNFR / RNTO | 文件重命名 |
| SIZE | 查询文件大小 |
| SYST / FEAT | 系统信息与扩展特性声明 |
| NOOP / HELP | 心跳保持与帮助 |

### 拓展特性
- 配置文件驱动（监听端口、数据端口范围、最大连接数、超时等）
- Daemon 守护进程模式（`-d` 参数）
- 命令行参数覆盖（`-p` 指定端口，`-c` 指定配置文件）
- 断点续传支持
- 详细日志输出

---

## 架构设计

```
┌──────────────────────────────────────────────────┐
│                   epoll_wait                       │
│       ┌──────────┬──────────┬──────────┐          │
│       │ listen   │ client_1 │ client_2 │  ...     │
│       │  (accept) │  (read)  │  (read)  │          │
│       └─────┬────┴────┬─────┴────┬─────┘          │
│             │         │          │                  │
│             ▼         ▼          ▼                  │
│       session_create  cmd_dispatch  cmd_dispatch    │
│       welcome msg     session_reply session_reply   │
└──────────────────────────────────────────────────┘
```

### 分层说明

| 层次 | 文件 | 职责 |
|------|------|------|
| **Reactor** | `reactor.h/cpp` | epoll 事件循环封装（add / mod / del / run） |
| **Session** | `session.h/cpp` | 每连接状态机、命令缓冲区、响应缓存、路径安全 |
| **Commands** | `commands.h/cpp` | 24 个命令处理函数 + 分发表 |
| **Data Conn** | `data_conn.h/cpp` | PASV 监听 / accept、PORT 连接、端口解析 |
| **Config** | `config.h/cpp` | 配置文件解析（键值对格式） |
| **Auth** | `auth.h/cpp` | 虚拟用户文件认证 |
| **Server** | `server.cpp` | 入口函数、daemon 化、信号处理、主事件循环 |

### 数据流（以 LIST 命令为例）

```
1. Client  →   PASV        →  Server   创建监听socket，返回IP:Port
2. Client  →   LIST        →  Server   发送150，accept数据连接
3. Client  ⇄  数据连接     ⇄  Server   发送文件列表
4. Server  →   226 OK      →  Client   传输完成
```

---

## 快速上手

### 环境要求

- Linux (Ubuntu 22.04+ / CentOS 7+)
- g++ 支持 C++11
- make

### 编译

```bash
make clean && make
```

编译产物：
- `ftpd` — FTP 服务器
- `ftp` — FTP 客户端

### 配置

**准备目录与配置文件：**

```bash
# 创建用户家目录
sudo mkdir -p /srv/ftp/user1 /srv/ftp/test /srv/ftp/admin

# 创建配置文件目录
sudo mkdir -p /etc/ftpd

# 复制配置文件
sudo cp ftpd.conf /etc/ftpd/
sudo cp users.conf /etc/ftpd/
```

**编辑用户文件 `/etc/ftpd/users.conf`（格式：`用户名:密码:家目录`）：**

```
ftpuser:password123:/srv/ftp/user1
test:test1234:/srv/ftp/test
admin:admin123:/srv/ftp/admin
```

### 运行服务器

```bash
# 前台运行（调试模式，Ctrl+C 停止）
sudo ./ftpd -c /etc/ftpd/ftpd.conf

# 后台运行（daemon模式）
sudo ./ftpd -d -c /etc/ftpd/ftpd.conf

# 指定端口（覆盖配置文件）
sudo ./ftpd -p 2121 -c ftpd.conf
```

### 使用客户端连接

```bash
# 方式一：启动后输入 open
./ftp
ftp> open 127.0.0.1 21

# 方式二：命令行直接连接
./ftp 127.0.0.1 21
```

**客户端操作示例：**

```
ftp> user test
ftp> pass test1234
ftp> pasv             ← 进入被动模式
ftp> list             ← 列出文件
ftp> pwd              ← 当前目录
ftp> cwd /tmp         ← 切换目录
ftp> get readme.txt   ← 下载文件（PASV后）
ftp> put local.txt    ← 上传文件（PASV后）
ftp> syst             ← 系统信息
ftp> quit             ← 退出
```

---

## 项目文件说明

| 文件 | 说明 |
|------|------|
| `server.cpp` | 主入口：daemon/信号/命令行参数/epoll事件循环 |
| `session.h / session.cpp` | FTP 会话状态机、路径解析与安全校验 |
| `commands.h / commands.cpp` | RFC 959 命令分发表与 24 个处理器 |
| `reactor.h / reactor.cpp` | epoll 事件循环封装 |
| `data_conn.h / data_conn.cpp` | PASV / PORT 数据连接管理 |
| `config.h / config.cpp` | 配置文件解析器 |
| `auth.h / auth.cpp` | 虚拟用户文件认证 |
| `client.cpp` | 交互式命令行 FTP 客户端 |
| `ftpd.conf` | 服务端配置示例 |
| `users.conf` | 虚拟用户文件示例 |
| `Makefile` | 编译构建 |

---

## 简历面试价值

### 项目亮点

| 技术点 | 面试常问方向 |
|--------|-------------|
| **epoll** | ET / LT 模式区别、select/poll/epoll 对比、水平触发与边缘触发 |
| **Reactor 模式** | 事件驱动设计、Reactor / Proactor 区别、单线程 vs 多线程 Reactor |
| **非阻塞 I/O** | EAGAIN 处理、write/read 部分写入、缓冲区管理 |
| **TCP 协议** | 三次握手、四次挥手、time_wait、TCP_NODELAY |
| **Linux 系统编程** | socket、fcntl、信号处理、daemon 进程、fork |
| **FTP 协议** | RFC 959、控制连接与数据连接分离、PASV vs PORT、断点续传 |

### 可深入方向

- 支持 SSL/TLS 加密（FTPS）
- 多线程 / 线程池 Reactor
- 支持 `IPv6`
- 传输速率限制（带宽控制）
- 日志系统（syslog）
- 配置文件热加载（SIGHUP）

---

## 许可

MIT License
