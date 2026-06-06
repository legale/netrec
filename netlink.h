#ifndef NETREC_NETLINK_H
#define NETREC_NETLINK_H

#include "state.h"

int nl_load_state(struct real_state *rs);
const char *nl_errstr(int rc);

#endif
