/*
 * client.cpp — FTP 命令行客户端
 * ===============================
 *
 * 这个文件实现了一个"FTP 客户端"程序。
 * 它让你在终端里输入命令（比如 list、get、put），
 * 然后和 FTP 服务器通信，完成文件的上传下载。
 *
 * 学习这个文件可以帮你理解：
 *   1. TCP 网络编程：socket() → connect() → send()/recv() → close()
 *   2. FTP 协议：客户端和服务器之间怎么用文本命令通信
 *   3. PASV 被动模式：数据传输通道是怎么建立的
 *   4. 粘包处理：TCP 是流协议，怎么从连续的数据流中提取完整消息
 *
 * 编译: g++ -std=c++11 -O2 -Wall -o ftp client.cpp
 * 运行: ./ftp                     ← 启动后手动输入 open 连接
 * 或:   ./ftp 127.0.0.1 21       ← 启动后自动连接
 */

/* =================================================================
 * 头文件包含
 *
 * 每个头文件都提供了特定的功能。你可以把它们想象成"工具箱"：
 *   #include <stdio.h>    →   拿来了"输入输出"工具箱
 *   #include <string.h>   →   拿来了"字符串操作"工具箱
 *   ...以此类推
 *
 * 注意：<> 表示在系统目录中找头文件，"" 表示在当前目录找。
 * ================================================================= */

#include <stdio.h>      /* 输入输出：printf, perror, snprintf, fgets 等 */
#include <stdlib.h>     /* 标准库：exit, atoi, malloc, free 等 */
#include <string.h>     /* 字符串：strlen, strcpy, strcmp, memcpy, strchr 等 */
#include <unistd.h>     /* Unix 系统调用：read, write, close, sleep 等 */
#include <fcntl.h>      /* 文件控制：open, O_RDONLY, O_WRONLY, O_CREAT 等 */
#include <errno.h>      /* 错误码：EAGAIN, EWOULDBLOCK 等 */
#include <stdarg.h>     /* 可变参数：va_list, va_start, va_end 等 */
#include <sys/socket.h> /* socket 编程核心：socket, connect, bind, listen, accept */
#include <netinet/in.h> /* 网络地址结构体：struct sockaddr_in */
#include <arpa/inet.h>  /* IP 地址转换：inet_pton（字符串→二进制）, inet_ntoa（二进制→字符串） */
#include <netdb.h>      /* 域名解析：gethostbyname（域名→IP） */


/* =================================================================
 * 全局变量
 *
 * "全局"意味着这些变量在整个文件的任何函数里都能访问。
 * 它们记录了客户端程序的"状态"——就像你玩游戏时屏幕上的
 * 血量条、位置坐标一样，这些变量时刻记录程序当前的状态。
 *
 * static 关键字：表示这些变量只在当前 .cpp 文件中可见，
 * 其他文件访问不到。这是 C 语言的"封装"手段。
 * ================================================================= */

/* --- 控制连接 ---
 * FTP 使用"双连接"架构：
 *   - 控制连接（ctrl_fd）：传命令和响应，像"对讲机"
 *   - 数据连接（data_fd）：传文件内容，像"快递通道"
 *
 * fd = file descriptor（文件描述符）
 * 在 Linux 中，"一切皆文件"——网络连接也被当作文件来操作。
 * fd 就是一个整数编号（比如 3、4、5），代表了打开的文件或连接。
 * 你通过这个编号来读写数据：read(fd, buf, size) 读，write(fd, buf, size) 写。
 *
 * 初始值 -1："未连接"或"已关闭"的标志。因为真正的 fd 从 3 开始，
 * 0=标准输入, 1=标准输出, 2=标准错误输出。
 */
static int ctrl_fd = -1;        /* 控制连接的 fd — 用来收发 FTP 命令和响应 */
static int data_fd = -1;        /* 数据连接的 fd — 用来收发文件数据 */
static int data_listen_fd = -1; /* PASV 模式中用到的监听 fd（当前客户端没用） */

static int logged_in = 0;       /* 是否已登录：0=未登录, 1=已登录 */

/* 服务器地址信息（默认值） */
static char server_ip[64] = "127.0.0.1";  /* 服务器 IP 地址 */
static int server_port = 21;              /* 服务器端口（FTP 默认是 21） */

/* --- 接收缓冲区 ---
 * 从服务器收到的数据先存到这里，再从中提取出完整的响应。
 * 为什么需要缓冲区？因为 TCP 是"流"协议——数据像水管里的水一样
 * 连续流动，没有明确的分隔标记。一次 read() 可能只读到半条消息，
 * 所以需要缓冲区来"攒"数据，等攒够一条完整消息再处理。
 *
 * 65536 字节 ≈ 64KB。这个大小足够容纳任何 FTP 响应。
 */
static char recv_buf[65536];    /* 环形缓冲区：存储从服务器收到的原始数据 */
static int recv_len = 0;        /* 缓冲区中有效数据的长度 */

/* --- 自动 PASV 模式 ---
 * 设置为 1 表示：当需要传输数据时，自动执行 PASV 命令建立数据连接。
 * 用户不需要手动输入 pasv，直接用 list / get / put 即可。
 */
static int auto_pasv = 1;


/* =================================================================
 * 工具函数 ① send_cmd() — 发送命令到服务器
 *
 * 作用：按照 FTP 协议的格式，向服务器发送一条命令。
 *
 * 参数里的 "..." 是 C 语言的"可变参数"语法，跟 printf 一样。
 * 你可以这样调用：
 *   send_cmd("USER %s\r\n", "test");     → 发送 "USER test\r\n"
 *   send_cmd("PWD\r\n");                  → 发送 "PWD\r\n"
 *   send_cmd("RETR %s\r\n", filename);    → 发送 "RETR readme.txt\r\n"
 *
 * 为什么要加 \r\n？
 * FTP 协议规定：每条命令必须以回车（\r）和换行（\n）结尾。
 * 就像写信要有"此致敬礼"一样，这是协议的"格式要求"。
 * 服务器收到 \r\n 就知道一条命令结束了，开始解析执行。
 * ================================================================= */

