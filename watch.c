#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../nlmon/nlmon.h"
#include "log.h"
#include "watch.h"

#define NETREC_WATCH_DEBOUNCE_MS 500

struct netrec_watch {
  struct netrec_run_cfg run;
  int wake_rfd;
  int wake_wfd;
  int debounce_ms;
  int manage_nlmon;
  int manage_signals;
  int sig_installed;
  int nlmon_started;
  volatile sig_atomic_t stop;
  nlmon_sub_t *sub;
  struct sigaction old_int;
  struct sigaction old_term;
  void (*run_done)(int rc, int ms, void *arg);
  void *run_done_arg;
};

static struct netrec_watch *g_sig_watch;

static long watch_now_ms(void) {
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int fd_nonblock(int fd) {
  int flags;

  flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0)
    return -errno;
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    return -errno;

  return 0;
}

static void wake_watch(struct netrec_watch *watch) {
  char ch;

  ch = 'x';
  if (!watch || watch->wake_wfd < 0)
    return;
  if (write(watch->wake_wfd, &ch, 1) < 0 && errno != EAGAIN)
    nr_perror("watch wake");
}

static void watch_signal(int signo) {
  (void)signo;

  if (!g_sig_watch)
    return;
  g_sig_watch->stop = 1;
  wake_watch(g_sig_watch);
}

static void watch_nlmon_cb(const char *ifname, int ifidx, uint32_t events,
                           unsigned short nlmsg_type, uint32_t ifi_flags,
                           uint32_t ifi_change, void *arg) {
  struct netrec_watch *watch;

  (void)ifname;
  (void)ifidx;
  (void)events;
  (void)nlmsg_type;
  (void)ifi_flags;
  (void)ifi_change;

  watch = arg;
  wake_watch(watch);
}

