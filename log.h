#ifndef NETREC_LOG_H
#define NETREC_LOG_H

#include <errno.h>
#include <string.h>
#include <syslog.h>

#include "../syslog2/syslog2.h"

#define nr_err(fmt, ...) syslog2(LOG_ERR, fmt, ##__VA_ARGS__)
#define nr_warn(fmt, ...) syslog2(LOG_WARNING, fmt, ##__VA_ARGS__)
#define nr_info(fmt, ...) syslog2(LOG_INFO, fmt, ##__VA_ARGS__)
#define nr_notice(fmt, ...) syslog2(LOG_NOTICE, fmt, ##__VA_ARGS__)

static inline void nr_perror(const char *ctx) {
  int err;

  err = errno;
  syslog2(LOG_ERR, "%s: %s", ctx, strerror(err));
}

#endif
