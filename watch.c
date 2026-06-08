#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../nlmon/nlmon.h"
#include "log.h"
#include "watch.h"

#define NETREC_WATCH_DEBOUNCE_MS 500

struct watch_ctx {
  struct netrec_run_cfg run;
  int wake_rfd;
  int wake_wfd;
  int debounce_ms;
  volatile sig_atomic_t stop;
  atomic_int pending;
};

static struct watch_ctx *g_watch;

static int fd_nonblock(int fd) {
  int flags;

  flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0)
    return -errno;
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    return -errno;

  return 0;
}

static void wake_watch(struct watch_ctx *ctx) {
  char ch;

  ch = 'x';
  if (ctx->wake_wfd < 0)
    return;
  if (write(ctx->wake_wfd, &ch, 1) < 0 && errno != EAGAIN)
    nr_perror("watch wake");
}

static void watch_signal(int signo) {
  (void)signo;

  if (!g_watch)
    return;
  g_watch->stop = 1;
  wake_watch(g_watch);
}

static void watch_nlmon_cb(const char *ifname, int ifidx, uint32_t events,
                           unsigned short nlmsg_type, uint32_t ifi_flags,
                           uint32_t ifi_change, void *arg) {
  struct watch_ctx *ctx;

  (void)ifname;
  (void)ifidx;
  (void)events;
  (void)nlmsg_type;
  (void)ifi_flags;
  (void)ifi_change;

  ctx = arg;
  atomic_store(&ctx->pending, 1);
  wake_watch(ctx);
}

static void watch_drain(int fd) {
  char buf[128];

  while (read(fd, buf, sizeof(buf)) > 0)
    ;
  if (errno != EAGAIN && errno != EWOULDBLOCK)
    nr_perror("watch drain");
}

static int watch_wait(int fd, int timeout_ms) {
  struct pollfd pfd;
  int rc;

  memset(&pfd, 0, sizeof(pfd));
  pfd.fd = fd;
  pfd.events = POLLIN;

  for (;;) {
    rc = poll(&pfd, 1, timeout_ms);
    if (rc >= 0)
      return rc;
    if (errno != EINTR)
      return -errno;
  }
}

int netrec_watch(const struct netrec_run_cfg *cfg, int debounce_ms) {
  struct sigaction old_int;
  struct sigaction old_term;
  struct sigaction sa;
  struct watch_ctx ctx;
  nlmon_filter_t filter;
  nlmon_sub_t *sub;
  int pipefd[2];
  int rc;

  memset(&ctx, 0, sizeof(ctx));
  memset(&filter, 0, sizeof(filter));
  memset(&sa, 0, sizeof(sa));
  memset(&old_int, 0, sizeof(old_int));
  memset(&old_term, 0, sizeof(old_term));
  sub = NULL;

  ctx.run = *cfg;
  ctx.wake_rfd = -1;
  ctx.wake_wfd = -1;
  ctx.debounce_ms = debounce_ms > 0 ? debounce_ms : NETREC_WATCH_DEBOUNCE_MS;
  atomic_init(&ctx.pending, 0);

  if (pipe(pipefd) < 0) {
    nr_perror("watch pipe");
    return 1;
  }

  ctx.wake_rfd = pipefd[0];
  ctx.wake_wfd = pipefd[1];
  rc = fd_nonblock(ctx.wake_rfd);
  if (!rc)
    rc = fd_nonblock(ctx.wake_wfd);
  if (rc) {
    nr_err("watch: nonblock failed rc=%d", rc);
    close(ctx.wake_rfd);
    close(ctx.wake_wfd);
    return 1;
  }

  sa.sa_handler = watch_signal;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGINT, &sa, &old_int) < 0) {
    nr_perror("sigaction SIGINT");
    close(ctx.wake_rfd);
    close(ctx.wake_wfd);
    return 1;
  }
  if (sigaction(SIGTERM, &sa, &old_term) < 0) {
    nr_perror("sigaction SIGTERM");
    sigaction(SIGINT, &old_int, NULL);
    close(ctx.wake_rfd);
    close(ctx.wake_wfd);
    return 1;
  }

  g_watch = &ctx;

  filter.events = NLMON_EVENT_ALL;
  filter.cb = watch_nlmon_cb;
  filter.arg = &ctx;

  if (nlmon_run() != 0) {
    nr_err("watch: nlmon_run failed");
    rc = 1;
    goto out;
  }
  sub = nlmon_subscribe(&filter);
  if (!sub) {
    nr_err("watch: nlmon_subscribe failed: %s", strerror(errno));
    rc = 1;
    goto out_stop;
  }

  nr_notice("watch: started debounce_ms=%d", ctx.debounce_ms);
  atomic_store(&ctx.pending, 1);
  wake_watch(&ctx);

  rc = 0;
  while (!ctx.stop) {
    int wait_rc;
    int run_rc;

    wait_rc = watch_wait(ctx.wake_rfd, -1);
    if (wait_rc < 0) {
      nr_err("watch: poll failed rc=%d", wait_rc);
      rc = 1;
      break;
    }
    if (!wait_rc)
      continue;

    watch_drain(ctx.wake_rfd);
    if (ctx.stop)
      break;

    for (;;) {
      wait_rc = watch_wait(ctx.wake_rfd, ctx.debounce_ms);
      if (wait_rc < 0) {
        nr_err("watch: debounce poll failed rc=%d", wait_rc);
        rc = 1;
        ctx.stop = 1;
        break;
      }
      if (!wait_rc)
        break;
      watch_drain(ctx.wake_rfd);
      if (ctx.stop)
        break;
    }
    if (ctx.stop)
      break;

    atomic_store(&ctx.pending, 0);
    run_rc = netrec_run_once(&ctx.run);
    if (run_rc < 0)
      nr_err("watch: run failed rc=%d", run_rc);
  }

out_stop:
  if (sub)
    (void)nlmon_unsubscribe(sub);
  nlmon_stop();
out:
  nr_notice("watch: stopped rc=%d", rc);
  g_watch = NULL;
  sigaction(SIGTERM, &old_term, NULL);
  sigaction(SIGINT, &old_int, NULL);
  if (ctx.wake_rfd >= 0)
    close(ctx.wake_rfd);
  if (ctx.wake_wfd >= 0)
    close(ctx.wake_wfd);
  return rc;
}
