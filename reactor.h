#ifndef REACTOR_H
#define REACTOR_H

#include <sys/epoll.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 前向声明 */
struct FtpSession;

/* 事件类型 */
#define EV_READ  EPOLLIN
#define EV_WRITE EPOLLOUT
#define EV_ET    EPOLLET  /* 边缘触发 */

/* 事件回调函数类型 */
typedef void (*event_callback)(struct FtpSession *session, uint32_t events);

/* Reactor事件循环 */
typedef struct {
    int epoll_fd;
    int max_events;
    int running;
    int timeout_ms;  /* epoll_wait超时，-1为阻塞等待 */
} Reactor;

/* 初始化Reactor，max_events为epoll_wait一次最多返回的事件数 */
int reactor_init(Reactor *r, int max_events);

/* 添加文件描述符到事件循环 */
int reactor_add(Reactor *r, int fd, uint32_t events, struct FtpSession *session);

/* 修改文件描述符监听事件 */
int reactor_mod(Reactor *r, int fd, uint32_t events, struct FtpSession *session);

/* 从事件循环删除文件描述符 */
int reactor_del(Reactor *r, int fd);

/* 启动事件循环，每次有事件时调用callback */
/* callback(session, events) - events为EPOLLIN/EPOLLOUT等 */
void reactor_run(Reactor *r, event_callback callback);

/* 停止事件循环 */
void reactor_stop(Reactor *r);

/* 销毁Reactor */
void reactor_destroy(Reactor *r);

#ifdef __cplusplus
}
#endif

#endif /* REACTOR_H */
