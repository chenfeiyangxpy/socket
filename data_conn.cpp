#include "data_conn.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

void data_conn_init(DataConnection *dc) {
    memset(dc, 0, sizeof(*dc));
    dc->fd = -1;
    dc->listen_fd = -1;
    dc->mode = DATA_NONE;
}

/* 设置socket为非阻塞 */
static int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/*
 * PASV被动模式：服务器创建一个监听socket，返回分配的端口号
 * 客户端将通过此端口连接服务器进行数据传输
 */
int data_conn_setup_pasv(DataConnection *dc, int port_min, int port_max) {
    int fd, port, ret;
    struct sockaddr_in addr;
    int opt = 1;

    /* 创建TCP socket */
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("[data_conn] socket");
        return -1;
    }

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    set_nonblock(fd);

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;

    /* 在端口范围内尝试绑定 */
    for (port = port_min; port <= port_max; port++) {
        addr.sin_port = htons(port);
        ret = bind(fd, (struct sockaddr*)&addr, sizeof(addr));
        if (ret == 0) {
            goto bound;
        }
    }

    /* 范围内的端口都不可用，让系统自动分配 */
    addr.sin_port = htons(0);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("[data_conn] bind");
        close(fd);
        return -1;
    }

bound:
    if (listen(fd, 1) < 0) {
        perror("[data_conn] listen");
        close(fd);
        return -1;
    }

    /* 获取实际分配的端口号 */
    socklen_t addrlen = sizeof(addr);
    if (getsockname(fd, (struct sockaddr*)&addr, &addrlen) < 0) {
        perror("[data_conn] getsockname");
        close(fd);
        return -1;
    }

    dc->mode = DATA_PASV;
    dc->listen_fd = fd;
    dc->listen_port = ntohs(addr.sin_port);
    dc->fd = -1;  /* 尚未accept */

    return dc->listen_port;
}

/*
 * 接受PASV连接（非阻塞尝试）
 * 返回0表示成功，-1表示暂时无连接（EAGAIN）
 */
int data_conn_accept_pasv(DataConnection *dc) {
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);

    if (dc->mode != DATA_PASV || dc->listen_fd < 0)
        return -1;

    int fd = accept(dc->listen_fd, (struct sockaddr*)&addr, &addrlen);
    if (fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return -1;
        perror("[data_conn] accept");
        return -1;
    }

    set_nonblock(fd);
    dc->fd = fd;

    /* accept之后，监听socket可以关闭了 */
    close(dc->listen_fd);
    dc->listen_fd = -1;

    return 0;
}

/*
 * PORT主动模式：服务器主动连接客户端提供的数据端口
 */
int data_conn_connect_port(DataConnection *dc) {
    int fd;
    struct sockaddr_in addr;

    if (dc->mode != DATA_PORT)
        return -1;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("[data_conn] socket");
        return -1;
    }

    set_nonblock(fd);

    memcpy(&addr, &dc->remote_addr, sizeof(addr));
    addr.sin_port = htons(dc->remote_port);

    int ret = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        perror("[data_conn] connect");
        close(fd);
        return -1;
    }

    dc->fd = fd;
    return 0;  /* 连接可能还在进行中，需通过epoll检查可写 */
}

void data_conn_close(DataConnection *dc) {
    if (dc->fd >= 0) {
        close(dc->fd);
        dc->fd = -1;
    }
    if (dc->listen_fd >= 0) {
        close(dc->listen_fd);
        dc->listen_fd = -1;
    }
    dc->mode = DATA_NONE;
    dc->restart_pos = 0;
}

/* 格式化PORT参数：h1,h2,h3,h4,p1,p2 */
void data_conn_format_port(const struct sockaddr_in *addr, int port,
                           char *buf, int bufsize) {
    const unsigned char *ip = (const unsigned char*)&addr->sin_addr.s_addr;
    snprintf(buf, bufsize, "%u,%u,%u,%u,%d,%d",
             ip[0], ip[1], ip[2], ip[3],
             (port >> 8) & 0xFF, port & 0xFF);
}

/* 解析PORT命令参数 */
int data_conn_parse_port(DataConnection *dc, const char *port_arg) {
    unsigned int h1, h2, h3, h4, p1, p2;

    if (sscanf(port_arg, "%u,%u,%u,%u,%u,%u",
               &h1, &h2, &h3, &h4, &p1, &p2) != 6)
        return -1;

    if (h1 > 255 || h2 > 255 || h3 > 255 || h4 > 255 ||
        p1 > 255 || p2 > 255)
        return -1;

    memset(&dc->remote_addr, 0, sizeof(dc->remote_addr));
    dc->remote_addr.sin_family = AF_INET;
    dc->remote_addr.sin_addr.s_addr =
        (h1 << 24) | (h2 << 16) | (h3 << 8) | h4;
    dc->remote_port = (p1 << 8) | p2;

    dc->mode = DATA_PORT;
    return 0;
}
