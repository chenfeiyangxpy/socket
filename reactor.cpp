#include "reactor.h"
#include "session.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

int reactor_init(Reactor *r, int max_events) {
    r->epoll_fd = epoll_create(max_events);
    if (r->epoll_fd < 0) {
        perror("[reactor] epoll_create");
        return -1;
    }
    r->max_events = max_events;
    r->running = 0;
    r->timeout_ms = -1;
    return 0;
}

static int reactor_ctl(Reactor *r, int fd, uint32_t events,
                        struct FtpSession *session, int op) {
    struct epoll_event ev;
    ev.events = events;
    ev.data.ptr = session;
    return epoll_ctl(r->epoll_fd, op, fd, &ev);
}

int reactor_add(Reactor *r, int fd, uint32_t events,
                struct FtpSession *session) {
    return reactor_ctl(r, fd, events, session, EPOLL_CTL_ADD);
}

int reactor_mod(Reactor *r, int fd, uint32_t events,
                struct FtpSession *session) {
    return reactor_ctl(r, fd, events, session, EPOLL_CTL_MOD);
}

int reactor_del(Reactor *r, int fd) {
    return epoll_ctl(r->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
}

void reactor_run(Reactor *r, event_callback callback) {
    struct epoll_event *events;
    int i, n;

    events = (struct epoll_event*)malloc(
        sizeof(struct epoll_event) * r->max_events);
    if (!events) {
        perror("[reactor] malloc");
        return;
    }

    r->running = 1;

    while (r->running) {
        n = epoll_wait(r->epoll_fd, events, r->max_events, r->timeout_ms);

        if (n < 0) {
            if (errno == EINTR) continue;  /* 信号中断，继续 */
            perror("[reactor] epoll_wait");
            break;
        }

        for (i = 0; i < n; i++) {
            struct FtpSession *session =
                (struct FtpSession*)events[i].data.ptr;
            callback(session, events[i].events);
        }
    }

    free(events);
}

void reactor_stop(Reactor *r) {
    r->running = 0;
}

void reactor_destroy(Reactor *r) {
    if (r->epoll_fd >= 0) {
        close(r->epoll_fd);
        r->epoll_fd = -1;
    }
}