static void watch_drain(int fd) {
  char buf[128];
  ssize_t rd;

  do {
    rd = read(fd, buf, sizeof(buf));
  } while (rd > 0);
  if (rd < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
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

static int watch_install_signals(struct netrec_watch *watch) {
  struct sigaction sa;

  memset(&sa, 0, sizeof(sa));
  memset(&watch->old_int, 0, sizeof(watch->old_int));
  memset(&watch->old_term, 0, sizeof(watch->old_term));
  sa.sa_handler = watch_signal;
  sigemptyset(&sa.sa_mask);

  if (sigaction(SIGINT, &sa, &watch->old_int) < 0) {
    nr_perror("sigaction SIGINT");
    return -errno;
  }
  if (sigaction(SIGTERM, &sa, &watch->old_term) < 0) {
    nr_perror("sigaction SIGTERM");
    sigaction(SIGINT, &watch->old_int, NULL);
    return -errno;
  }

  g_sig_watch = watch;
  watch->sig_installed = 1;
  return 0;
}

static void watch_uninstall_signals(struct netrec_watch *watch) {
  if (!watch || !watch->sig_installed)
    return;

  if (g_sig_watch == watch)
    g_sig_watch = NULL;
  sigaction(SIGTERM, &watch->old_term, NULL);
  sigaction(SIGINT, &watch->old_int, NULL);
  watch->sig_installed = 0;
}

static void watch_cleanup_runtime(struct netrec_watch *watch) {
  if (!watch)
    return;

  if (watch->sub) {
    (void)nlmon_unsubscribe(watch->sub);
    watch->sub = NULL;
  }
  if (watch->nlmon_started) {
    nlmon_stop();
    watch->nlmon_started = 0;
  }
  watch_uninstall_signals(watch);
}

struct netrec_watch *netrec_watch_open(const struct netrec_run_cfg *cfg,
                                       const struct netrec_watch_opts *opts) {
  struct netrec_watch *watch;
  int pipefd[2];
  int rc;

  if (!cfg)
    return NULL;

  watch = calloc(1, sizeof(*watch));
  if (!watch)
    return NULL;

  watch->run = *cfg;
  watch->wake_rfd = -1;
  watch->wake_wfd = -1;
  watch->debounce_ms = NETREC_WATCH_DEBOUNCE_MS;
  watch->manage_nlmon = 1;
  watch->manage_signals = 1;
  if (opts) {
    if (opts->debounce_ms >= 0)
      watch->debounce_ms = opts->debounce_ms;
    watch->manage_nlmon = opts->manage_nlmon;
    watch->manage_signals = opts->manage_signals;
    watch->run_done = opts->run_done;
    watch->run_done_arg = opts->run_done_arg;
  }

  if (pipe(pipefd) < 0) {
    free(watch);
    return NULL;
  }

  watch->wake_rfd = pipefd[0];
  watch->wake_wfd = pipefd[1];
  rc = fd_nonblock(watch->wake_rfd);
  if (!rc)
    rc = fd_nonblock(watch->wake_wfd);
  if (rc) {
    nr_err("watch: nonblock failed rc=%d", rc);
    netrec_watch_close(watch);
    return NULL;
  }

  return watch;
}

void netrec_watch_close(struct netrec_watch *watch) {
  if (!watch)
    return;

  watch_cleanup_runtime(watch);
  if (watch->wake_rfd >= 0)
    close(watch->wake_rfd);
  if (watch->wake_wfd >= 0)
    close(watch->wake_wfd);
  free(watch);
}

void netrec_watch_stop(struct netrec_watch *watch) {
  if (!watch)
    return;

  watch->stop = 1;
  wake_watch(watch);
}

int netrec_watch_loop(struct netrec_watch *watch) {
  nlmon_filter_t filter;
  int rc;

  if (!watch)
    return 1;

  memset(&filter, 0, sizeof(filter));
  watch->stop = 0;

  if (watch->manage_signals) {
    rc = watch_install_signals(watch);
    if (rc)
      return 1;
  }

  filter.events = NLMON_EVENT_ALL;
  filter.cb = watch_nlmon_cb;
  filter.arg = watch;

  if (watch->manage_nlmon) {
    if (nlmon_run() != 0) {
      nr_err("watch: nlmon_run failed");
      watch_cleanup_runtime(watch);
      return 1;
    }
    watch->nlmon_started = 1;
  }

  watch->sub = nlmon_subscribe(&filter);
  if (!watch->sub) {
    nr_err("watch: nlmon_subscribe failed: %s", strerror(errno));
    watch_cleanup_runtime(watch);
    return 1;
  }

  nr_notice("watch: started debounce_ms=%d", watch->debounce_ms);
  wake_watch(watch);

  rc = 0;
  while (!watch->stop) {
    int wait_rc;
    int run_rc;
    long start_ms;
    long end_ms;

    wait_rc = watch_wait(watch->wake_rfd, -1);
    if (wait_rc < 0) {
      nr_err("watch: poll failed rc=%d", wait_rc);
      rc = 1;
      break;
    }
    if (!wait_rc)
      continue;

    watch_drain(watch->wake_rfd);
    if (watch->stop)
      break;

    for (;;) {
      wait_rc = watch_wait(watch->wake_rfd, watch->debounce_ms);
      if (wait_rc < 0) {
        nr_err("watch: debounce poll failed rc=%d", wait_rc);
        rc = 1;
        watch->stop = 1;
        break;
      }
      if (!wait_rc)
        break;
      watch_drain(watch->wake_rfd);
      if (watch->stop)
        break;
    }
    if (watch->stop)
      break;

    start_ms = watch_now_ms();
    run_rc = netrec_run_once(&watch->run);
    end_ms = watch_now_ms();
    if (watch->run_done)
      watch->run_done(run_rc, (int)(end_ms - start_ms), watch->run_done_arg);
    if (run_rc < 0)
      nr_err("watch: run failed rc=%d", run_rc);
  }

  nr_notice("watch: stopped rc=%d", rc);
  watch_cleanup_runtime(watch);
  return rc;
}

int netrec_watch(const struct netrec_run_cfg *cfg, int debounce_ms) {
  struct netrec_watch_opts opts;
  struct netrec_watch *watch;
  int rc;

  memset(&opts, 0, sizeof(opts));
  opts.debounce_ms = debounce_ms;
  opts.manage_nlmon = 1;
  opts.manage_signals = 1;

  watch = netrec_watch_open(cfg, &opts);
  if (!watch)
    return 1;

  rc = netrec_watch_loop(watch);
  netrec_watch_close(watch);
  return rc;
}
