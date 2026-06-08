#ifndef NETREC_CFG_H
#define NETREC_CFG_H

#include <stdio.h>

enum cfg_type {
  CFG_OBJ,
  CFG_ARR,
  CFG_STR,
};

struct cfg_node {
  enum cfg_type type;
  char *name;
  char *val;
  struct cfg_node *parent;
  struct cfg_node *next;
  struct cfg_node *child;
  struct cfg_node *tail;
  int n_child;
};

struct cfg {
  struct cfg_node root;
};

void cfg_init(struct cfg *cfg);
void cfg_free(struct cfg *cfg);
int cfg_load(struct cfg *cfg, const char *path);
int cfg_dump(const struct cfg *cfg, const char *path);
int cfg_dump_file(const struct cfg *cfg, FILE *f);
int cfg_get_node(struct cfg *cfg, const char *path, struct cfg_node **out);
int cfg_get_str(struct cfg *cfg, const char *path, const char **out);
int cfg_arr_len(struct cfg *cfg, const char *path);
int cfg_set_str(struct cfg *cfg, const char *path, const char *val);
int cfg_del(struct cfg *cfg, const char *path);

enum cfg_type cfg_type(const struct cfg_node *node);
struct cfg_node *cfg_child(const struct cfg_node *node, const char *name);
struct cfg_node *cfg_first(const struct cfg_node *node);
struct cfg_node *cfg_next(const struct cfg_node *node);
const char *cfg_name(const struct cfg_node *node);
const char *cfg_str(const struct cfg_node *node);
int cfg_child_count(const struct cfg_node *node);

#endif