static void send_cmd(const char *fmt, ...) {
    /* 第一步：组装命令字符串 */

    char buf[4096];    /* 临时缓冲区，存放格式化后的完整命令 */

    va_list ap;        /* va_list：可变参数列表的"指针" */
    va_start(ap, fmt); /* 让 ap 指向第一个可变参数 */
    /*
     * vsnprintf 和 snprintf 一样，都是"格式化字符串到缓冲区"。
     * 区别是 vsnprintf 从 va_list 中取参数，而不是直接写参数。
     * 这里因为参数是"..."可变参数，所以用 vsnprintf。
     *
     * 例如：send_cmd("USER %s\r\n", "test")
     *   fmt = "USER %s\r\n"
     *   ap 指向 "test"
     *   vsnprintf 把 "test" 代入 %s → buf = "USER test\r\n"
     */
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);        /* 清理 va_list */

    /* 第二步：发送到服务器 */

    int len = strlen(buf);        /* 计算要发送的字节数 */
    /*
     * write() 是 Linux 的系统调用，用来向一个文件描述符写入数据。
     * 这里 ctrl_fd 代表和服务器的网络连接（它就是一个 fd），
     * 所以 write(ctrl_fd, buf, len) 就是把数据"写"到网络中，
     * 数据会通过网络传输到服务器。
     *
     * write() 返回值是实际写入的字节数，如果失败返回 -1。
     * 这里用 (void)ret 把返回值"强制忽略"，因为它不影响后续逻辑。
     */
    ssize_t ret = write(ctrl_fd, buf, len);
    (void)ret;                     /* 显式忽略返回值，避免编译器警告 */

    /* 第三步：在控制台打印发送的内容（方便调试和观察） */

    printf(">>> %s", buf);         /* 打印 >>> 前缀，表示"客户端发出去的内容" */
}


/* =================================================================
 * 工具函数 ② read_response() — 读取服务器响应
 *
 * 作用：从服务器读取一条完整的响应消息。
 *
 * 返回值：服务器返回的"状态码"，比如 220（成功）、331（需要密码）等。
 *
 * 为什么这个函数这么复杂？
 * 因为 TCP 是"流"协议！当你调用 read() 时，可能发生三种情况：
 *   1. 只读到半条消息（比如只读了 "220 Wel"）
 *   2. 刚好读到一条完整消息
 *   3. 一次读到了多条消息（例如两条连续响应粘在一起）
 *
 * 这叫做"粘包"问题。解决办法就是循环读，攒够完整消息再返回。
 *
 * FTP 协议规定响应的格式是：
 *   单行：三位数字码 + 空格 + 消息 + \r\n
 *   多行（多个响应行，以 ---- 分隔最后一行）：
 *     三位数字码 + 短横线 + 消息 + \r\n
 *     ...
 *     三位数字码 + 空格 + 消息 + \r\n
 *
 * 所以判断"一条完整响应"的标准就是：
 *   最后一行以 "三位数字码 + 空格" 开头。
 * 我们找到最后一行，检查它的第 4 个字符是不是空格。
 * ================================================================= */

static int read_response() {
    char buf[4096];     /* 临时缓冲区，每次 read 读到的数据放这里 */
    ssize_t n;          /* 本次 read 实际读到的字节数 */

    /*
     * recv_len = 0：清空全局缓冲区的"长度计数器"。
     * 注意，我们只是把计数器清零，没有真的擦除缓冲区里的数据。
     * 因为后续会覆盖写入，所以没关系。
     */
    recv_len = 0;

    /* 循环读取，直到收到一条完整响应 */
    while (1) {
        /*
         * read()：从文件描述符读取数据。
         *   ctrl_fd：从控制连接读取
         *   buf：数据存到哪里
         *   sizeof(buf) - 1：最多读多少字节（留 1 个字节放 \0）
         *
         * 返回值 n：
         *   > 0：成功读到了 n 个字节
         *   = 0：对端关闭了连接（优雅关闭）
         *   < 0：出错（比如连接被重置）
         */
        n = read(ctrl_fd, buf, sizeof(buf) - 1);

        if (n <= 0) {
            /* 连接关闭或出错 */
            printf("[!] Connection closed by server\n");
            return -1;
        }

        /*
         * buf[n] = '\0'：给读到的数据末尾加字符串结束符。
         * 这样我们可以把 buf 当作字符串来使用（比如 printf("%s", buf)）。
         * 注意：sizeof(buf) - 1 确保 n 最大为 4095，留出的 1 个位置就是放 \0 的。
         */
        buf[n] = '\0';

        /* --- 把本次读到的数据追加到全局缓冲区 recv_buf --- */

        /*
         * 检查剩余空间是否够用。recv_len 是已存数据长度，
         * recv_len + n 就是存完之后的总长度。
         * 必须小于 recv_buf 的总大小 - 1（留 1 个字节放 \0）。
         */
        if (recv_len + n < (int)sizeof(recv_buf) - 1) {
            /*
             * memcpy：内存拷贝
             *   recv_buf + recv_len：目标地址（从已有数据的末尾开始写）
             *   buf：源地址（刚读到的数据）
             *   n：复制多少个字节
             */
            memcpy(recv_buf + recv_len, buf, n);
            recv_len += n;               /* 更新已存数据长度 */
            recv_buf[recv_len] = '\0';   /* 尾部加 \0 确保字符串安全 */
        }

        /* 在控制台打印服务器返回的内容 */
        printf("%s", buf);

        /* --- 判断是否收到了完整响应 --- */

        /*
         * 只有当本次 read 至少读到 4 个字节时，才有可能是一条完整响应。
         * 因为最短的 FTP 响应是 "NNN\r\n"（如 "200\r\n"），至少 5 个字节。
         * 这里 n >= 4 是一个快速过滤条件。
         */
        if (n >= 4) {
            /*
             * 找到 recv_buf 中"最后一行"的起始位置。
             *
             * 方法：从靠近末尾的位置往前搜索 \n 字符。
             * recv_len - 5 是倒数第 5 个字符的位置。
             * 为什么从 -5 开始？因为一条完整响应至少 5 个字节（如 "220\r\n"），
             * 所以我们只需要检查最后几个字符就够了。
             *
             * last_line 初始指向 recv_buf 开头（第一行），
             * 每找到一个 \n，就把 last_line 移到该行的下一个字符。
             * 这样循环结束后，last_line 就指向最后一行了。
             */
            char *last_line = recv_buf;  /* 默认从缓冲区开头找 */
            for (int i = recv_len - 5; i >= 0; i--) {
                if (recv_buf[i] == '\n') {
                    /*
                     * 发现了一个换行符！这意味着一行结束了。
                     * recv_buf + i + 1 就是下一行（即最后一行）的开头。
                     * 然后 break 跳出循环。
                     */
                    last_line = recv_buf + i + 1;
                    break;
                }
            }

            /*
             * 检查最后一行是否以 "NNN " 开头。
             * llen = last_line 的长度。如果 llen >= 4 且 last_line[3] == ' '，
             * 说明这行是 "三位数字码 + 空格 + 消息" 的格式，
             * 即——这是一条完整的 FTP 响应！
             *
             * 为什么是 last_line[3]？因为：
             *   last_line[0] = 百位数字（如 '2'）
             *   last_line[1] = 十位数字（如 '2'）
             *   last_line[2] = 个位数字（如 '0'）
             *   last_line[3] = ' ' 或 '-'
             *   空格表示最后一行（响应结束），短横线表示还有后续行
             */
            int llen = strlen(last_line);
            if (llen >= 4 && last_line[3] == ' ') {
                /*
                 * atoi：把字符串转成整数（ASCII to Integer）
                 * "220 OK\r\n" → 220
                 * 返回状态码，调用者根据这个码判断是成功还是失败。
                 */
                return atoi(last_line);
            }
        }

        /*
         * 如果执行到这里，说明还没有收到完整响应。
         * 继续循环，用下一次 read() 读取更多数据。
         */
    }
}


