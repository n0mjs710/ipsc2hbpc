/* eventloop.c — poll()-based loop with monotonic one-shot timers. */
#include "eventloop.h"
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <time.h>
#include <errno.h>

#define MAX_FDS 8

struct ev_timer {
    double      when;       /* monotonic fire time */
    ev_timer_cb cb;
    void       *ud;
    int         active;
    ev_timer   *next;
};

struct ev_loop {
    struct {
        int      fd;
        ev_fd_cb cb;
        void    *ud;
    } fds[MAX_FDS];
    int        nfds;
    ev_timer  *timers;      /* singly-linked list of active timers */
    int        running;
};

ev_loop *ev_new(void)
{
    ev_loop *l = calloc(1, sizeof *l);
    return l;
}

void ev_free(ev_loop *loop)
{
    if (!loop) return;
    ev_timer *t = loop->timers;
    while (t) { ev_timer *n = t->next; free(t); t = n; }
    free(loop);
}

double ev_now(ev_loop *loop)
{
    (void)loop;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

void ev_add_fd(ev_loop *loop, int fd, ev_fd_cb cb, void *ud)
{
    for (int i = 0; i < loop->nfds; i++) {
        if (loop->fds[i].fd == fd) {
            loop->fds[i].cb = cb; loop->fds[i].ud = ud;
            return;
        }
    }
    if (loop->nfds < MAX_FDS) {
        loop->fds[loop->nfds].fd = fd;
        loop->fds[loop->nfds].cb = cb;
        loop->fds[loop->nfds].ud = ud;
        loop->nfds++;
    }
}

void ev_del_fd(ev_loop *loop, int fd)
{
    for (int i = 0; i < loop->nfds; i++) {
        if (loop->fds[i].fd == fd) {
            loop->fds[i] = loop->fds[loop->nfds - 1];
            loop->nfds--;
            return;
        }
    }
}

ev_timer *ev_timer_at(ev_loop *loop, double when, ev_timer_cb cb, void *ud)
{
    ev_timer *t = calloc(1, sizeof *t);
    t->when = when; t->cb = cb; t->ud = ud; t->active = 1;
    t->next = loop->timers;
    loop->timers = t;
    return t;
}

ev_timer *ev_timer_after(ev_loop *loop, double delay, ev_timer_cb cb, void *ud)
{
    return ev_timer_at(loop, ev_now(loop) + delay, cb, ud);
}

void ev_timer_cancel(ev_loop *loop, ev_timer *t)
{
    (void)loop;
    if (t) t->active = 0;   /* reaped on next loop pass */
}

static void reap_inactive(ev_loop *loop)
{
    ev_timer **pp = &loop->timers;
    while (*pp) {
        if (!(*pp)->active) {
            ev_timer *dead = *pp;
            *pp = dead->next;
            free(dead);
        } else {
            pp = &(*pp)->next;
        }
    }
}

void ev_run(ev_loop *loop)
{
    loop->running = 1;
    while (loop->running) {
        reap_inactive(loop);

        /* find soonest active timer */
        double now = ev_now(loop);
        double next = -1.0;
        for (ev_timer *t = loop->timers; t; t = t->next)
            if (t->active && (next < 0 || t->when < next))
                next = t->when;

        int timeout_ms = -1;
        if (next >= 0) {
            double d = (next - now) * 1000.0;
            timeout_ms = d <= 0 ? 0 : (int)d;
        }

        struct pollfd pfd[MAX_FDS];
        int map[MAX_FDS];
        int n = 0;
        for (int i = 0; i < loop->nfds; i++) {
            pfd[n].fd = loop->fds[i].fd;
            pfd[n].events = POLLIN;
            pfd[n].revents = 0;
            map[n] = i;
            n++;
        }

        int r = poll(pfd, n, timeout_ms);
        if (r < 0) {
            if (errno == EINTR) continue;   /* signal — re-evaluate */
            break;
        }

        /* fire expired timers (snapshot fire set first; callbacks may add timers) */
        now = ev_now(loop);
        for (;;) {
            ev_timer *fire = NULL;
            for (ev_timer *t = loop->timers; t; t = t->next)
                if (t->active && t->when <= now) {
                    if (!fire || t->when < fire->when) fire = t;
                }
            if (!fire) break;
            fire->active = 0;            /* one-shot */
            fire->cb(loop, fire->ud);
            if (!loop->running) break;
        }
        reap_inactive(loop);

        if (r > 0) {
            for (int i = 0; i < n; i++) {
                if (pfd[i].revents & (POLLIN | POLLERR | POLLHUP)) {
                    int idx = map[i];
                    if (idx < loop->nfds && loop->fds[idx].fd == pfd[i].fd)
                        loop->fds[idx].cb(loop, pfd[i].fd, loop->fds[idx].ud);
                }
                if (!loop->running) break;
            }
        }
    }
}

void ev_stop(ev_loop *loop)
{
    loop->running = 0;
}
