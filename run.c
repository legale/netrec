#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

#include "log.h"
#include "netlink.h"
#include "run.h"
#include "state.h"
#include "uci.h"
#include "verify.h"
#include "yaml.h"

static int apply_lock(void) {
  int fd;

  fd = open("/tmp/netrec.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  if (fd < 0) {
    nr_err("apply: lock open failed: %s", strerror(errno));
    return -errno;
  }

  if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
    nr_err("apply: another netrec apply is running");
    close(fd);
    return -EAGAIN;
  }

  return fd;
}

static int load_real(struct real_state *rs) {
  int rc;

  rc = nl_load_state(rs);
  if (rc) {
    nr_err("netlink: failed: %s", nl_errstr(rc));
    rs_free(rs);
    return rc;
  }

  rc = rs_load_resolv_conf(rs, "/etc/resolv.conf");
  if (rc) {
    rs_free(rs);
    return rc;
  }

  return 0;
}

static int run_verify_set(const struct desired_set *set, int apply, int *act_fail) {
  struct real_state rs;
  int diff;
  int i;
  int rc;

  diff = 0;
  rc = load_real(&rs);
  if (rc)
    return rc;

  *act_fail = 0;
  for (i = 0; i < set->n_state; i++) {
    rc = verify_state(&set->state[i], &rs, apply, act_fail);
    if (rc < 0) {
      rs_free(&rs);
      return rc;
    }
    diff += rc;
  }

  rs_free(&rs);
  return diff;
}

static int load_desired_set(const struct netrec_run_cfg *cfg, struct desired_set *set) {
  if (!strcmp(cfg->source, "yaml")) {
    if (!cfg->cfg_path)
      return -EINVAL;
    return yaml_load_desired_set(cfg->cfg_path, set);
  }

  if (!strcmp(cfg->source, "uci")) {
    return uci_load_desired_set(cfg->uci_bin, cfg->uci_network,
                                cfg->uci_wireless, set);
  }

  nr_err("source: unsupported %s", cfg->source);
  return -EINVAL;
}

int netrec_run_once(const struct netrec_run_cfg *cfg) {
  struct desired_set set;
  int act_fail;
  int lock_fd;
  int diff;
  int rc;

  memset(&set, 0, sizeof(set));
  act_fail = 0;
  lock_fd = -1;

  rc = load_desired_set(cfg, &set);
  if (rc)
    return rc;

  if (cfg->apply) {
    lock_fd = apply_lock();
    if (lock_fd < 0)
      return lock_fd;
  }

  diff = run_verify_set(&set, cfg->apply, &act_fail);
  if (!cfg->apply) {
    if (lock_fd >= 0)
      close(lock_fd);
    return diff > 0 ? 1 : diff;
  }

  if (diff < 0 || act_fail) {
    if (lock_fd >= 0)
      close(lock_fd);
    return diff < 0 ? diff : 1;
  }
  if (!diff) {
    if (lock_fd >= 0)
      close(lock_fd);
    return 0;
  }

  printf("POST_VERIFY\n");
  diff = run_verify_set(&set, 0, &act_fail);
  if (lock_fd >= 0)
    close(lock_fd);
  if (diff < 0)
    return diff;
  if (diff || act_fail)
    return 1;

  return 0;
}
