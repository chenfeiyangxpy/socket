/*
 * client.cpp - 简单的FTP命令行客户端
 * 支持标准FTP命令（USER, PASS, LIST, RETR, STOR, PASV等）
 *
 * 编译: g++ -std=c++11 -O2 -Wall -o ftp client.cpp
 * 运行: ./ftp
 * 或:   ./ftp <server> <port>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

/* ========== 全局状态 ========== */

static int ctrl_fd = -1;       /* 控制连接 */
static int data_fd = -1;       /* 数据连接 */
static int data_listen_fd = -1; /* PASV数据监听 */
static int logged_in = 0;
static char server_ip[64] = "127.0.0.1";
static int server_port = 21;

/* 接收缓冲区 */
static char recv_buf[65536];
static int recv_len = 0;

/* ========== 工具函数 ========== */

/* 发送命令到服务器 */
static void send_cmd(const char *fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    int len = strlen(buf);
    ssize_t ret = write(ctrl_fd, buf, len);
    (void)ret;
    printf(">>> %s", buf);
}

/* 读取服务器响应（阻塞直到收到完整响应） */
static int read_response() {
    char buf[4096];
    ssize_t n;

    recv_len = 0;  /* 清空接收缓冲区 */

    while (1) {
        n = read(ctrl_fd, buf, sizeof(buf) - 1);
        if (n <= 0) {
            printf("[!] Connection closed by server\n");
            return -1;
        }
        buf[n] = '\0';

        /* 保存到全局缓冲区（供PASV解析使用） */
        if (recv_len + n < (int)sizeof(recv_buf) - 1) {
            memcpy(recv_buf + recv_len, buf, n);
            recv_len += n;
            recv_buf[recv_len] = '\0';
        }

        printf("%s", buf);

        /* 检查响应是否完整 */
        /* 单行响应: "NNN ...\r\n" 或 "NNN ...\n" */
        /* 多行响应: "NNN-...\r\n...\r\nNNN ...\r\n" */
        if (n >= 4) {
            char *last_line = recv_buf;
            for (int i = recv_len - 5; i >= 0; i--) {
                if (recv_buf[i] == '\n') {
                    last_line = recv_buf + i + 1;
                    break;
                }
            }
            int llen = strlen(last_line);
            if (llen >= 4 && last_line[3] == ' ') {
                return atoi(last_line);
            }
        }
    }
}

/* 建立数据连接（PASV模式） */
static int connect_data_pasv(const char *pasv_resp) {
    unsigned int h1, h2, h3, h4, p1, p2;
    int data_port;
    char addr[64];

    /* 解析PASV响应中的地址 */
    /* 227 Entering Passive Mode (h1,h2,h3,h4,p1,p2) */
    const char *p = strchr(pasv_resp, '(');
    if (!p) return -1;
    p++;
    if (sscanf(p, "%u,%u,%u,%u,%u,%u", &h1, &h2, &h3, &h4, &p1, &p2) != 6)
        return -1;

    snprintf(addr, sizeof(addr), "%u.%u.%u.%u", h1, h2, h3, h4);
    data_port = (p1 << 8) | p2;

    /* 创建数据连接 */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("[data] socket");
        return -1;
    }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(data_port);
    inet_pton(AF_INET, addr, &sa.sin_addr);

    if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        perror("[data] connect");
        close(fd);
        return -1;
    }

    data_fd = fd;
    printf("[data] PASV connected to %s:%d\n", addr, data_port);
    return 0;
}

/* 关闭数据连接 */
static void close_data() {
    if (data_fd >= 0) {
        close(data_fd);
        data_fd = -1;
    }
    if (data_listen_fd >= 0) {
        close(data_listen_fd);
        data_listen_fd = -1;
    }
}

/* ========== 文件传输函数 ========== */

/* RETR：从服务器下载文件 */
static void cmd_retr(const char *arg) {
    if (!arg || !*arg) {
        printf("Usage: get <filename>\n");
        return;
    }
    if (data_fd < 0) {
        printf("Use 'pasv' first.\n");
        return;
    }

    char filename[256];
    /* 使用文件名（去掉路径） */
    const char *slash = strrchr(arg, '/');
    if (slash)
        snprintf(filename, sizeof(filename), "%s", slash + 1);
    else
        snprintf(filename, sizeof(filename), "%s", arg);

    /* 发送RETR命令 */
    send_cmd("RETR %s\r\n", arg);
    int code = read_response();

    if (code != 150 && code != 125) {
        printf("[!] Download rejected\n");
        close_data();
        return;
    }

    /* 接收文件数据 */
    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        perror("[retr] open");
        close_data();
        return;
    }

    char buf[8192];
    ssize_t n;
    off_t total = 0;

    while ((n = read(data_fd, buf, sizeof(buf))) > 0) {
        ssize_t w = write(fd, buf, n);
        (void)w;
        total += n;
    }

    close(fd);
    close_data();
    printf("[retr] Downloaded %lld bytes to %s\n", (long long)total, filename);

    read_response(); /* 读取 226 Transfer complete */
}

