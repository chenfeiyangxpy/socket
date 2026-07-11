/*
 * server.cpp - Tiny FTP Server
 * 基于 epoll 的 Reactor 模式，遵循 RFC 959
 *
 * 编译: g++ -std=c++11 -O2 -Wall -o ftpd server.cpp session.cpp commands.cpp
 *               config.cpp auth.cpp data_conn.cpp reactor.cpp
 * 运行: sudo ./ftpd -d -c /etc/ftpd/ftpd.conf
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <getopt.h>

/* 需要 C++ 标准库头文件 */
#include <string>
#include <vector>
#include <algorithm>

#include "config.h"
#include "auth.h"
#include "reactor.h"
#include "session.h"
#include "commands.h"

/* ========== 全局变量 ========== */

ServerConfig g_config;          /* 全局配置 */
char g_user_file[256];          /* 用户文件路径，供auth模块使用 */
static Reactor g_reactor;       /* epoll 事件循环 */
static int g_listen_fd = -1;    /* 监听socket */
static int g_max_sessions = 0;  /* 最大会话数 */
static FtpSession **g_sessions = NULL;  /* 会话池（简易管理） */

/* ========== 辅助函数 ========== */

/* 创建监听socket */
static int create_listen_fd(const ServerConfig *cfg) {
    int fd;
    struct sockaddr_in addr;
    int opt = 1;

    fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        perror("[listen] socket");
        return -1;
    }

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg->port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("[listen] bind");
        close(fd);
        return -1;
    }

    if (listen(fd, 128) < 0) {
        perror("[listen] listen");
        close(fd);
        return -1;
    }

    return fd;
}

/* ========== 会话管理 ========== */

static FtpSession* session_create(int fd, const struct sockaddr_in *addr) {
    int i;
    FtpSession *s = NULL;

    /* 在池中查找空位 */
    for (i = 0; i < g_max_sessions; i++) {
        if (g_sessions[i] == NULL) {
            s = (FtpSession*)calloc(1, sizeof(FtpSession));
            if (!s) return NULL;
            g_sessions[i] = s;
            break;
        }
    }

    if (!s) {
        /* 池满，拒绝连接 */
        close(fd);
        return NULL;
    }

    session_init(s, fd, addr);
    return s;
}

static void session_destroy(FtpSession *s) {
    int i;

    /* 从epoll中移除 */
    if (s->in_epoll && s->ctrl_fd >= 0) {
        reactor_del(&g_reactor, s->ctrl_fd);
    }

    /* 清理资源 */
    session_reset(s);

    /* 从池中移除 */
    for (i = 0; i < g_max_sessions; i++) {
        if (g_sessions[i] == s) {
            g_sessions[i] = NULL;
            break;
        }
    }

    free(s);
}

/* ========== 事件处理 ========== */

/* 处理控制连接可读事件 */
static void handle_ctrl_read(FtpSession *s) {
    char buf[4096];
    ssize_t n;
    char *p;

    n = read(s->ctrl_fd, buf, sizeof(buf) - 1);
    if (n <= 0) {
        /* 客户端断开 */
        printf("[session] Client disconnected (fd=%d)\n", s->ctrl_fd);
        session_destroy(s);
        return;
    }

    buf[n] = '\0';

    /* 追加到命令缓冲区 */
    int space = sizeof(s->cmd_buf) - s->cmd_len - 1;
    if (space > n) space = n;
    memcpy(s->cmd_buf + s->cmd_len, buf, space);
    s->cmd_len += space;
    s->cmd_buf[s->cmd_len] = '\0';

    /* 处理完整的行（以\r\n或\n结尾） */
    while ((p = strstr(s->cmd_buf, "\r\n")) || (p = strchr(s->cmd_buf, '\n'))) {
        *p = '\0';
        int remaining = s->cmd_len - (p - s->cmd_buf + 2);
        if (remaining < 0) remaining = s->cmd_len - (p - s->cmd_buf + 1);

        printf("[session] CMD: %s\n", s->cmd_buf);

        /* 分发命令 */
        int ret = cmd_dispatch(s, s->cmd_buf);

        if (ret < 0) {
            session_destroy(s);
            return;
        }

        /* 移动剩余数据 */
        char *next = p + ((p[1] == '\n') ? 2 : 1);
        memmove(s->cmd_buf, next, remaining);
        s->cmd_len = remaining;
        s->cmd_buf[s->cmd_len] = '\0';
    }
}

/* Reactor事件回调 */
static void on_event(struct FtpSession *session, uint32_t events) {
    if (!session) return;

    if (events & EPOLLIN) {
        handle_ctrl_read(session);
    }

    if (events & EPOLLERR || events & EPOLLHUP) {
        printf("[session] Error/ Hangup on fd=%d\n", session->ctrl_fd);
        session_destroy(session);
    }

    /* 处理可写事件（发送缓存的响应） */
    if (events & EPOLLOUT && session->resp_len > session->resp_sent) {
        int remaining = session->resp_len - session->resp_sent;
        ssize_t n = write(session->ctrl_fd,
                          session->resp_buf + session->resp_sent,
                          remaining);
        if (n > 0) {
            session->resp_sent += n;
            if (session->resp_sent >= session->resp_len) {
                session->resp_len = 0;
                session->resp_sent = 0;
                /* 如果不再有数据可写，停止监听EPOLLOUT */
                reactor_mod(&g_reactor, session->ctrl_fd,
                            EPOLLIN, session);
            }
        }
    }
}

