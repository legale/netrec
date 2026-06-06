#ifndef NETREC_YAML_H
#define NETREC_YAML_H

#include "state.h"

int yaml_load_desired(const char *path, struct desired_state *ds);
int yaml_load_desired_set(const char *path, struct desired_set *set);

#endif
