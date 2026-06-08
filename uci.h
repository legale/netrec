#ifndef NETREC_UCI_H
#define NETREC_UCI_H

#include "state.h"

int uci2yml(const char *network_path, const char *wireless_path, const char *yaml_path);
int uci_load_desired_set(const char *uci_bin, const char *network_path,
                         const char *wireless_path, struct desired_set *set);

#endif