/* =================================================================
 * 工具函数 ③ connect_data_pasv() — 建立 PASV 数据连接
 *
 * 作用：解析服务器返回的 PASV 响应，然后主动连接服务器的数据端口。
 *
 * 背景知识：FTP 的 PASV（被动）模式
 *   1. 客户端发送 PASV 命令
 *   2. 服务器会开启一个临时端口（如 30001），并返回 IP 和端口号
 *   3. 客户端用这个 IP:端口 建立新的 TCP 连接（这就是"数据连接"）
 *   4. 文件传输都通过这个数据连接进行
 *
 * 服务器返回的格式类似：
 *   227 Entering Passive Mode (127,0,0,1,117,48)
 *   括号里 6 个数字的含义：
 *     127,0,0,1  = IP 地址（四个数字分别代表 IP 的四段）
 *     117,48     = 端口号（高8位=117, 低8位=48, 所以端口号 = 117*256 + 48 = 30000）
 * ================================================================= */

static int connect_data_pasv(const char *pasv_resp) {
    /*
     * 用来存储解析结果的 6 个"无符号整数"
     * h1-h4: IP 地址的四段
     * p1-p2: 端口号的高8位和低8位
     */
    unsigned int h1, h2, h3, h4, p1, p2;
    int data_port;     /* 计算出的完整端口号 */
    char addr[64];     /* IP 地址字符串（如 "127.0.0.1"） */

    /* --- 第一步：从响应字符串中解析 IP 和端口 --- */

    /*
     * strchr：在字符串中搜索指定字符，返回该字符第一次出现的位置指针。
     * 这里找 '(' 字符，定位到 PASV 信息的起始位置。
     * 比如："227 Entering Passive Mode (127,0,0,1,117,48)"
     *                                       ↑ p 指向这里
     */
    const char *p = strchr(pasv_resp, '(');
    if (!p) return -1;      /* 没找到 '('，说明响应格式不对，返回失败 */
    p++;                     /* 跳过 '('，p 现在指向 '1'（127 的第一个数字） */

    /*
     * sscanf："字符串扫描格式化"——从字符串中按指定格式提取数据。
     * "%u,%u,%u,%u,%u,%u" 表示"无符号整数,逗号,无符号整数,..."
     * 从 p 指向的 "(127,0,0,1,117,48)" 中提取 6 个数字。
     * 返回值是成功匹配的项目数，必须等于 6 才说明解析成功。
     */
    if (sscanf(p, "%u,%u,%u,%u,%u,%u", &h1, &h2, &h3, &h4, &p1, &p2) != 6)
        return -1;

    /* 组装 IP 地址字符串：把 4 个数字用点连起来 */
    snprintf(addr, sizeof(addr), "%u.%u.%u.%u", h1, h2, h3, h4);

    /* 计算端口号：p1 是高 8 位，p2 是低 8 位，左移 8 位再相加 */
    data_port = (p1 << 8) | p2;   /* 相当于 p1 * 256 + p2 */

    /* 在控制台打印解析结果（方便调试） */
    printf("[data] PASV address: %s:%d\n", addr, data_port);

    /* --- 第二步：创建并连接数据 socket --- */

    /*
     * socket()：创建一个网络端点（socket）。
     * 参数：
     *   AF_INET     = IPv4 地址族（还有 AF_INET6 代表 IPv6）
     *   SOCK_STREAM = 流式套接字（即 TCP，面向连接的可靠传输）
     *   0           = 自动选择协议（TCP）
     *
     * 返回一个文件描述符 fd，后续通过这个 fd 来收发数据。
     * 和普通文件的差别：这个 fd 对应的是网络连接，不是磁盘文件。
     */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("[data] socket");   /* perror 自动打印"错误描述" */
        return -1;
    }

    /*
     * struct sockaddr_in：IPv4 地址结构体。
     * 用来存储"要连接的目标地址"（IP + 端口）。
     *
     * 在 TCP 编程中，你需要：
     *   1. 创建一个 sockaddr_in 结构体
     *   2. 填上目标 IP 和端口
     *   3. 传给 connect() 函数
     * 就像你寄快递需要填写"收件人地址"一样。
     */
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));        /* 清零：把结构体所有字节设成 0 */
    sa.sin_family = AF_INET;            /* 地址族：IPv4 */

    /*
     * htons()："Host TO Network Short"
     * 把"主机字节序"的端口号转成"网络字节序"（大端）。
     *
     * 为什么需要字节序转换？
     * 不同的 CPU 存储多字节数据的方式不同：
     *   - 小端（x86）：低地址存低位字节（如 0x1234 存为 34 12）
     *   - 大端（网络）：低地址存高位字节（如 0x1234 存为 12 34）
     * 网络协议规定所有多字节字段都是"大端"的。
     * 如果你的 CPU 是小端，就需要用 htons() 转换。
     *
     * 同理：htonl() 处理 32 位整数，ntohs()/ntohl() 反过来（网络→主机）。
     */
    sa.sin_port = htons(data_port);     /* 设置端口（网络字节序） */

    /*
     * inet_pton()："Presentation TO Network"
     * 把 IP 地址的"字符串形式"转成"二进制形式"。
     * "127.0.0.1" → 二进制 0x7F000001
     *
     * 第一个参数是地址族（AF_INET 表示 IPv4），
     * 第二个是输入字符串，
     * 第三个是输出缓冲区的地址。
     */
    inet_pton(AF_INET, addr, &sa.sin_addr);

    /*
     * connect()：连接到服务器。
     * 这是 TCP 客户端最关键的一步——主动向服务器发起连接请求。
     * 对应 TCP 三次握手的开始。
     *
     * 参数：
     *   fd：我们刚创建的 socket
     *   (struct sockaddr*)&sa：目标地址（强制转换类型）
     *   sizeof(sa)：地址结构体的大小
     *
     * 注意类型转换：(struct sockaddr*)&sa
     * 因为 connect() 设计时（1970 年代）C 语言还没有"多态"概念，
     * 所以设计者用这种方式来支持多种地址族（IPv4、IPv6 等）。
     * sockaddr_in 是 IPv4 的地址结构，sockaddr_in6 是 IPv6 的，
     * 它们都通过强制转换为 sockaddr* 来传给 connect()。
     * 这在 C 语言中很常见，叫做"类型双关"。
     */
    if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        perror("[data] connect");
        close(fd);          /* 连接失败，关闭 socket，释放资源 */
        return -1;
    }

    /* 连接成功！保存 data_fd，后续通过它来收发文件数据 */
    data_fd = fd;
    printf("[data] PASV connected to %s:%d\n", addr, data_port);
    return 0;               /* 返回 0 表示成功 */
}