/* STOR：上传文件到服务器 */
static void cmd_stor(const char *arg) {
    if (!arg || !*arg) {
        printf("Usage: put <local_file>\n");
        return;
    }
    if (data_fd < 0) {
        printf("Use 'pasv' first.\n");
        return;
    }

    /* 检查本地文件 */
    int fd = open(arg, O_RDONLY);
    if (fd < 0) {
        perror("[stor] open");
        return;
    }

    /* 获取文件名 */
    const char *slash = strrchr(arg, '/');
    const char *filename = slash ? slash + 1 : arg;

    /* 发送STOR命令 */
    send_cmd("STOR %s\r\n", filename);
    int code = read_response();

    if (code != 150 && code != 125) {
        printf("[!] Upload rejected\n");
        close(fd);
        close_data();
        return;
    }

    /* 发送文件数据 */
    char buf[8192];
    ssize_t n;
    off_t total = 0;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        ssize_t written = 0;
        while (written < n) {
            ssize_t w = write(data_fd, buf + written, n - written);
            if (w < 0) {
                perror("[stor] write");
                goto stor_done;
            }
            written += w;
            total += w;
        }
    }

stor_done:
    close(fd);
    close_data();
    printf("[stor] Uploaded %lld bytes\n", (long long)total);

    read_response(); /* 读取 226 Transfer complete */
}

/* ========== 交互式命令处理 ========== */

