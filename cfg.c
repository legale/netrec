#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"

#define CFG_PATH_SEG_MAX 128
#define CFG_PATH_DEPTH_MAX 64

enum cfg_tok_type {
  CFG_TOK_KEY,
  CFG_TOK_INDEX,
  CFG_TOK_APPEND,
};

struct cfg_tok {
  enum cfg_tok_type type;
  char key[CFG_PATH_SEG_MAX];
  int idx;
};

static char *xstrdup(const char *s) {
  size_t len;
  char *copy;

  if (!s)
    return NULL;

  len = strlen(s) + 1;
  copy = malloc(len);
  if (!copy)
    return NULL;

  memcpy(copy, s, len);
  return copy;
}

static void node_init(struct cfg_node *node, enum cfg_type type, const char *name) {
  memset(node, 0, sizeof(*node));
  node->type = type;
  node->name = xstrdup(name);
}

static struct cfg_node *node_new(enum cfg_type type, const char *name) {
  struct cfg_node *node;

  node = malloc(sizeof(*node));
  if (!node)
    return NULL;

  memset(node, 0, sizeof(*node));
  node->type = type;
  node->name = xstrdup(name);
  if (name && !node->name) {
    free(node);
    return NULL;
  }

  return node;
}

static void node_free(struct cfg_node *node) {
  struct cfg_node *child;
  struct cfg_node *next;

  for (child = node->child; child; child = next) {
    next = child->next;
    node_free(child);
  }

  free(node->name);
  free(node->val);
  free(node);
}

static void child_add(struct cfg_node *parent, struct cfg_node *node) {
  node->parent = parent;
  if (!parent->child)
    parent->child = node;
  else
    parent->tail->next = node;
  parent->tail = node;
  parent->n_child++;
}

static struct cfg_node *obj_find(const struct cfg_node *obj, const char *name) {
  struct cfg_node *child;

  if (!obj || obj->type != CFG_OBJ)
    return NULL;

  for (child = obj->child; child; child = child->next) {
    if (child->name && !strcmp(child->name, name))
      return child;
  }

  return NULL;
}

static int arr_idx(const struct cfg_node *arr, int idx, int *real_idx) {
  if (!arr || arr->type != CFG_ARR)
    return -ENOTDIR;

  if (idx >= 0) {
    if (idx >= arr->n_child)
      return -ENOENT;
    *real_idx = idx;
    return 0;
  }

  idx = arr->n_child + idx;
  if (idx < 0)
    return -ENOENT;

  *real_idx = idx;
  return 0;
}

static struct cfg_node *arr_nth(const struct cfg_node *arr, int idx) {
  struct cfg_node *child;
  int i;

  for (child = arr->child, i = 0; child; child = child->next, i++) {
    if (i == idx)
      return child;
  }

  return NULL;
}

static int is_key_start(int c) {
  return isalpha((unsigned char)c) || c == '_';
}

static int is_key_char(int c) {
  return isalnum((unsigned char)c) || c == '_';
}

static int parse_key_tok(const char **src, struct cfg_tok *tok) {
  const char *p;
  size_t len;

  p = *src;
  if (!is_key_start(*p))
    return -EINVAL;

  memset(tok, 0, sizeof(*tok));
  tok->type = CFG_TOK_KEY;

  len = 0;
  while (is_key_char(*p)) {
    if (len + 1 >= sizeof(tok->key))
      return -E2BIG;
    tok->key[len++] = *p++;
  }
  tok->key[len] = '\0';
  *src = p;
  return 0;
}

static int parse_index_tok(const char **src, struct cfg_tok *tok) {
  const char *p;
  const char *digits;
  char *end;
  long idx;

  p = *src;
  if (*p != '[')
    return -EINVAL;

  p++;
  memset(tok, 0, sizeof(*tok));

  if (*p == ']') {
    tok->type = CFG_TOK_APPEND;
    *src = p + 1;
    return 0;
  }

  tok->type = CFG_TOK_INDEX;
  digits = p;
  if (*p == '-')
    p++;
  if (!isdigit((unsigned char)*p))
    return -EINVAL;
  if (*p == '0' && isdigit((unsigned char)p[1]))
    return -EINVAL;

  errno = 0;
  idx = strtol(digits, &end, 10);
  if (errno || end == digits || *end != ']' || idx < INT_MIN || idx > INT_MAX)
    return -EINVAL;

  tok->idx = (int)idx;
  *src = end + 1;
  return 0;
}

