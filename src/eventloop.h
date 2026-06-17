/* eventloop.h — single-threaded poll() event loop with monotonic timers.
 * Replaces the Python asyncio loop: UDP read callbacks + call_at/call_later. */
#ifndef EVENTLOOP_H
#define EVENTLOOP_H

typedef struct ev_loop ev_loop;
typedef struct ev_timer ev_timer;

typedef void (*ev_fd_cb)(ev_loop *loop, int fd, void *ud);
typedef void (*ev_timer_cb)(ev_loop *loop, void *ud);

ev_loop *ev_new(void);
void     ev_free(ev_loop *loop);

/* Monotonic clock in seconds (matches asyncio loop.time()). */
double ev_now(ev_loop *loop);

/* Register/unregister a UDP (or any) fd for read-readiness callbacks. */
void ev_add_fd(ev_loop *loop, int fd, ev_fd_cb cb, void *ud);
void ev_del_fd(ev_loop *loop, int fd);

/* One-shot timers.  Return a handle that can be cancelled until it fires.
 * After a timer fires its handle is freed — null your stored pointer in the
 * callback (as the Python code does) and only cancel non-null handles. */
ev_timer *ev_timer_at(ev_loop *loop, double when, ev_timer_cb cb, void *ud);
ev_timer *ev_timer_after(ev_loop *loop, double delay, ev_timer_cb cb, void *ud);
void      ev_timer_cancel(ev_loop *loop, ev_timer *t);

void ev_run(ev_loop *loop);
void ev_stop(ev_loop *loop);

#endif
