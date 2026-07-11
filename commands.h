#ifndef COMMANDS_H
#define COMMANDS_H

#include "session.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * FTP命令处理函数
 * 每个函数接收会话指针和命令参数字符串
 * 命令参数不包含命令本身（例如CWD /pub，arg为"/pub"）
 * 返回0表示继续，-1表示需要断开连接
 */

/* 认证相关 */
int cmd_USER(FtpSession *s, const char *arg);
int cmd_PASS(FtpSession *s, const char *arg);
int cmd_QUIT(FtpSession *s, const char *arg);

/* 目录操作 */
int cmd_PWD(FtpSession *s, const char *arg);
int cmd_CWD(FtpSession *s, const char *arg);
int cmd_CDUP(FtpSession *s, const char *arg);
int cmd_MKD(FtpSession *s, const char *arg);
int cmd_RMD(FtpSession *s, const char *arg);

/* 文件操作 */
int cmd_DELE(FtpSession *s, const char *arg);
int cmd_RNFR(FtpSession *s, const char *arg);
int cmd_RNTO(FtpSession *s, const char *arg);
int cmd_SIZE(FtpSession *s, const char *arg);

/* 列表 */
int cmd_LIST(FtpSession *s, const char *arg);
int cmd_NLST(FtpSession *s, const char *arg);

/* 传输设置 */
int cmd_TYPE(FtpSession *s, const char *arg);
int cmd_REST(FtpSession *s, const char *arg);

/* 数据连接 */
int cmd_PASV(FtpSession *s, const char *arg);
int cmd_PORT(FtpSession *s, const char *arg);

/* 文件传输 */
int cmd_RETR(FtpSession *s, const char *arg);
int cmd_STOR(FtpSession *s, const char *arg);

/* 其他 */
int cmd_SYST(FtpSession *s, const char *arg);
int cmd_NOOP(FtpSession *s, const char *arg);
int cmd_FEAT(FtpSession *s, const char *arg);
int cmd_HELP(FtpSession *s, const char *arg);
int cmd_UNKNOWN(FtpSession *s, const char *arg);

/* 命令分发表 */
typedef struct {
    const char *name;               /* 命令名（大写） */
    int (*handler)(FtpSession*, const char*);  /* 处理函数 */
    int need_login;                 /* 是否需要登录 */
} CommandEntry;

extern CommandEntry cmd_table[];

/* 查找并执行命令 */
int cmd_dispatch(FtpSession *s, const char *line);

#ifdef __cplusplus
}
#endif

#endif /* COMMANDS_H */