/* ========== 信号处理 ========== */

static volatile int g_shutdown = 0;

static void sig_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        g_shutdown = 1;
        reactor_stop(&g_reactor);
    }
}

/* ========== 主函数 ========== */

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  -d          Run as daemon\n"
        "  -c <file>   Config file path\n"
        "  -p <port>   Listen port (override config)\n"
        "  -h          Show this help\n",
        prog);
}

int main(int argc, char *argv[]) {
    int opt;
    int daemon_mode = 0;
    const char *config_path = NULL;
    int port_override = 0;

    /* 解析命令行参数 */
    while ((opt = getopt(argc, argv, "dc:p:h")) != -1) {
        switch (opt) {
            case 'd': daemon_mode = 1; break;
            case 'c': config_path = optarg; break;
            case 'p': port_override = atoi(optarg); break;
            case 'h': usage(argv[0]); return 0;
            default: usage(argv[0]); return 1;
        }
    }

    /* 加载配置 */
    config_load(&g_config, config_path);
    if (port_override > 0) g_config.port = port_override;
    snprintf(g_user_file, sizeof(g_user_file), "%s", g_config.user_file);

    config_print(&g_config);

    /* Daemon模式 */
    if (daemon_mode) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("[main] fork");
            return 1;
        }
        if (pid > 0) {
            printf("[main] Daemon started, PID=%d\n", pid);
            _exit(0);
        }
        setsid();
        /* 关闭标准输入输出 */
        close(0); close(1); close(2);
        open("/dev/null", O_RDONLY);
        open("/dev/null", O_WRONLY);
        open("/dev/null", O_WRONLY);
    }

    /* 设置信号处理器 */
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);  /* 忽略SIGPIPE */

    /* 创建监听socket */
    g_listen_fd = create_listen_fd(&g_config);
    if (g_listen_fd < 0) {
        return 1;
    }

    printf("[main] Listening on port %d...\n", g_config.port);

    /* 初始化Reactor */
    g_max_sessions = g_config.max_clients + 10;
    if (reactor_init(&g_reactor, 64) < 0) {
        close(g_listen_fd);
        return 1;
    }

    /* 创建会话指针池 */
    g_sessions = (FtpSession**)calloc(g_max_sessions, sizeof(FtpSession*));
    if (!g_sessions) {
        perror("[main] calloc");
        reactor_destroy(&g_reactor);
        close(g_listen_fd);
        return 1;
    }

    /* 将监听socket加入epoll */
    {
        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.ptr = NULL;  /* 空指针表示监听socket */
        if (epoll_ctl(g_reactor.epoll_fd, EPOLL_CTL_ADD, g_listen_fd, &ev) < 0) {
            perror("[main] epoll_ctl listen");
            reactor_destroy(&g_reactor);
            close(g_listen_fd);
            return 1;
        }
    }

    /* 事件循环 */
    struct epoll_event events[64];

    while (!g_shutdown) {
        int n = epoll_wait(g_reactor.epoll_fd, events, 64, 1000);

        if (n < 0) {
            if (errno == EINTR) continue;
            perror("[main] epoll_wait");
            break;
        }

        for (int i = 0; i < n; i++) {
            if (events[i].data.ptr == NULL) {
                /* 监听socket有新连接 */
                struct sockaddr_in client_addr;
                socklen_t addrlen = sizeof(client_addr);

                while (1) {
                    int client_fd = accept4(g_listen_fd,
                        (struct sockaddr*)&client_addr,
                        &addrlen, SOCK_NONBLOCK);
                    if (client_fd < 0) {
                        if (errno == EAGAIN) break;
                        if (errno == EMFILE) {
                            fprintf(stderr, "[main] Too many open files\n");
                            break;
                        }
                        perror("[main] accept");
                        break;
                    }

                    printf("[main] New connection from %s:%d\n",
                           inet_ntoa(client_addr.sin_addr),
                           ntohs(client_addr.sin_port));

                    FtpSession *s = session_create(client_fd, &client_addr);
                    if (!s) {
                        printf("[main] Connection rejected (max sessions)\n");
                        close(client_fd);
                        continue;
                    }

                    /* 加入epoll */
                    if (reactor_add(&g_reactor, client_fd, EPOLLIN, s) < 0) {
                        session_destroy(s);
                        continue;
                    }
                    s->in_epoll = 1;

                    /* 发送欢迎消息 */
                    session_reply(s, "220 %s\r\n", g_config.welcome_msg);
                }
            } else {
                /* 客户端会话事件 */
                on_event((FtpSession*)events[i].data.ptr, events[i].events);
            }
        }
    }

    /* 清理 */
    printf("\n[main] Shutting down...\n");

    for (int i = 0; i < g_max_sessions; i++) {
        if (g_sessions[i]) {
            session_destroy(g_sessions[i]);
        }
    }

    free(g_sessions);
    reactor_destroy(&g_reactor);

    if (g_listen_fd >= 0) close(g_listen_fd);

    printf("[main] Goodbye.\n");
    return 0;
}