static int parse_path(const char *path, struct cfg_tok *tok, int max, int *nr) {
  const char *p;
  int n;
  int rc;

  *nr = 0;
  if (!path || !*path)
    return 0;

  p = path;
  n = 0;

  if (n >= max)
    return -E2BIG;
  rc = parse_key_tok(&p, &tok[n]);
  if (rc)
    return rc;
  n++;

  while (*p) {
    if (n >= max)
      return -E2BIG;
    if (*p == '.') {
      p++;
      rc = parse_key_tok(&p, &tok[n]);
    } else if (*p == '[') {
      rc = parse_index_tok(&p, &tok[n]);
    } else {
      return -EINVAL;
    }
    if (rc)
      return rc;
    n++;
  }

  *nr = n;
  return 0;
}

static int path_file_ok(const char *path) {
  struct cfg_tok tok[CFG_PATH_DEPTH_MAX];
  int i;
  int nr;
  int rc;

  rc = parse_path(path, tok, CFG_PATH_DEPTH_MAX, &nr);
  if (rc)
    return rc;

  for (i = 0; i < nr; i++) {
    if (tok[i].type == CFG_TOK_INDEX)
      return -EINVAL;
    if (tok[i].type == CFG_TOK_APPEND && i + 1 != nr)
      return -EINVAL;
  }

  return 0;
}

static int walk_get(struct cfg *cfg, const char *path, struct cfg_node **out) {
  struct cfg_tok tok[CFG_PATH_DEPTH_MAX];
  struct cfg_node *cur;
  int nr;
  int i;
  int rc;
  int idx;

  rc = parse_path(path, tok, CFG_PATH_DEPTH_MAX, &nr);
  if (rc)
    return rc;

  cur = &cfg->root;
  for (i = 0; i < nr; i++) {
    if (tok[i].type == CFG_TOK_APPEND)
      return -EINVAL;

    if (tok[i].type == CFG_TOK_KEY) {
      if (cur->type != CFG_OBJ)
        return -ENOTDIR;
      cur = obj_find(cur, tok[i].key);
      if (!cur)
        return -ENOENT;
      continue;
    }

    if (cur->type != CFG_ARR)
      return -ENOTDIR;

    rc = arr_idx(cur, tok[i].idx, &idx);
    if (rc)
      return rc;
    cur = arr_nth(cur, idx);
    if (!cur)
      return -ENOENT;
  }

  *out = cur;
  return 0;
}

static int set_str_plain(struct cfg_node *parent, const char *name, const char *val) {
  struct cfg_node *node;
  char *copy;

  node = obj_find(parent, name);
  if (!node) {
    node = node_new(CFG_STR, name);
    if (!node)
      return -ENOMEM;
    child_add(parent, node);
  } else if (node->type != CFG_STR) {
    return -EISDIR;
  }

  copy = xstrdup(val);
  if (!copy)
    return -ENOMEM;

  free(node->val);
  node->val = copy;
  return 0;
}

static int set_arr_append(struct cfg_node *arr, const char *val) {
  struct cfg_node *node;
  char *copy;

  if (!arr || arr->type != CFG_ARR)
    return -ENOTDIR;

  node = node_new(CFG_STR, NULL);
  if (!node)
    return -ENOMEM;
  copy = xstrdup(val);
  if (!copy) {
    node_free(node);
    return -ENOMEM;
  }
  node->val = copy;
  child_add(arr, node);
  return 0;
}

static int set_arr_idx(struct cfg_node *arr, int idx, const char *val) {
  struct cfg_node *node;
  char *copy;
  int real_idx;

  if (!arr || arr->type != CFG_ARR)
    return -ENOTDIR;
  if (idx >= 0 && idx == arr->n_child)
    return -ENOENT;
  if (arr_idx(arr, idx, &real_idx))
    return -ENOENT;

  node = arr_nth(arr, real_idx);
  if (!node || node->type != CFG_STR)
    return -ENOENT;
  copy = xstrdup(val);
  if (!copy)
    return -ENOMEM;

  free(node->val);
  node->val = copy;
  return 0;
}

