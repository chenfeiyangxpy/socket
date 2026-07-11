#ifndef SESSION_H
#define SESSION_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <unistd.h>
#include "data_conn.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 会话状态 */
typedef enum {
    ST_WAIT_USER,      /* 等待USER命令 */
    ST_WAIT_PASS,      /* 已收到USER，等待PASS */
    ST_LOGGED_IN,      /* 已登录，接受命令 */
    ST_QUIT            /* 退出中 */
} SessionState;

/* 传输类型 */
typedef enum {
    XFER_NONE,
    XFER_RETR,         /* 下载：从服务器到客户端 */
    XFER_STOR,         /* 上传：从客户端到服务器 */
    XFER_LIST          /* 列表：发送目录列表 */
} XferType;

/* 传输模式（TYPE命令） */
typedef enum {
    TYPE_ASCII = 'A',
    TYPE_BINARY = 'I'
} TransferMode;

/* FTP会话结构 */
typedef struct FtpSession {
    /* 控制连接 */
    int ctrl_fd;                  /* 控制连接socket */
    struct sockaddr_in ctrl_addr; /* 客户端地址 */
    SessionState state;           /* 会话状态 */

    /* 用户信息 */
    char username[64];
    char home_dir[256];           /* 用户家目录（绝对路径） */
    char current_dir[256];        /* 当前工作目录（相对家目录的路径） */
    int login_fails;              /* 登录失败次数 */
    TransferMode xfer_mode;       /* 传输模式 A/I */

    /* 数据连接 */
    DataConnection data_conn;

    /* 传输状态 */
    XferType xfer_type;           /* 当前传输类型 */
    int xfer_fd;                  /* 正在传输的文件fd */
    off_t xfer_offset;            /* 断点续传偏移量 */

    /* 重命名状态（RNFR -> RNTO） */
    int rename_pending;
    char rename_from[256];

    /* 命令缓冲区 */
    char cmd_buf[4096];
    int cmd_len;

    /* 响应缓冲区 */
    char resp_buf[8192];
    int resp_len;
    int resp_sent;

    /* 是否已加入epoll */
    int in_epoll;
} FtpSession;

/* 初始化会话 */
void session_init(FtpSession *s, int fd, const struct sockaddr_in *addr);

/* 重置会话（用于断开后重用） */
void session_reset(FtpSession *s);

/* 获取当前绝对路径（写入buf） */
void session_get_abs_path(FtpSession *s, char *buf, size_t size);

/* 获取完整路径（相对cwd解析） */
void session_resolve_path(FtpSession *s, const char *input,
                          char *abs_path, size_t size);

/* 检查路径是否在用户家目录内（安全限制） */
int session_path_safe(FtpSession *s, const char *abs_path);

/* 发送响应 */
void session_reply(FtpSession *s, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* SESSION_H */
