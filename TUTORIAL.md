# socket3 项目从零学习教程

本教程面向有 C/C++ 基础但想深入理解 Linux 网络编程的同学，带你从零看懂这个 FTP 项目。

---

## 目录

1. [前置知识](#1-前置知识)
2. [环境搭建](#2-环境搭建)
3. [项目整体架构](#3-项目整体架构)
4. [模块一：TCP Socket 基础 — 从 client.cpp 开始](#4-模块一tcp-socket-基础--从-clientcpp-开始)
5. [模块二：epoll + Reactor — 理解 server.cpp](#5-模块二epoll--reactor--理解-servercpp)
6. [模块三：FTP 协议 — 理解 commands.cpp](#6-模块三ftp-协议--理解-commandscpp)
7. [模块四：数据连接 — 理解 data_conn.cpp](#7-模块四数据连接--理解-data_conncpp)
8. [模块五：配置文件与认证](#8-模块五配置文件与认证)
9. [完整流程串联](#9-完整流程串联)
10. [动手练习](#10-动手练习)
11. [面试知识图谱](#11-面试知识图谱)

---

## 1. 前置知识

### 需要的背景

| 知识点 | 需要掌握到什么程度 | 建议学习资源 |
|--------|-------------------|-------------|
| C 语言基础 | 指针、结构体、函数指针、字符串操作 | 《C Primer Plus》前 15 章 |
| Linux 基础 | gcc/g++ 编译、make、文件操作、终端 | 会用 Linux 写代码即可 |
| 计算机网络 | TCP/IP 协议栈、端口、IP 地址 | 《计算机网络：自顶向下》第 3 章 |

### 不需要的知识（项目会教会你）

- epoll — 项目核心，我会详细讲解
- FTP 协议 — 项目实现了 RFC 959，边读边学
- Reactor 模式 — 跟着代码理解事件驱动

---

## 2. 环境搭建

### 2.1 克隆项目

```bash
git clone https://github.com/chenfeiyangxpy/socket.git socket3
cd socket3
```

### 2.2 编译

```bash
make clean && make
```

### 2.3 准备运行环境

```bash
# 创建用户家目录
sudo mkdir -p /srv/ftp/user1 /srv/ftp/test /srv/ftp/admin

# 复制配置文件
sudo mkdir -p /etc/ftpd
sudo cp ftpd.conf /etc/ftpd/
sudo cp users.conf /etc/ftpd/
```

### 2.4 运行

**终端 1 — 启动服务器：**

```bash
sudo ./ftpd -c /etc/ftpd/ftpd.conf
```

**终端 2 — 启动客户端：**

```bash
./ftp 127.0.0.1 21
ftp> user test
ftp> pass test1234
ftp> list
```

### 2.5 推荐学习工具

- **Wireshark** — 抓包分析 FTP 协议交互过程
- **strace** — `sudo strace -p <ftpd_pid>` 查看系统调用
- **gdb** — `gdb ./ftpd` 断点调试

---

## 3. 项目整体架构

### 3.1 分层架构图

```
┌─────────────────────────────────────────────────────────┐
│                    Main 入口 (server.cpp)                  │
│         命令行参数解析 / daemon化 / 信号处理                 │
├─────────────────────────────────────────────────────────┤
│                Reactor 层 (reactor.cpp)                   │
│         epoll_create / epoll_ctl / epoll_wait 封装         │
├─────────────────────────────────────────────────────────┤
│                Session 层 (session.cpp)                   │
│       连接状态机 / 命令缓冲区 / 路径安全 / 响应发送          │
├─────────────────────────────────────────────────────────┤
│             Commands 层 (commands.cpp)                    │
│      FTP 命令分发表 + 24 个命令处理函数                    │
├──────────────┬──────────────────────────────────────────┤
│ Data Conn    │  Config        │  Auth                     │
│ (data_conn)  │  (config)      │  (auth)                   │
│ PASV / PORT  │  配置文件解析   │  虚拟用户认证              │
└──────────────┴────────────────┴──────────────────────────┘
```

### 3.2 学习路线建议

按这个顺序读代码，从最容易的入手：

1. **client.cpp** — TCP socket 编程入门
2. **config.cpp** — 最简单的模块
3. **auth.cpp** — 文件解析 + 字符串比较
4. **data_conn.cpp** — 数据通道管理
5. **reactor.cpp** — epoll 封装
6. **session.cpp** — 会话管理
7. **commands.cpp** — FTP 协议实现（最核心）
8. **server.cpp** — 主事件循环（最后看，串联所有模块）

---

## 4. 模块一：TCP Socket 基础 — 从 client.cpp 开始

### 4.1 阅读目标

理解 TCP 客户端完整流程：
`socket()` → `connect()` → `send()` / `recv()` → `close()`

### 4.2 代码地图

[client.cpp](file:///d:/token3/socket3/client.cpp) 约 530 行，结构如下：

| 行号 | 内容 | 核心知识点 |
|------|------|-----------|
| 1-20 | 头文件 | socket 相关头文件 |
| 24-36 | 全局变量 | ctrl_fd, data_fd, recv_buf |
| 38-52 | `send_cmd()` | 向服务器发送 FTP 命令 |
| 55-95 | `read_response()` | 读取服务器响应，处理粘包 |
| 96-145 | `connect_data_pasv()` / `close_data()` | 数据连接管理 |
| 150-280 | `cmd_retr()` / `cmd_stor()` | 文件下载/上传逻辑 |
| 284-468 | `process_line()` | 命令解析与分发 |
| 473-508 | `main()` | 主循环 |

### 4.3 关键知识点

#### 知识点 1：socket 创建与连接 (main 函数)

```c
// 1. 创建 socket（第 461 行）
ctrl_fd = socket(AF_INET, SOCK_STREAM, 0);

// 2. 准备服务器地址结构体
struct sockaddr_in sa;
memset(&sa, 0, sizeof(sa));         // 清零
sa.sin_family = AF_INET;            // IPv4
sa.sin_port = htons(server_port);   // 端口（网络字节序）
inet_pton(AF_INET, server_ip, &sa.sin_addr);  // IP 字符串→二进制

// 3. 连接服务器（第 476 行）
connect(ctrl_fd, (struct sockaddr*)&sa, sizeof(sa));
```

**面试常问：**
- 为什么用 `sockaddr_in` 但 `connect` 参数是 `sockaddr*`？— 这是 C 语言多态的模拟方式
- `htons()` 是做什么的？— 主机字节序转网络字节序（大端）
- `inet_pton` 和 `inet_ntoa` 区别？— pton: 字符串→二进制, ntoa: 二进制→字符串

#### 知识点 2：粘包处理 (read_response)

TCP 是流协议，没有消息边界。一次 `send("USER test\r\n")` 可能和后面的 `send("PASS xxx\r\n")` 一起收到。

```c
// 第 58-92 行：read_response 的核心逻辑
// 每次读数据追加到 recv_buf
// 检查最后一行是否以 "NNN " 开头（完整响应标志）
// 如果是，说明收到了一条完整响应
```

**为什么这样能判断完整？**

FTP 协议规定响应格式：`XXX 消息\r\n`（单行）或 `XXX-...\r\n...\r\nXXX ...\r\n`（多行）。
最后一行一定是 `三位数字码 + 空格 + 正文`，`read_response` 就靠这个判断。

#### 知识点 3：PASV 连接 (connect_data_pasv)

```c
// 第 95-133 行
// 解析服务器返回的 "227 Entering Passive Mode (127,0,0,1,117,48)"
// 括号里是 IP 和端口（高8位, 低8位）
sscanf(p, "%u,%u,%u,%u,%u,%u", &h1, &h2, &h3, &h4, &p1, &p2);
data_port = (p1 << 8) | p2;  // 合并端口号

// 创建新的 socket 连接数据通道
int fd = socket(AF_INET, SOCK_STREAM, 0);
connect(fd, ...);  // 连接服务器数据端口
```

### 4.4 动手实验

1. 把 `htons(server_port)` 改成 `server_port`（不转字节序）会怎样？
2. 注释掉 `read_response()` 的粘包判断，看有什么现象
3. 在 `connect_data_pasv` 里加 `printf` 打印解析出的 IP 和端口

---

## 5. 模块二：epoll + Reactor — 理解 server.cpp

### 5.1 阅读目标

理解 I/O 多路复用概念和 Reactor 事件驱动模式，这是项目最核心的设计。

### 5.2 为什么需要 epoll？

**传统多线程方案：** 每个客户端一个线程。100 个客户端 → 100 个线程。线程切换开销大。

**epoll 方案：** 一个线程同时监听上百个 socket。哪个 socket 有数据来了就去处理哪个。就像餐厅里一个服务员同时服务所有桌子，而不是每桌配一个服务员。

**对比：**

| 方式 | 模型 | 100 个客户端 |
|------|------|-------------|
| 多线程 | 1 连接 : 1 线程 | 100 线程 |
| epoll | N 连接 : 1 线程 | 1 线程 + 1 epoll fd |

### 5.3 reactor.cpp 核心

[reactor.cpp](file:///d:/token3/socket3/reactor.cpp) 只有 65 行：

```c
// 1. 创建 epoll 实例（第 12 行）
int epfd = epoll_create1(0);

// 2. 添加事件关注（第 26 行）
struct epoll_event ev;
ev.events = EPOLLIN | EPOLLET;    // 可读事件 + 边缘触发
ev.data.ptr = session;             // 关联的自定义数据
epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);

// 3. 等待事件（第 36 行）
int nfds = epoll_wait(epfd, events, MAX_EVENTS, timeout);
// events 数组里就是就绪的 socket 和关联数据

// 4. 修改/删除事件
epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);  // 修改（如增加写关注）
epoll_ctl(epfd, EPOLL_CTL_DEL, fd, &ev);  // 删除
```

### 5.4 关键知识点

#### ET vs LT

[reactor.cpp](file:///d:/token3/socket3/reactor.cpp#L27) 使用 `EPOLLET`（边缘触发）：

| | 水平触发 LT（默认） | 边缘触发 ET |
|--|-------------------|------------|
| 数据到达时 | 一直通知直到读完 | 只通知一次 |
| 必须读完？ | 不强制 | 必须一次读完（循环读直到 EAGAIN）|
| 优点 | 编程简单 | 效率高，事件少 |
| 缺点 | 重复通知 | 必须用非阻塞 I/O + 循环读 |

**代码体现：**

```c
// server.cpp 读数据处理（第 309 行附近）
while (1) {
    n = read(s->ctrl_fd, s->cmd_buf + s->cmd_len, 
             sizeof(s->cmd_buf) - s->cmd_len - 1);
    if (n <= 0) {
        if (errno == EAGAIN) break;  // 数据读完了，退出循环
        // 连接关闭
        goto disconnect;
    }
    s->cmd_len += n;
}
```

因为用了 ET，所以必须 `while` 循环读到 `EAGAIN`，否则剩下的数据不会再通知。

#### Reactor 主循环

[server.cpp](file:///d:/token3/socket3/server.cpp) 主循环逻辑：

```
while (1) {
    epoll_wait 等待事件
    
    for (每个就绪的事件) {
        if (是监听 socket) {
            accept 新连接
            创建 FtpSession
            注册到 epoll
        }
        else if (是可读事件) {
            读取客户端命令
            cmd_dispatch 分发执行
        }
        else if (是可写事件) {
            发送响应数据
        }
    }
}
```

**这就是 Reactor 模式**：把所有 I/O 事件注册到 epoll，由 epoll 告诉程序"哪个 socket 可以干什么了"，程序再去做对应操作。

### 5.5 动手实验

1. 用 `strace -e epoll_ctl,epoll_wait -p <ftpd_pid>` 观察 epoll 系统调用
2. 把 `EPOLLET` 改成 0（水平触发），看看有什么变化
3. 开两个客户端同时连接，观察服务端怎么同时处理

---

## 6. 模块三：FTP 协议 — 理解 commands.cpp

### 6.1 阅读目标

理解 FTP 协议的命令-响应模型，以及每个命令的具体实现。

### 6.2 FTP 协议基础

FTP 使用**控制连接**和**数据连接**分离的架构：

```
控制连接（端口 21）：传输命令和响应，文本格式
    Client → Server:  USER test\r\n
    Server → Client:  331 User name okay, need password.\r\n

数据连接（临时端口）：传输文件和目录列表，二进制格式
    Server → Client:  文件内容...
```

**FTP 命令格式：** `命令 [参数]\r\n`
**FTP 响应格式：** `三位数字码 [空格/-] 消息\r\n`

```
1xx: 已接受，继续
2xx: 成功
3xx: 需要更多信息
4xx: 临时错误
5xx: 永久错误
```

### 6.3 命令分发表

[commands.cpp](file:///d:/token3/socket3/commands.cpp#L741-L767) 最巧妙的设计：

```c
typedef int (*CommandHandler)(FtpSession*, const char*);

typedef struct {
    const char *name;           // 命令名
    CommandHandler handler;     // 处理函数
    int need_login;             // 是否需要登录
} CommandEntry;

CommandEntry cmd_table[] = {
    { "USER",  cmd_USER,  0 },   // 不需要登录
    { "PASS",  cmd_PASS,  0 },
    { "QUIT",  cmd_QUIT,  0 },
    { "LIST",  cmd_LIST,  1 },   // 需要登录
    { "RETR",  cmd_RETR,  1 },
    // ... 共 24 个命令
    { NULL,    NULL,      0 }    // 结束标记
};
```

**函数指针数组** — 根据命令名找到对应的处理函数。没有长长的 `if-else`，而是查表。

### 6.4 典型的命令执行流程（以 LIST 为例）

```
1. 客户端发送 PASV → cmd_PASV()
   - 创建监听 socket
   - 返回端口号给客户端

2. 客户端连接数据端口
   - 客户端 connect 到服务器指定端口
   - 服务器在 data_conn_accept_pasv() 中 accept

3. 客户端发送 LIST → cmd_LIST()
   - 检查是否已建立数据连接
   - 解析路径参数
   - accept 数据连接（对于 PASV 模式）
   - 回复 "150 Opening data connection"
   - send_list_data()：遍历目录，发送文件列表
   - 关闭数据连接
   - 回复 "226 Directory send OK"
```

### 6.5 面试高频考察点

#### 路径安全（面试常考）

[commands.cpp](file:///d:/token3/socket3/commands.cpp#L186-L207) CWD 命令：

```c
int cmd_CWD(FtpSession *s, const char *arg) {
    char abs_path[1024];
    session_resolve_path(s, arg, abs_path, sizeof(abs_path));
    
    if (!session_path_safe(s, abs_path)) {
        session_reply(s, "550 Access denied.\r\n");  // ← 防止 ../ 越狱
        return 0;
    }
    // ... 切换目录
}
```

[session.cpp](file:///d:/token3/socket3/session.cpp#L107-L126) 安全校验：

```c
int session_path_safe(FtpSession *s, const char *abs_path) {
    char real_home[PATH_MAX];
    realpath(s->home_dir, real_home);     // 获取家目录绝对路径
    
    char *real_path = realpath(abs_path, NULL);
    // 检查目标路径是否以家目录开头
    if (strncmp(real_path, real_home, strlen(real_home)) != 0)
        return 0;  // 不是家目录下的路径 → 拒绝
    return 1;
}
```

**为什么需要这个？** 用户输入 `CWD ../../etc/passwd` 如果直接拼接，就能越狱到系统目录。`realpath` + 前缀检查可以防止这种攻击。

#### 断点续传

[REST 命令](file:///d:/token3/socket3/commands.cpp#L380-L395)：

```c
int cmd_REST(FtpSession *s, const char *arg) {
    off_t pos = (off_t)atoll(arg);
    s->data_conn.restart_pos = pos;  // 记录断点位置
    session_reply(s, "350 Restart position accepted.\r\n");
}
```

[open_file_for_retr](file:///d:/token3/socket3/commands.cpp#L99-L120) 中：

```c
if (s->data_conn.restart_pos > 0) {
    lseek(fd, s->data_conn.restart_pos, SEEK_SET);  // 跳过已下载的部分
}
```

### 6.6 动手实验

1. 用 Wireshark 抓包，观察 LIST 命令的完整交互过程
2. 在 `cmd_MKD` 里加 `printf` 打印解析出的绝对路径
3. 尝试 `CWD ../../`，看服务器返回什么

---

## 7. 模块四：数据连接 — 理解 data_conn.cpp

### 7.1 阅读目标

理解 FTP 控制连接和数据连接分离的机制，PASV 和 PORT 两种模式的实现。

### 7.2 核心结构体

[data_conn.h](file:///d:/token3/socket3/data_conn.h)：

```c
typedef struct {
    int mode;            // DATA_NONE / DATA_PASV / DATA_PORT
    int fd;              // 数据连接 socket（用于收发数据）
    int listen_fd;       // PASV 监听 socket
    int listen_port;     // PASV 监听端口
    struct sockaddr_in remote_addr;  // PORT 模式：客户端地址
    int remote_port;     // PORT 模式：客户端端口
    off_t restart_pos;   // 断点续传偏移量
} DataConnection;
```

### 7.3 PASV 被动模式流程

```
客户端                  服务端
  |                      |
  |--- PASV -----------> |  创建监听 socket，绑定随机端口
  |<-- 227 (h1,h2,...) --|  返回 IP 和端口
  |                      |
  |--- connect --------->|  accept 连接
  |--- LIST -----------> |  通过数据连接发送目录列表
  |<-- 226 -------------|  传输完成
```

[实现代码](file:///d:/token3/socket3/data_conn.cpp#L27-L84)：

```c
int data_conn_setup_pasv(DataConnection *dc, int port_min, int port_max) {
    // 1. 创建 socket
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    
    // 2. 在端口范围内尝试 bind
    for (port = port_min; port <= port_max; port++) {
        addr.sin_port = htons(port);
        if (bind(fd, ..., sizeof(addr)) == 0) goto bound;
    }
    
    // 3. 范围内都不可用，让系统自动分配
    addr.sin_port = 0;
    bind(fd, ...);
    
bound:
    listen(fd, 1);  // 开始监听
    // 拿实际分配的端口号
    getsockname(fd, ...);
    dc->listen_fd = fd;
    return port;
}
```

### 7.4 PORT 主动模式流程

```
客户端                  服务端
  |                      |
  |--- PORT (h1,...) --> |  告知自己的 IP:端口
  |<-- 200 OK ----------|  收到
  |                      |
  |--- LIST -----------> |  主动 connect 客户端
  |<-- 150 数据连接 ---  |  
  |<-- 数据 ------------|  发送目录列表
  |<-- 226 完成 --------|  
```

[实现代码](file:///d:/token3/socket3/data_conn.cpp#L118-L145)：

```c
int data_conn_connect_port(DataConnection *dc) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    addr.sin_port = htons(dc->remote_port);
    addr.sin_addr = dc->remote_addr.sin_addr;
    connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    dc->fd = fd;
}
```

### 7.5 动手实验

1. 用 Wireshark 分别抓 PASV 和 PORT 模式的数据包，对比数据连接的方向
2. 为什么 PASV 模式在 `accept` 之后要立刻 `close(listen_fd)`？
3. 如果数据端口范围设置太小会发生什么？

---

## 8. 模块五：配置文件与认证

### 8.1 配置文件解析 (config.cpp)

[ftpd.conf](file:///d:/token3/socket3/ftpd.conf) 格式：

```ini
port = 21
data_port_min = 30000
data_port_max = 30010
root_dir = /srv/ftp
welcome_msg = Welcome to Tiny FTP Server (RFC 959)
```

[config.cpp](file:///d:/token3/socket3/config.cpp) 解析逻辑：

```
读每行 → trim 掉首尾空格 → 跳过空行和注释 →
找 = 分割键值 → value trim → 去掉行尾注释 →
根据 key 名赋值到 g_config 对应字段
```

### 8.2 用户认证 (auth.cpp)

[users.conf](file:///d:/token3/socket3/users.conf) 格式：

```
用户名:密码:家目录
test:test1234:/srv/ftp/test
```

[auth.cpp](file:///d:/token3/socket3/auth.cpp) 认证流程：

```
打开 users.conf
逐行读取
用 sscanf(line, "%[^:]:%[^:]:%[^\n]", user, pass, home) 解析
匹配用户名和密码
成功 → 复制家目录到 user 结构体
失败 → 返回 -1
```

---

## 9. 完整流程串联

### 9.1 从启动到服务一个客户端

```
1. main() 启动
   └─ 解析命令行参数（-d, -p, -c）
   └─ daemon_init()（如果有 -d 参数）
   └─ config_load() 加载配置文件
   └─ reactor_init() 创建 epoll
   └─ 创建监听 socket，bind，listen
   └─ reactor_add() 把监听 socket 加入 epoll
   └─ reactor_run() 进入事件循环

2. 新客户端连接
   └─ epoll_wait 返回 EPOLLIN 事件（监听 socket 可读）
   └─ accept() 创建新 socket
   └─ session_init() 创建 FtpSession
   └─ reactor_add() 把客户端 socket 加入 epoll
   └─ session_reply("220 Welcome...") 发送欢迎消息

3. 客户端发送命令
   └─ epoll_wait 返回 EPOLLIN 事件（客户端 socket 可读）
   └─ 循环 read 直到 EAGAIN
   └─ cmd_dispatch() 解析命令名，查表调用处理函数
   └─ 处理函数执行对应操作
   └─ session_reply() 发送响应

4. 数据传输（以 RETR 下载为例）
   └─ 客户端先 PASV → 服务器创建监听 socket → 返回端口
   └─ 客户端连接数据端口 → accept
   └─ 客户端发送 RETR filename
   └─ session_resolve_path() 解析文件路径
   └─ session_path_safe() 检查路径安全
   └─ 打开文件
   └─ 循环 read(fd) → write(data_fd) 发送文件内容
   └─ 关闭文件和 data_fd
   └─ session_reply("226 Transfer complete")

5. 客户端断开
   └─ read 返回 0
   └─ session_reset() 清理资源
   └─ reactor_del() 从 epoll 移除
   └─ close(ctrl_fd)
```

### 9.2 关键数据结构关系

```
Reactor (全局)
  └─ epfd (int) — epoll 实例
  └─ events (epoll_event[]) — 事件数组
  └─ sessions (FtpSession*) — 所有客户端会话

FtpSession (每个连接一个)
  ├─ ctrl_fd — 控制连接 socket
  ├─ state — ST_WAIT_USER / ST_WAIT_PASS / ST_LOGGED_IN
  ├─ username / home_dir / current_dir — 用户信息
  ├─ data_conn (DataConnection) — 数据连接状态
  │   ├─ mode — DATA_NONE / DATA_PASV / DATA_PORT
  │   ├─ fd — 数据连接 socket
  │   ├─ listen_fd — PASV 监听 socket
  │   └─ restart_pos — 断点续传偏移
  ├─ cmd_buf / cmd_len — 命令缓冲
  └─ resp_buf / resp_len — 响应缓冲
```

---

## 10. 动手练习

按难度递进，建议边改边测试：

### 初级（理解代码）

1. **改欢迎语**：修改 `server.cpp` 中发送的 220 欢迎消息
2. **限制最大目录深度**：`cmd_CWD` 中限制 `current_dir` 不超过 5 级
3. **禁用匿名用户**：修改 `cmd_USER` 拒绝名为 `anonymous` 的用户

### 中级（增加功能）

4. **增加 STAT 命令**：参考 `cmd_LIST` 实现，返回当前路径状态
5. **限速传输**：在 `cmd_RETR` 的传输循环中，每发送 1MB 就 sleep(1)
6. **上传覆盖确认**：`cmd_STOR` 时如果文件已存在，询问客户端是否覆盖

### 高级（架构改造）

7. **添加 MD5 校验**：传输完成后用 MD5 校验文件完整性
8. **线程池改造**：把数据连接的收发放到线程池中（mutex + condition variable 同步）
9. **日志系统**：用 syslog 替代 printf，支持日志级别（DEBUG/INFO/ERROR）

---

## 11. 面试知识图谱

这个项目覆盖的面试知识点：

### TCP/IP 协议

| 问题 | 答案位置 | 代码体现 |
|------|---------|---------|
| 三次握手过程 | 第 5 节 | server: accept, client: connect |
| 四次挥手过程 | server.cpp session_reset | close(ctrl_fd) |
| 粘包与拆包 | 第 4 节 | read_response 的完整行判断 |
| 网络字节序 | 第 4 节 | htons, inet_pton |
| TIME_WAIT | server.cpp | SO_REUSEADDR |

### Linux 系统编程

| 问题 | 答案位置 | 代码体现 |
|------|---------|---------|
| select/poll/epoll 区别 | 第 5 节 | reactor.cpp |
| ET vs LT 触发模式 | 第 5 节 | EPOLLET + 循环读 |
| 非阻塞 I/O | server.cpp | fcntl(O_NONBLOCK) |
| 守护进程 | server.cpp | daemon_init |
| 信号处理 | server.cpp | signal(SIGINT, ..) |
| 文件 I/O | commands.cpp | open/read/write |

### FTP 协议

| 问题 | 答案位置 | 代码体现 |
|------|---------|---------|
| 控制连接 vs 数据连接 | 第 7 节 | ctrl_fd vs data_conn.fd |
| PASV vs PORT | 第 7 节 | data_conn.cpp |
| 断点续传 | 第 6 节 | REST 命令 + lseek |
| 路径安全 | 第 6 节 | session_path_safe |

### 设计模式

| 模式 | 项目体现 |
|------|---------|
| Reactor 模式 | epoll 事件循环 + 回调分发 |
| 状态机 | SessionState: WAIT_USER → WAIT_PASS → LOGGED_IN |
| 分发表（Table-driven） | cmd_table 函数指针数组 |
| RAII（C++） | socket 资源管理 |

---

## 附录：推荐学习资源

### 书籍
- 《Unix 网络编程 卷1：套接字联网 API》— 网络编程圣经
- 《Linux 高性能服务器编程》— epoll 实战
- 《计算机网络：自顶向下》第 3 章 — TCP 协议
- RFC 959 — FTP 协议官方文档

### 在线资料
- [Beej's Guide to Network Programming](https://beej.us/guide/bgnet/) — 网络编程入门
- [Linux man pages](https://man7.org/linux/man-pages/) — epoll, socket 等
- [Wireshark FTP 分析](https://wiki.wireshark.org/FileTransferProtocol) — 抓包看协议

### 建议学习顺序

```
Week 1: TCP socket 基础 → 读 client.cpp + 写一个 echo 客户端
Week 2: epoll → 读 reactor.cpp + 写一个 echo 服务器
Week 3: FTP 协议 → 读 commands.cpp + 用 Wireshark 抓包
Week 4: 完整项目 → 手写一遍核心逻辑（server + commands + session）
```

---

*教程结束。有问题随时问！*