static int del_arr(struct cfg_node *arr, int idx) {
  struct cfg_node *node;
  struct cfg_node *prev;
  int real_idx;
  int i;

  if (!arr || arr->type != CFG_ARR)
    return -ENOENT;

  if (arr_idx(arr, idx, &real_idx))
    return -ENOENT;

  prev = NULL;
  node = arr->child;
  for (i = 0; node; node = node->next, i++) {
    if (i == real_idx)
      break;
    prev = node;
  }

  if (!node)
    return -ENOENT;

  if (prev)
    prev->next = node->next;
  else
    arr->child = node->next;
  if (arr->tail == node)
    arr->tail = prev;
  arr->n_child--;
  node->next = NULL;
  node_free(node);
  return 0;
}

static int del_plain(struct cfg_node *parent, const char *name) {
  struct cfg_node *node;
  struct cfg_node *prev;

  prev = NULL;
  for (node = parent->child; node; node = node->next) {
    if (node->name && !strcmp(node->name, name))
      break;
    prev = node;
  }

  if (!node)
    return -ENOENT;

  if (prev)
    prev->next = node->next;
  else
    parent->child = node->next;
  if (parent->tail == node)
    parent->tail = prev;
  parent->n_child--;
  node->next = NULL;
  node_free(node);
  return 0;
}

static char *trim(char *s) {
  char *end;

  while (*s == ' ' || *s == '\t')
    s++;

  end = s + strlen(s);
  while (end > s && (end[-1] == ' ' || end[-1] == '\t'))
    *--end = '\0';

  return s;
}

static void trim_eol(char *s) {
  size_t len;

  len = strlen(s);
  while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r'))
    s[--len] = '\0';
}

static int parse_value(char *src, char **out) {
  char *dst;
  char *val;
  size_t len;

  src = trim(src);
  if (!src[0]) {
    *out = xstrdup("");
    return *out ? 0 : -ENOMEM;
  }

  if (src[0] != '"') {
    char *hash;

    hash = strchr(src, '#');
    if (hash)
      *hash = '\0';
    src = trim(src);
    *out = xstrdup(src);
    return *out ? 0 : -ENOMEM;
  }

  src++;
  len = strlen(src) + 1;
  dst = malloc(len);
  if (!dst)
    return -ENOMEM;

  val = dst;
  while (*src && *src != '"') {
    if (*src == '\\') {
      src++;
      if (!*src) {
        free(dst);
        return -EINVAL;
      }
      if (*src == 'n')
        *val++ = '\n';
      else if (*src == 't')
        *val++ = '\t';
      else
        *val++ = *src;
      src++;
      continue;
    }
    *val++ = *src++;
  }

  if (*src != '"') {
    free(dst);
    return -EINVAL;
  }

  *val = '\0';
  src++;
  src = trim(src);
  if (*src && *src != '#') {
    free(dst);
    return -EINVAL;
  }

  *out = dst;
  return 0;
}

static int load_line(struct cfg *cfg, char *line) {
  char *eq;
  char *key;
  char *val;
  int rc;

  key = trim(line);
  if (!key[0] || key[0] == '#')
    return 0;

  eq = strchr(key, '=');
  if (!eq)
    return -EINVAL;

  *eq++ = '\0';
  key = trim(key);
  eq = trim(eq);
  if (!key[0])
    return -EINVAL;

  rc = path_file_ok(key);
  if (rc)
    return rc;

  rc = parse_value(eq, &val);
  if (rc)
    return rc;

  rc = cfg_set_str(cfg, key, val);
  free(val);
  return rc;
}

static int quote(FILE *f, const char *s) {
  const unsigned char *p;

  if (fputc('"', f) == EOF)
    return -EIO;

  for (p = (const unsigned char *)s; *p; p++) {
    if (*p == '\\' || *p == '"') {
      if (fputc('\\', f) == EOF || fputc(*p, f) == EOF)
        return -EIO;
      continue;
    }
    if (*p == '\n') {
      if (fputs("\\n", f) == EOF)
        return -EIO;
      continue;
    }
    if (*p == '\t') {
      if (fputs("\\t", f) == EOF)
        return -EIO;
      continue;
    }
    if (fputc(*p, f) == EOF)
      return -EIO;
  }

  if (fputc('"', f) == EOF)
    return -EIO;

  return 0;
}