/* =================================================================
 * 工具函数 ④ close_data() — 关闭数据连接
 *
 * 每次文件传输完成后，都需要关闭数据连接。
 * 因为 FTP 协议中，数据连接是"一次性的"——每次传输都用新的连接。
 * 关闭后重置 fd 为 -1，以便下一次传输时重新建立连接。
 * ================================================================= */

static void close_data() {
    if (data_fd >= 0) {          /* 如果数据连接还开着 */
        close(data_fd);          /* 关闭 socket（就像挂断电话） */
        data_fd = -1;            /* 标志为"已关闭" */
    }
    if (data_listen_fd >= 0) {   /* 如果有监听 fd 也关闭 */
        close(data_listen_fd);
        data_listen_fd = -1;
    }
}


/* =================================================================
 * 自动 PASV 功能
 *
 * 传统上，FTP 客户端需要手动执行两步操作：
 *   1. pasv    → 建立数据连接
 *   2. list    → 发送 LIST 命令
 *
 * 现在自动执行了：用户输入 list，系统自动先做 pasv，再做 list。
 * 用户完全感觉不到 pasv 的存在，体验更流畅。
 * ================================================================= */

/*
 * ensure_pasv() — 确保数据连接已建立
 *
 * 如果 data_fd 还没有连接（即 -1），就自动发送 PASV 命令并连接。
 * 如果已经连接了，就直接返回 0（不需要重复连接）。
 *
 * 返回值：0 表示数据连接已就绪，-1 表示失败。
 */
static int ensure_pasv() {
    /* 如果 data_fd >= 0，说明已经有数据连接了，不需要重复建立 */
    if (data_fd >= 0) return 0;

    /* 如果 auto_pasv == 0，说明用户关闭了自动 PASV */
    if (!auto_pasv) return -1;

    /* 自动发送 PASV 命令 */
    printf("[auto] Sending PASV...\n");
    send_cmd("PASV\r\n");               /* 发送 PASV 命令 */
    int code = read_response();          /* 读取服务器的响应 */

    if (code == 227) {
        /*
         * 227 是 PASV 命令的成功响应码。
         * 响应内容如："227 Entering Passive Mode (127,0,0,1,117,48)"
         * recv_buf 中保存了完整的响应文本，传给 connect_data_pasv() 解析。
         */
        return connect_data_pasv(recv_buf);
    }

    /* 如果服务器没有返回 227，说明 PASV 失败了 */
    printf("[auto] PASV failed (code=%d)\n", code);
    return -1;
}


/* =================================================================
 * 文件传输函数 ① cmd_retr() — 下载文件（RETR 命令）
 *
 * RETR = RETRIEVE（获取），对应客户端的 get 命令。
 * 作用：从服务器下载一个文件到本地。
 *
 * 流程：
 *   1. 确保数据连接已建立（自动 PASV）
 *   2. 发送 "RETR 文件名\r\n" 给服务器
 *   3. 从数据连接读取文件数据，写入本地磁盘文件
 *   4. 关闭数据连接
 * ================================================================= */

