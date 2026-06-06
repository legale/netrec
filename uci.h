#ifndef NETREC_UCI_H
#define NETREC_UCI_H

#include "state.h"

int uci_load_desired_set(const char *network_path, const char *wireless_path,
                         struct desired_set *set);

#endif
