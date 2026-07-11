#ifndef DATA_CONN_H
#define DATA_CONN_H

#include <sys/socket.h>
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 数据连接模式 */
typedef enum {
    DATA_NONE = 0,   /* 无数据连接 */
    DATA_PASV,       /* 被动模式：服务器监听，客户端连接 */
    DATA_PORT        /* 主动模式：服务器主动连接客户端 */
} DataMode;

/* 数据连接状态 */
typedef struct {
    DataMode mode;
    int fd;             /* 已建立的数据socket (用于传输) */
    int listen_fd;      /* PASV模式：监听socket */
    int listen_port;    /* PASV模式：监听端口 */
    int remote_port;    /* PORT模式：客户端提供的数据端口 */
    struct sockaddr_in remote_addr;  /* PORT模式：客户端地址 */
    off_t restart_pos;  /* REST命令设置的断点位置 */
} DataConnection;

/* 初始化数据连接 */
void data_conn_init(DataConnection *dc);

/* PASV模式：创建监听socket，返回分配的端口号（负数为失败） */
int data_conn_setup_pasv(DataConnection *dc, int port_min, int port_max);

/* 接受PASV连接（非阻塞尝试），成功返回0 */
int data_conn_accept_pasv(DataConnection *dc);

/* PORT模式：主动连接客户端，成功返回0 */
int data_conn_connect_port(DataConnection *dc);

/* 关闭数据连接 */
void data_conn_close(DataConnection *dc);

/* 格式化PORT命令参数为字符串（h1,h2,h3,h4,p1,p2） */
void data_conn_format_port(const struct sockaddr_in *addr, int port,
                           char *buf, int bufsize);

/* 解析PORT命令参数 */
int data_conn_parse_port(DataConnection *dc, const char *port_arg);

#ifdef __cplusplus
}
#endif

#endif /* DATA_CONN_H */