static int dump_node(FILE *f, const struct cfg_node *node, char *path, size_t sz) {
  const struct cfg_node *child;
  size_t len;
  int rc;

  if (node->type == CFG_STR) {
    if (fprintf(f, "%s = ", path) < 0)
      return -EIO;
    rc = quote(f, node->val ? node->val : "");
    if (rc)
      return rc;
    if (fputc('\n', f) == EOF)
      return -EIO;
    return 0;
  }

  if (node->type == CFG_ARR) {
    for (child = node->child; child; child = child->next) {
      if (child->type != CFG_STR)
        return -EOPNOTSUPP;
      if (fprintf(f, "%s[] = ", path) < 0)
        return -EIO;
      rc = quote(f, child->val ? child->val : "");
      if (rc)
        return rc;
      if (fputc('\n', f) == EOF)
        return -EIO;
    }
    return 0;
  }

  for (child = node->child; child; child = child->next) {
    len = strlen(path);
    if (len) {
      if (snprintf(path + len, sz - len, ".%s", child->name) >= (int)(sz - len))
        return -E2BIG;
    } else {
      if (snprintf(path, sz, "%s", child->name) >= (int)sz)
        return -E2BIG;
    }

    rc = dump_node(f, child, path, sz);
    if (rc)
      return rc;

    path[len] = '\0';
  }

  return 0;
}

void cfg_init(struct cfg *cfg) {
  memset(cfg, 0, sizeof(*cfg));
  node_init(&cfg->root, CFG_OBJ, NULL);
}

void cfg_free(struct cfg *cfg) {
  struct cfg_node *child;
  struct cfg_node *next;

  for (child = cfg->root.child; child; child = next) {
    next = child->next;
    child->next = NULL;
    node_free(child);
  }

  free(cfg->root.name);
  free(cfg->root.val);
  memset(cfg, 0, sizeof(*cfg));
}

int cfg_load(struct cfg *cfg, const char *path) {
  char line[4096];
  FILE *f;
  int rc;
  int lineno;

  cfg_init(cfg);

  f = fopen(path, "rb");
  if (!f) {
    perror(path);
    cfg_free(cfg);
    return -errno;
  }

  lineno = 0;
  while (fgets(line, sizeof(line), f)) {
    lineno++;
    if (!strchr(line, '\n') && !feof(f)) {
      fprintf(stderr, "cfg: line too long in %s:%d\n", path, lineno);
      fclose(f);
      cfg_free(cfg);
      return -E2BIG;
    }
    trim_eol(line);
    rc = load_line(cfg, line);
    if (rc) {
      fprintf(stderr, "cfg: parse failed in %s:%d\n", path, lineno);
      fclose(f);
      cfg_free(cfg);
      return rc;
    }
  }

  fclose(f);
  return 0;
}

int cfg_dump(const struct cfg *cfg, const char *path) {
  FILE *f;
  int rc;

  f = fopen(path, "wb");
  if (!f) {
    perror(path);
    return -errno;
  }

  rc = cfg_dump_file(cfg, f);
  if (!rc && fflush(f) < 0)
    rc = -errno;
  if (fclose(f) < 0 && !rc)
    rc = -errno;
  if (rc)
    remove(path);

  return rc;
}

int cfg_dump_file(const struct cfg *cfg, FILE *f) {
  char path[512];

  path[0] = '\0';
  return dump_node(f, &cfg->root, path, sizeof(path));
}

int cfg_get_node(struct cfg *cfg, const char *path, struct cfg_node **out) {
  if (!path || !path[0]) {
    *out = &cfg->root;
    return 0;
  }

  return walk_get(cfg, path, out);
}

int cfg_get_str(struct cfg *cfg, const char *path, const char **out) {
  struct cfg_node *node;
  int rc;

  rc = cfg_get_node(cfg, path, &node);
  if (rc)
    return rc;
  if (node->type != CFG_STR)
    return -EISDIR;

  *out = node->val ? node->val : "";
  return 0;
}