static void cmd_retr(const char *arg) {
    /*
     * arg 是用户输入的文件名参数。
     * if (!arg || !*arg) 检查是否为空：
     *   !arg  → arg 是 NULL 指针（没传参）
     *   !*arg → arg 指向空字符串（传了空串）
     * 这种写法是 C 语言的惯用技巧，初学者可能觉得绕，
     * 其实就是在判断"有没有提供文件名"。
     */
    if (!arg || !*arg) {
        printf("Usage: get <filename>\n");
        return;
    }

    /* 确保数据连接已建立（自动 PASV） */
    if (ensure_pasv() < 0) {
        printf("[!] No data connection. Use 'pasv' manually or check server.\n");
        return;
    }

    /* --- 提取纯文件名（去掉路径前缀） --- */

    char filename[256];
    /*
     * strrchr：从字符串末尾向前搜索指定字符。
     * 比如 arg = "/home/test/readme.txt"，搜索 '/'，
     * 找到最后一个 '/' 的位置，slash + 1 就是 "readme.txt"。
     *
     * 为什么需要这个操作？
     * 如果用户输入 "get ../abc/readme.txt"，服务器按这个路径找文件。
     * 但下载到本地时，我们只想要 "readme.txt" 作为文件名。
     */
    const char *slash = strrchr(arg, '/');
    if (slash)
        snprintf(filename, sizeof(filename), "%s", slash + 1);  /* 取 '/' 后面的部分 */
    else
        snprintf(filename, sizeof(filename), "%s", arg);        /* 没有 '/'，直接用原名字 */

    /* --- 发送 RETR 命令，告诉服务器我们要下载文件 --- */

    send_cmd("RETR %s\r\n", arg);   /* 例如：RETR readme.txt\r\n */
    int code = read_response();     /* 读取响应码 */

    /*
     * 服务器可能返回：
     *   150 = 数据连接已打开，准备传输（最常见）
     *   125 = 数据连接已打开，马上开始传输
     *   其他 = 失败（比如文件不存在、没有权限等）
     */
    if (code != 150 && code != 125) {
        printf("[!] Download rejected\n");
        close_data();
        return;
    }

    /* --- 在本地创建文件，准备写入 --- */

    /*
     * open()：打开或创建一个文件。
     * 参数：
     *   filename         = 文件名
     *   O_WRONLY         = 只写模式
     *   O_CREAT          = 如果文件不存在就创建
     *   O_TRUNC          = 如果文件已存在就清空（覆盖写）
     *   0666             = 文件权限：所有者/组/其他都可读写
     *
     * 返回文件描述符 fd，后面的 write() 通过 fd 来写数据。
     */
    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        perror("[retr] open");    /* 比如磁盘满了、权限不够 */
        close_data();
        return;
    }

    /* --- 从数据连接读取数据，写入本地文件 --- */

    char buf[8192];     /* 8KB 缓冲区：从网络读到这块内存，再写到磁盘 */
    ssize_t n;          /* 记录每次读到的字节数 */
    off_t total = 0;    /* 累加总字节数 */

    /*
     * 循环读取数据：从数据连接（data_fd）读取，写入本地文件（fd）。
     * 当 read() 返回 0 时，表示服务器数据发送完毕，关闭了数据连接。
     * 这就是为什么 FTP 协议每次传输都要新建数据连接——靠连接关闭
     * 来表示"数据发完了"，而不是像控制连接那样约定 \r\n 结束。
     */
    while ((n = read(data_fd, buf, sizeof(buf))) > 0) {
        /*
         * write()：把 buf 中的数据写入文件 fd。
         * 简单场景下（如本地磁盘），write 一次就能写完所有数据。
         * 所以这里没有检查部分写入的情况（和 cmd_stor 不同）。
         */
        ssize_t w = write(fd, buf, n);
        (void)w;
        total += n;     /* 累加下载的总字节数 */
    }

    /* --- 清理 --- */

    close(fd);               /* 关闭本地文件 */
    close_data();            /* 关闭数据连接 */
    printf("[retr] Downloaded %lld bytes to %s\n", (long long)total, filename);

    /*
     * 读取服务器的最终响应：226 Transfer complete。
     * 虽然我们已经下载完了，但协议层面还需要确认服务器知道传输完成。
     * 不读这个响应服务器可能还在等我们确认。
     */
    read_response();
}


/* =================================================================
 * 文件传输函数 ② cmd_stor() — 上传文件（STOR 命令）
 *
 * STOR = STORE（存储），对应客户端的 put 命令。
 * 作用：把本地文件上传到服务器。
 *
 * 和 RETR 的区别：
 *   RETR：服务器 → 客户端（下载，数据从服务器流向客户端）
 *   STOR：客户端 → 服务器（上传，数据从客户端流向服务器）
 *
 * 在代码上的区别：
 *   RETR：先发 RETR 命令，再读 data_fd 写入本地文件
 *   STOR：先发 STOR 命令，再读本地文件写 data_fd（方向相反）
 * ================================================================= */

