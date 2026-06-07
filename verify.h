#ifndef NETREC_VERIFY_H
#define NETREC_VERIFY_H

#include "state.h"

int verify_state(const struct desired_state *ds, const struct real_state *rs, int apply, int *act_fail);

#endif
