#ifndef NETREC_WATCH_H
#define NETREC_WATCH_H

#include "run.h"

struct netrec_watch;

struct netrec_watch_opts {
  int debounce_ms;
  int manage_nlmon;
  int manage_signals;
  void (*run_done)(int rc, int ms, void *arg);
  void *run_done_arg;
};

struct netrec_watch *netrec_watch_open(const struct netrec_run_cfg *cfg,
                                       const struct netrec_watch_opts *opts);
void netrec_watch_close(struct netrec_watch *watch);
void netrec_watch_stop(struct netrec_watch *watch);
int netrec_watch_loop(struct netrec_watch *watch);
int netrec_watch(const struct netrec_run_cfg *cfg, int debounce_ms);

#endif