static void process_line(const char *line) {
    char cmd[64], arg[1024];
    int i;

    /* 跳过前导空白 */
    while (*line == ' ') line++;
    if (!*line) return;

    /* 提取命令 */
    const char *space = strchr(line, ' ');
    if (space) {
        size_t clen = space - line;
        if (clen >= sizeof(cmd)) clen = sizeof(cmd) - 1;
        memcpy(cmd, line, clen);
        cmd[clen] = '\0';
        snprintf(arg, sizeof(arg), "%s", space + 1);
    } else {
        snprintf(cmd, sizeof(cmd), "%s", line);
        arg[0] = '\0';
    }

    /* 转为小写 */
    for (i = 0; cmd[i]; i++)
        if (cmd[i] >= 'A' && cmd[i] <= 'Z')
            cmd[i] += 32;

    /* 内部命令（客户端本地处理） */
    if (strcmp(cmd, "open") == 0) {
        /* open <host> [port] */
        char host[256];
        int port = 21;
        if (sscanf(arg, "%s %d", host, &port) < 1) {
            printf("Usage: open <host> [port]\n");
            return;
        }

        /* 解析主机名 */
        struct hostent *he = gethostbyname(host);
        if (!he) {
            printf("Unknown host: %s\n", host);
            return;
        }

        strncpy(server_ip, inet_ntoa(*(struct in_addr*)he->h_addr),
                sizeof(server_ip) - 1);
        server_port = port;

        /* 连接 */
        if (ctrl_fd >= 0) close(ctrl_fd);
        ctrl_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (ctrl_fd < 0) {
            perror("[open] socket");
            return;
        }

        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(server_port);
        inet_pton(AF_INET, server_ip, &sa.sin_addr);

        if (connect(ctrl_fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
            perror("[open] connect");
            close(ctrl_fd);
            ctrl_fd = -1;
            return;
        }

        logged_in = 0;
        printf("Connected to %s:%d\n", server_ip, server_port);
        read_response(); /* 220 Welcome */
        return;
    }

    if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
        if (ctrl_fd >= 0) {
            send_cmd("QUIT\r\n");
            read_response();
        }
        exit(0);
    }

    if (strcmp(cmd, "passive") == 0 || strcmp(cmd, "pasv") == 0) {
        send_cmd("PASV\r\n");
        int code = read_response();
        if (code == 227) {
            connect_data_pasv(recv_buf);
        }
        return;
    }

    if (strcmp(cmd, "get") == 0) {
        cmd_retr(arg);
        return;
    }

    if (strcmp(cmd, "put") == 0) {
        cmd_stor(arg);
        return;
    }

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        printf(
            "Commands:\n"
            "  open <host> [port]  Connect to FTP server\n"
            "  user <name>         Send username\n"
            "  pass <pass>         Send password\n"
            "  pasv                Enter passive mode\n"
            "  list [path]         List directory\n"
            "  get <file>          Download file (use after pasv)\n"
            "  put <file>          Upload file (use after pasv)\n"
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

    /* list: 发送 LIST 命令并读取数据通道的目录列表 */
    if (strcmp(cmd, "list") == 0) {
        if (arg[0]) send_cmd("LIST %s\r\n", arg);
        else send_cmd("LIST\r\n");
        int code = read_response();  /* 期望 150 */
        if ((code == 150 || code == 125) && data_fd >= 0) {
            /* 读取目录列表数据并打印 */
            char dbuf[4096];
            ssize_t n;
            while ((n = read(data_fd, dbuf, sizeof(dbuf) - 1)) > 0) {
                dbuf[n] = '\0';
                printf("%s", dbuf);
            }
            close_data();
        }
        read_response();  /* 226 Directory send OK */
        return;
    }

    /* nlst: 类似 list 但只输出文件名 */
    /* 自动构造标准FTP命令 */
    if (strcmp(cmd, "user") == 0) {
        send_cmd("USER %s\r\n", arg);
    } else if (strcmp(cmd, "pass") == 0) {
        send_cmd("PASS %s\r\n", arg);
    } else if (strcmp(cmd, "pwd") == 0) {
        send_cmd("PWD\r\n");
    } else if (strcmp(cmd, "cwd") == 0 || strcmp(cmd, "cd") == 0) {
        send_cmd("CWD %s\r\n", arg);
    } else if (strcmp(cmd, "mkd") == 0) {
        send_cmd("MKD %s\r\n", arg);
    } else if (strcmp(cmd, "rmd") == 0) {
        send_cmd("RMD %s\r\n", arg);
    } else if (strcmp(cmd, "dele") == 0) {
        send_cmd("DELE %s\r\n", arg);
    } else if (strcmp(cmd, "syst") == 0) {
        send_cmd("SYST\r\n");
    } else if (strcmp(cmd, "size") == 0) {
        send_cmd("SIZE %s\r\n", arg);
    } else if (strcmp(cmd, "type") == 0) {
        send_cmd("TYPE %s\r\n", arg);
    } else if (strcmp(cmd, "noop") == 0) {
        send_cmd("NOOP\r\n");
    } else if (strcmp(cmd, "feat") == 0) {
        send_cmd("FEAT\r\n");
    } else if (strcmp(cmd, "rnfr") == 0) {
        send_cmd("RNFR %s\r\n", arg);
    } else if (strcmp(cmd, "rnto") == 0) {
        send_cmd("RNTO %s\r\n", arg);
    } else if (strcmp(cmd, "rest") == 0) {
        send_cmd("REST %s\r\n", arg);
    } else {
        printf("Unknown command: %s (try 'help')\n", cmd);
        return;
    }

    read_response();
}

/* ========== 主函数 ========== */

int main(int argc, char *argv[]) {
    char line[4096];

    printf("Tiny FTP Client\n");
    printf("Type 'help' for commands, 'quit' to exit.\n\n");

    /* 解析命令行参数 */
    if (argc > 1) {
        strncpy(server_ip, argv[1], sizeof(server_ip) - 1);
        if (argc > 2) server_port = atoi(argv[2]);

        /* 自动连接 */
        ctrl_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (ctrl_fd < 0) {
            perror("[connect] socket");
            return 1;
        }

        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(server_port);
        if (inet_pton(AF_INET, server_ip, &sa.sin_addr) <= 0) {
            printf("Invalid address: %s\n", server_ip);
            return 1;
        }

        if (connect(ctrl_fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
            perror("[connect] connect");
            return 1;
        }

        printf("Connected to %s:%d\n", server_ip, server_port);
        read_response(); /* 220 Welcome */
    }

    /* 交互式主循环 */
    while (1) {
        printf("ftp> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) break;

        /* 去除换行 */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        if (len == 0) continue;

        process_line(line);
    }

    if (ctrl_fd >= 0) {
        send_cmd("QUIT\r\n");
        close(ctrl_fd);
    }

    return 0;
}