static void cmd_stor(const char *arg) {
    if (!arg || !*arg) {
        printf("Usage: put <local_file>\n");
        return;
    }

    /* 确保数据连接已建立 */
    if (ensure_pasv() < 0) {
        printf("[!] No data connection. Use 'pasv' manually or check server.\n");
        return;
    }

    /* --- 打开本地文件，准备读取 --- */

    /*
     * open(arg, O_RDONLY)：以"只读"模式打开本地文件。
     * arg 是用户输入的文件路径（如 "test.txt" 或 "/home/user/test.txt"）。
     */
    int fd = open(arg, O_RDONLY);
    if (fd < 0) {
        perror("[stor] open");   /* 文件不存在或无法读取 */
        return;
    }

    /* --- 提取纯文件名（同 RETR） --- */

    const char *slash = strrchr(arg, '/');
    const char *filename = slash ? slash + 1 : arg;

    /* --- 发送 STOR 命令，告诉服务器我们要上传文件 --- */

    send_cmd("STOR %s\r\n", filename);  /* 例如：STOR test.txt\r\n */
    int code = read_response();

    if (code != 150 && code != 125) {
        printf("[!] Upload rejected\n");
        close(fd);
        close_data();
        return;
    }

    /* --- 从本地文件读取数据，写入数据连接（发送到服务器） --- */

    char buf[8192];     /* 8KB 缓冲区 */
    ssize_t n;          /* 每次从文件读取的字节数 */
    off_t total = 0;    /* 上传总字节数 */

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        ssize_t written = 0;  /* 当前批次已写入的字节数 */

        /*
         * 这里使用"内层循环"处理部分写入的情况。
         *
         * 为什么需要内层循环？
         * write(data_fd, ...) 是写向网络的。网络可能繁忙，导致一次
         * write() 只写了一半数据。比如 n=4096 字节，write() 只写了
         * 2000 字节就返回了（返回 2000），剩下的 2096 字节需要重试。
         *
         * 内层循环的逻辑：
         *   while (written < n)   ← 还没写完本次的所有数据
         *     write(buf + written) ← 从上次写到的位置继续写
         *
         * 这和 RETR 不同。RETR 的 write() 是写本地磁盘，磁盘通常
         * 不会有"部分写入"的问题（除非磁盘满了），所以 RETR 不需要
         * 内层循环，直接一次 write() 即可。
         *
         * 这就体现了网络编程中的一个重要概念：
         * "本地 I/O"和"网络 I/O"行为不同。网络 write 可能因为
         * 对方接收缓冲区满、网络拥堵等原因只写部分数据。
         */
        while (written < n) {
            /*
             * buf + written：从本次数据中尚未写入的部分开始
             * n - written：还剩多少字节没写
             */
            ssize_t w = write(data_fd, buf + written, n - written);
            if (w < 0) {
                perror("[stor] write");      /* 网络错误 */
                goto stor_done;              /* 跳到清理代码 */
            }
            written += w;     /* 累加已写入的字节数 */
            total += w;       /* 累加上传总字节数 */
        }
    }

stor_done:
    close(fd);               /* 关闭本地文件 */
    close_data();            /* 关闭数据连接 */
    printf("[stor] Uploaded %lld bytes\n", (long long)total);

    /* 读取服务器的最终确认响应 */
    read_response();
}


/* =================================================================
 * 命令处理函数 — process_line()
 *
 * 这个函数是整个客户端的"大脑"。
 * 它读取用户输入的一行命令，解析命令名和参数，
 * 然后决定是"本地处理"还是"发送给服务器"。
 *
 * 两种处理方式：
 *   1. 本地处理（内部命令）：open, quit, pasv, get, put, list, help
 *      这些命令直接在客户端执行，不需要发送给服务器。
 *      （或者说，它们包含多个步骤，不是简单的一条 FTP 命令）
 *   2. 发送给服务器：user, pass, pwd, cwd, mkd, rmd 等
 *      直接把命令字符串发到控制连接，等服务器处理。
 * ================================================================= */