int cfg_arr_len(struct cfg *cfg, const char *path) {
  struct cfg_node *node;
  int rc;

  rc = cfg_get_node(cfg, path, &node);
  if (rc)
    return rc;
  if (node->type != CFG_ARR)
    return -ENOTDIR;

  return node->n_child;
}

int cfg_set_str(struct cfg *cfg, const char *path, const char *val) {
  struct cfg_tok tok[CFG_PATH_DEPTH_MAX];
  struct cfg_node *cur;
  struct cfg_node *child;
  enum cfg_type want;
  int nr;
  int i;
  int rc;

  rc = parse_path(path, tok, CFG_PATH_DEPTH_MAX, &nr);
  if (rc)
    return rc;
  if (!nr)
    return -EINVAL;

  cur = &cfg->root;
  for (i = 0; i + 1 < nr; i++) {
    if (tok[i].type == CFG_TOK_APPEND)
      return -EINVAL;
    if (tok[i].type == CFG_TOK_INDEX) {
      int idx;

      if (cur->type != CFG_ARR)
        return -ENOTDIR;
      rc = arr_idx(cur, tok[i].idx, &idx);
      if (rc)
        return rc;
      cur = arr_nth(cur, idx);
      if (!cur)
        return -ENOENT;
      continue;
    }

    if (cur->type != CFG_OBJ)
      return -ENOTDIR;

    want = tok[i + 1].type == CFG_TOK_KEY ? CFG_OBJ : CFG_ARR;
    child = obj_find(cur, tok[i].key);
    if (!child) {
      child = node_new(want, tok[i].key);
      if (!child)
        return -ENOMEM;
      child_add(cur, child);
    } else if (child->type != want) {
      return -ENOTDIR;
    }

    cur = child;
  }

  if (tok[nr - 1].type == CFG_TOK_KEY) {
    if (cur->type != CFG_OBJ)
      return -ENOTDIR;
    return set_str_plain(cur, tok[nr - 1].key, val);
  }
  if (tok[nr - 1].type == CFG_TOK_APPEND)
    return set_arr_append(cur, val);

  return set_arr_idx(cur, tok[nr - 1].idx, val);
}

int cfg_del(struct cfg *cfg, const char *path) {
  struct cfg_tok tok[CFG_PATH_DEPTH_MAX];
  struct cfg_node *cur;
  struct cfg_node *child;
  int nr;
  int i;
  int rc;

  rc = parse_path(path, tok, CFG_PATH_DEPTH_MAX, &nr);
  if (rc)
    return rc;
  if (!nr)
    return -EINVAL;

  cur = &cfg->root;
  for (i = 0; i + 1 < nr; i++) {
    if (tok[i].type == CFG_TOK_APPEND)
      return -EINVAL;
    if (tok[i].type == CFG_TOK_INDEX) {
      int idx;

      if (cur->type != CFG_ARR)
        return -ENOTDIR;
      rc = arr_idx(cur, tok[i].idx, &idx);
      if (rc)
        return rc;
      cur = arr_nth(cur, idx);
      if (!cur)
        return -ENOENT;
      continue;
    }

    if (cur->type != CFG_OBJ)
      return -ENOTDIR;
    child = obj_find(cur, tok[i].key);
    if (!child)
      return -ENOENT;
    cur = child;
  }

  if (tok[nr - 1].type == CFG_TOK_APPEND)
    return -EINVAL;
  if (tok[nr - 1].type == CFG_TOK_INDEX)
    return del_arr(cur, tok[nr - 1].idx);
  if (cur->type != CFG_OBJ)
    return -ENOTDIR;

  return del_plain(cur, tok[nr - 1].key);
}

enum cfg_type cfg_type(const struct cfg_node *node) {
  return node->type;
}

struct cfg_node *cfg_child(const struct cfg_node *node, const char *name) {
  return obj_find(node, name);
}

struct cfg_node *cfg_first(const struct cfg_node *node) {
  return node ? node->child : NULL;
}

struct cfg_node *cfg_next(const struct cfg_node *node) {
  return node ? node->next : NULL;
}

const char *cfg_name(const struct cfg_node *node) {
  return node ? node->name : NULL;
}

const char *cfg_str(const struct cfg_node *node) {
  return node ? node->val : NULL;
}

int cfg_child_count(const struct cfg_node *node) {
  return node ? node->n_child : 0;
}
