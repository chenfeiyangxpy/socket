#include "session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <limits.h>

/* 设置非阻塞 */
static int set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

void session_init(FtpSession *s, int fd, const struct sockaddr_in *addr) {
    memset(s, 0, sizeof(*s));
    s->ctrl_fd = fd;
    memcpy(&s->ctrl_addr, addr, sizeof(*addr));
    s->state = ST_WAIT_USER;
    s->xfer_fd = -1;
    s->xfer_mode = TYPE_ASCII;
    s->in_epoll = 0;
    data_conn_init(&s->data_conn);

    /* 默认家目录（登录后会更新） */
    strcpy(s->home_dir, "/tmp");
    s->current_dir[0] = '\0';

    set_nonblock(fd);
}

void session_reset(FtpSession *s) {
    data_conn_close(&s->data_conn);
    if (s->xfer_fd >= 0) {
        close(s->xfer_fd);
        s->xfer_fd = -1;
    }
    if (s->ctrl_fd >= 0) {
        close(s->ctrl_fd);
        s->ctrl_fd = -1;
    }
    s->state = ST_QUIT;
    s->xfer_type = XFER_NONE;
    s->rename_pending = 0;
    s->cmd_len = 0;
    s->resp_len = 0;
    s->resp_sent = 0;
}

/* 获取绝对路径（拼接home_dir和current_dir） */
void session_get_abs_path(FtpSession *s, char *buf, size_t size) {
    char tmp[1024];

    if (s->current_dir[0] == '\0' || strcmp(s->current_dir, "/") == 0) {
        snprintf(tmp, sizeof(tmp), "%s", s->home_dir);
    } else {
        snprintf(tmp, sizeof(tmp), "%s%s", s->home_dir, s->current_dir);
    }

    /* 截断以保证不超出目标缓冲区 */
    tmp[sizeof(tmp) - 1] = '\0';
    snprintf(buf, size, "%s", tmp);
}

/*
 * 解析用户输入的路径为绝对路径
 * 支持绝对路径（以/开头）和相对路径
 * 结果存放在abs_path中，已经过realpath解析
 */
void session_resolve_path(FtpSession *s, const char *input,
                          char *abs_path, size_t size) {
    char tmp[4096];
    char cwd[2048];

    session_get_abs_path(s, cwd, sizeof(cwd));

    if (input == NULL || input[0] == '\0') {
        snprintf(abs_path, size, "%s", cwd);
        return;
    }

    if (input[0] == '/') {
        /* 绝对路径：在home_dir下解析 */
        snprintf(tmp, sizeof(tmp), "%s%s", s->home_dir, input);
    } else {
        /* 相对路径：拼接当前目录 */
        snprintf(tmp, sizeof(tmp), "%s/%s", cwd, input);
    }
    /* 确保截断 */
    tmp[sizeof(tmp) - 1] = '\0';

    /* 使用realpath解析".."和"."等 */
    char *real = realpath(tmp, NULL);
    if (real) {
        snprintf(abs_path, size, "%s", real);
        free(real);
    } else {
        /* realpath失败（如路径尚不存在），使用原始拼接 */
        snprintf(abs_path, size, "%s", tmp);
    }
}

/* 检查路径是否在用户家目录内（安全性，防止../越狱） */
int session_path_safe(FtpSession *s, const char *abs_path) {
    char real_home[1024];
    char real_path[1024];
    char *p;

    p = realpath(s->home_dir, real_home);
    if (!p) return 0;

    p = realpath(abs_path, NULL);
    if (!p) return 0;
    strncpy(real_path, p, sizeof(real_path) - 1);
    free(p);

    /* 检查real_path是否以real_home开头 */
    size_t len = strlen(real_home);
    if (strncmp(real_path, real_home, len) != 0)
        return 0;
    if (real_path[len] != '\0' && real_path[len] != '/')
        return 0;

    return 1;
}

/* 发送响应到客户端（缓存到resp_buf，等待可写事件） */
void session_reply(FtpSession *s, const char *fmt, ...) {
    char buf[4096];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    int len = strlen(buf);

    /* 直接发送（非阻塞，可能发不完） */
    ssize_t n = write(s->ctrl_fd, buf, len);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        /* 缓存起来等可写事件 */
        if (s->resp_len + len < (int)sizeof(s->resp_buf)) {
            memcpy(s->resp_buf + s->resp_len, buf, len);
            s->resp_len += len;
        }
    }
    /* 如果只发了一部分，缓存剩余部分 */
    if (n > 0 && n < len) {
        if (s->resp_len + (len - n) < (int)sizeof(s->resp_buf)) {
            memcpy(s->resp_buf + s->resp_len, buf + n, len - n);
            s->resp_len += (len - n);
        }
    }

    printf("[%s] >>> %s", s->username, buf);
}