static void process_line(const char *line) {
    char cmd[64];       /* 命令名（如 "list", "get", "user"） */
    char arg[1024];     /* 命令参数（如文件名、目录名） */
    int i;

    /* --- 跳过开头的空白字符（空格、制表符等） --- */

    /*
     * while (*line == ' ') line++;
     * 这行代码的意思是：只要 line 指向的字符是空格，就把指针往后移。
     * 这样用户输入 "  list" 和 "list" 效果一样。
     * 指针 line 本身是函数的参数（是复制的），修改它不影响外面。
     */
    while (*line == ' ') line++;
    if (!*line) return;  /* 如果整行都是空格，直接返回 */

    /* --- 分割命令名和参数 --- */

    /*
     * strchr：在字符串中找空格。
     * 第一个空格把命令和参数分开，如 "get readme.txt"：
     *   line:  "get readme.txt"
     *   space:     ↑ 指向这个空格
     *   cmd:   "get"
     *   arg:   "readme.txt"
     */
    const char *space = strchr(line, ' ');
    if (space) {
        /* 有参数的情况 */
        size_t clen = space - line;    /* 命令名的长度 */
        if (clen >= sizeof(cmd)) clen = sizeof(cmd) - 1;  /* 防止溢出 */
        memcpy(cmd, line, clen);       /* 复制命令名 */
        cmd[clen] = '\0';              /* 加字符串结束符 */

        /*
         * space + 1 跳过了空格，指向参数部分。
         * 但参数前面可能还有空格（用户输入 "get  file" 多打了空格），
         * 下面的 while 循环会跳过这些空格。
         */
        snprintf(arg, sizeof(arg), "%s", space + 1);
    } else {
        /* 没有参数，整行都是命令名 */
        snprintf(cmd, sizeof(cmd), "%s", line);
        arg[0] = '\0';    /* 参数设为空字符串 */
    }

    /* --- 把命令名转为小写（方便用户输入） --- */

    /*
     * 把 'A'~'Z' 的字符统一加上 32，转成 'a'~'z'。
     * 因为 ASCII 编码中，大写字母和小写字母相差 32，
     * 比如 'A'=65, 'a'=97（97-65=32）。
     *
     * 这样用户输入 LIST、list、List 效果都一样。
     */
    for (i = 0; cmd[i]; i++)
        if (cmd[i] >= 'A' && cmd[i] <= 'Z')
            cmd[i] += 32;

    /* ============================================================
     * 本地处理的命令
     * ============================================================ */

    /* --- open：连接到服务器 --- */

    if (strcmp(cmd, "open") == 0) {
        char host[256];
        int port = 21;                 /* 默认 FTP 端口 */
        if (sscanf(arg, "%s %d", host, &port) < 1) {
            printf("Usage: open <host> [port]\n");
            return;
        }

        /*
         * gethostbyname：通过主机名获取 IP 地址。
         * 输入可以是域名（如 "ftp.example.com"）或 IP 字符串。
         * 返回一个 hostent 结构体，里面包含了该主机的所有 IP 地址。
         */
        struct hostent *he = gethostbyname(host);
        if (!he) {
            printf("Unknown host: %s\n", host);
            return;
        }

        /*
         * inet_ntoa："Network TO ASCII"
         * 把二进制 IP 地址转成字符串形式。
         * h_addr 是第一个 IP 地址的二进制数据。
         */
        strncpy(server_ip, inet_ntoa(*(struct in_addr*)he->h_addr),
                sizeof(server_ip) - 1);
        server_port = port;

        /* 如果之前有旧连接，先关闭 */
        if (ctrl_fd >= 0) close(ctrl_fd);

        /* 创建控制连接的 socket */
        ctrl_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (ctrl_fd < 0) {
            perror("[open] socket");
            return;
        }

        /* 准备服务器地址 */
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(server_port);
        inet_pton(AF_INET, server_ip, &sa.sin_addr);

        /* 连接到服务器 */
        if (connect(ctrl_fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
            perror("[open] connect");
            close(ctrl_fd);
            ctrl_fd = -1;
            return;
        }

        logged_in = 0;
        printf("Connected to %s:%d\n", server_ip, server_port);
        read_response();    /* 读取欢迎消息（220 Welcome） */
        return;
    }

    /* --- quit/exit：退出程序 --- */

    if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
        if (ctrl_fd >= 0) {
            send_cmd("QUIT\r\n");         /* 礼貌地告诉服务器我们要断开 */
            read_response();              /* 等待服务器说再见 */
        }
        exit(0);                          /* 结束进程 */
    }

    /* --- pasv：手动进入被动模式 --- */

    if (strcmp(cmd, "passive") == 0 || strcmp(cmd, "pasv") == 0) {
        send_cmd("PASV\r\n");             /* 发送 PASV 命令 */
        int code = read_response();       /* 读取 227 响应 */
        if (code == 227) {
            connect_data_pasv(recv_buf);  /* 解析并连接数据端口 */
        }
        return;
    }

    /* --- get：下载文件 --- */

    if (strcmp(cmd, "get") == 0) {
        cmd_retr(arg);    /* 调用下载函数 */
        return;
    }

    /* --- put：上传文件 --- */

    if (strcmp(cmd, "put") == 0) {
        cmd_stor(arg);    /* 调用上传函数 */
        return;
    }

    /* --- help/?：显示帮助信息 --- */

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        printf(
            "Commands:\n"
            "  open <host> [port]  Connect to FTP server\n"
            "  user <name>         Send username\n"
            "  pass <pass>         Send password\n"
            "  pasv                Enter passive mode (auto by default)\n"
            "  list [path]         List directory\n"
            "  get <file>          Download file (PASV auto)\n"
            "  put <file>          Upload file (PASV auto)\n"
            "  pwd                 Print working directory\n"
            "  cwd <dir>           Change directory\n"
            "  mkd <dir>           Create directory\n"
            "  rmd <dir>           Remove directory\n"
            "  dele <file>         Delete file\n"
            "  syst                System type\n"
            "  size <file>         File size\n"
            "  type <A|I>          Transfer mode\n"
            "  quit                Exit\n"
        );
        return;
    }

    /* --- list：列出目录内容 --- */

    /*
     * list 需要数据连接。先确保数据连接已建立（自动 PASV），
     * 然后发送 LIST 命令，从数据连接读取目录数据并打印。
     */
    if (strcmp(cmd, "list") == 0) {
        if (ensure_pasv() < 0) {
            printf("[!] No data connection. Use 'pasv' manually or check server.\n");
            return;
        }

        /* 如果用户指定了路径，发 "LIST 路径\r\n"，否则发 "LIST\r\n" */
        if (arg[0]) send_cmd("LIST %s\r\n", arg);
        else send_cmd("LIST\r\n");

        int code = read_response();  /* 期望 150 或 125 */

        if (code == 150 || code == 125) {
            /*
             * 从数据连接读取服务器发来的目录列表。
             * 数据是文本格式（类似 ls -l 的输出），直接打印到控制台。
             */
            char dbuf[4096];
            ssize_t n;
            while ((n = read(data_fd, dbuf, sizeof(dbuf) - 1)) > 0) {
                dbuf[n] = '\0';
                printf("%s", dbuf);   /* 逐块打印目录列表 */
            }
            close_data();             /* 读取完毕，关闭数据连接 */
        }
        read_response();  /* 读取 226 Directory send OK */
        return;
    }

    /* ============================================================
     * 发送到服务器的命令
     *
     * 以下这些命令不需要客户端做额外处理，直接把命令字符串
     * 原样发送给服务器，然后读取响应即可。
     * 服务器会解析并执行这些命令。
     * ============================================================ */

    if (strcmp(cmd, "user") == 0) {
        send_cmd("USER %s\r\n", arg);   /* 例如：USER test\r\n */
    } else if (strcmp(cmd, "pass") == 0) {
        send_cmd("PASS %s\r\n", arg);   /* 例如：PASS 123456\r\n */
    } else if (strcmp(cmd, "pwd") == 0) {
        send_cmd("PWD\r\n");            /* 服务器返回当前路径 */
    } else if (strcmp(cmd, "cwd") == 0 || strcmp(cmd, "cd") == 0) {
        send_cmd("CWD %s\r\n", arg);    /* 切换工作目录 */
    } else if (strcmp(cmd, "mkd") == 0) {
        send_cmd("MKD %s\r\n", arg);    /* 创建目录 */
    } else if (strcmp(cmd, "rmd") == 0) {
        send_cmd("RMD %s\r\n", arg);    /* 删除目录 */
    } else if (strcmp(cmd, "dele") == 0) {
        send_cmd("DELE %s\r\n", arg);   /* 删除文件 */
    } else if (strcmp(cmd, "syst") == 0) {
        send_cmd("SYST\r\n");           /* 查询系统类型（如 UNIX） */
    } else if (strcmp(cmd, "size") == 0) {
        send_cmd("SIZE %s\r\n", arg);   /* 查询文件大小 */
    } else if (strcmp(cmd, "type") == 0) {
        send_cmd("TYPE %s\r\n", arg);   /* 设置传输模式 A=ASCII, I=Binary */
    } else if (strcmp(cmd, "noop") == 0) {
        send_cmd("NOOP\r\n");           /* 心跳检测（No Operation） */
    } else if (strcmp(cmd, "feat") == 0) {
        send_cmd("FEAT\r\n");           /* 查询服务器支持哪些特性 */
    } else if (strcmp(cmd, "rnfr") == 0) {
        send_cmd("RNFR %s\r\n", arg);   /* 重命名：指定原文件名 */
    } else if (strcmp(cmd, "rnto") == 0) {
        send_cmd("RNTO %s\r\n", arg);   /* 重命名：指定新文件名 */
    } else if (strcmp(cmd, "rest") == 0) {
        send_cmd("REST %s\r\n", arg);   /* 断点续传：指定偏移量 */
    } else {
        printf("Unknown command: %s (try 'help')\n", cmd);
        return;     /* 注意：这里 return 了，不需要 read_response() */
    }

    /* 发送完命令后，读取服务器的响应 */
    read_response();
}


/* =================================================================
 * 主函数 — main()
 *
 * 程序的入口点。流程：
 *   1. 打印欢迎信息
 *   2. 如果命令行带了 IP 和端口，自动连接
 *   3. 进入"交互式循环"，不断读用户输入→执行命令
 *   4. 退出时发送 QUIT 并关闭连接
 *
 * 面试常问：argc 和 argv 是什么？
 *   argc（argument count）：命令行参数的数量
 *   argv（argument vector）：命令行参数的字符串数组
 *   比如执行 ./ftp 127.0.0.1 21：
 *     argc = 3
 *     argv[0] = "./ftp"
 *     argv[1] = "127.0.0.1"
 *     argv[2] = "21"
 * ================================================================= */

int main(int argc, char *argv[]) {
    char line[4096];    /* 存储用户输入的每一行命令 */

    printf("Tiny FTP Client\n");
    printf("Type 'help' for commands, 'quit' to exit.\n\n");

    /* --- 如果命令行带了参数，自动连接服务器 --- */

    if (argc > 1) {
        /* 从命令行参数获取服务器地址 */
        strncpy(server_ip, argv[1], sizeof(server_ip) - 1);
        if (argc > 2) server_port = atoi(argv[2]);

        /* 创建 socket */
        ctrl_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (ctrl_fd < 0) {
            perror("[connect] socket");
            return 1;
        }

        /* 准备服务器地址 */
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(server_port);

        /* 解析 IP 地址 */
        if (inet_pton(AF_INET, server_ip, &sa.sin_addr) <= 0) {
            printf("Invalid address: %s\n", server_ip);
            return 1;
        }

        /* 连接到服务器 */
        if (connect(ctrl_fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
            perror("[connect] connect");
            return 1;
        }

        printf("Connected to %s:%d\n", server_ip, server_port);
        read_response();    /* 读取 220 Welcome 欢迎消息 */
    }

    /* --- 交互式主循环 --- */

    /*
     * while (1)：无限循环，只有遇到 break、return 或 exit() 才会退出。
     * 每次循环：显示提示符 → 读取用户输入 → 处理命令。
     *
     * 这就是所谓的"REPL"模式：
     *   Read（读取输入）→ Evaluate（执行命令）→ Print（打印结果）→ Loop（重复）
     * 很多编程语言的交互式环境（如 Python 的 REPL）也是这样工作的。
     */
    while (1) {
        /*
         * 显示提示符 "ftp> "，让用户知道可以输入命令了。
         * fflush(stdout) 立即把输出刷到屏幕上，不缓存。
         * 因为 printf 默认是行缓冲的（遇到换行才输出），
         * 而 "ftp> " 没有换行，所以需要 fflush 强制输出。
         */
        printf("ftp> ");
        fflush(stdout);

        /*
         * fgets：从标准输入（键盘）读取一行文本。
         * 最多读 sizeof(line) - 1 个字节，留一个给 \0。
         * 用户按回车后，line 里包含用户输入的内容 + 换行符。
         *
         * 如果 fgets 返回 NULL，说明输入流关闭了（比如按 Ctrl+D），
         * 执行 break 跳出循环，程序结束。
         */
        if (!fgets(line, sizeof(line), stdin)) break;

        /* --- 去掉末尾的换行符 --- */

        size_t len = strlen(line);
        /*
         * 从末尾开始检查，把 \n（换行）和 \r（回车）替换成 \0。
         * while 循环可以处理多个结尾字符的情况。
         *
         * 比如用户输入 "list\n"：
         *   len = 5
         *   line[4] = '\n' → 设为 '\0'，len 变为 4
         *   退出循环
         *   line 变成了 "list"
         *
         * --len：先减 1 再取下标。因为数组下标从 0 开始，
         * 最后一个字符的下标是 len-1。
         */
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        /* 如果用户只按了回车（空行），跳过，继续循环 */
        if (len == 0) continue;

        /* 处理这行命令 */
        process_line(line);
    }

    /* --- 退出时清理 --- */

    if (ctrl_fd >= 0) {
        send_cmd("QUIT\r\n");   /* 告诉服务器我们要断开了 */
        close(ctrl_fd);          /* 关闭 socket 连接 */
    }

    return 0;   /* 返回 0 表示程序正常退出 */
}
