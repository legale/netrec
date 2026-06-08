#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cfg.h"

static int expect_rc(const char *tag, int got, int want) {
  if (got == want)
    return 0;

  fprintf(stderr, "FAIL %s rc=%d want=%d\n", tag, got, want);
  return 1;
}

static int expect_str(const char *tag, const char *got, const char *want) {
  if (!strcmp(got, want))
    return 0;

  fprintf(stderr, "FAIL %s got=%s want=%s\n", tag, got, want);
  return 1;
}

static int write_file(const char *path, const char *text) {
  FILE *f;

  f = fopen(path, "wb");
  if (!f)
    return -errno;
  if (fputs(text, f) == EOF) {
    fclose(f);
    return -EIO;
  }
  if (fclose(f) < 0)
    return -errno;

  return 0;
}

int main(void) {
  struct cfg cfg;
  struct cfg cfg2;
  struct cfg_node *node;
  const char *val;
  char tmp[] = "/tmp/netrec-cfg-XXXXXX";
  int fd;
  int rc;

  cfg_init(&cfg);
  cfg_init(&cfg2);

  if (expect_rc("set name", cfg_set_str(&cfg, "bridge.name", "br-lan"), 0))
    return 1;
  if (expect_rc("append 0", cfg_set_str(&cfg, "bridge.ports[]", "lan1"), 0))
    return 1;
  if (expect_rc("append 1", cfg_set_str(&cfg, "bridge.ports[]", "lan2"), 0))
    return 1;
  if (expect_rc("append 2", cfg_set_str(&cfg, "bridge.ports[]", "lan3"), 0))
    return 1;

  rc = cfg_arr_len(&cfg, "bridge.ports");
  if (expect_rc("arr len", rc, 3))
    return 1;

  if (expect_rc("get last", cfg_get_str(&cfg, "bridge.ports[-1]", &val), 0))
    return 1;
  if (expect_str("last val", val, "lan3"))
    return 1;

  if (expect_rc("get first neg", cfg_get_str(&cfg, "bridge.ports[-3]", &val), 0))
    return 1;
  if (expect_str("first neg val", val, "lan1"))
    return 1;

  if (expect_rc("neg oob", cfg_get_str(&cfg, "bridge.ports[-4]", &val), -ENOENT))
    return 1;
  if (expect_rc("set hole", cfg_set_str(&cfg, "bridge.ports[3]", "wan"), -ENOENT))
    return 1;
  if (expect_rc("get container as str", cfg_get_str(&cfg, "bridge.ports", &val), -EISDIR))
    return 1;

  if (expect_rc("get node arr", cfg_get_node(&cfg, "bridge.ports", &node), 0))
    return 1;
  if (expect_rc("node type arr", cfg_type(node), CFG_ARR))
    return 1;

  if (expect_rc("set last", cfg_set_str(&cfg, "bridge.ports[-1]", "wan"), 0))
    return 1;
  if (expect_rc("get last 2", cfg_get_str(&cfg, "bridge.ports[2]", &val), 0))
    return 1;
  if (expect_str("last replaced", val, "wan"))
    return 1;

  if (expect_rc("del middle", cfg_del(&cfg, "bridge.ports[-2]"), 0))
    return 1;
  rc = cfg_arr_len(&cfg, "bridge.ports");
  if (expect_rc("arr len 2", rc, 2))
    return 1;
  if (expect_rc("get new last", cfg_get_str(&cfg, "bridge.ports[-1]", &val), 0))
    return 1;
  if (expect_str("new last val", val, "wan"))
    return 1;

  fd = mkstemp(tmp);
  if (fd < 0)
    return 1;
  close(fd);

  if (expect_rc("dump file", cfg_dump(&cfg, tmp), 0))
    return 1;
  if (expect_rc("load file", cfg_load(&cfg2, tmp), 0))
    return 1;
  if (expect_rc("roundtrip val", cfg_get_str(&cfg2, "bridge.ports[-1]", &val), 0))
    return 1;
  if (expect_str("roundtrip last", val, "wan"))
    return 1;

  rc = write_file(tmp, "bridge.ports[0] = \"lan1\"\n");
  if (expect_rc("write bad file", rc, 0))
    return 1;
  if (expect_rc("indexed load reject", cfg_load(&cfg2, tmp), -EINVAL))
    return 1;

  unlink(tmp);
  cfg_free(&cfg2);
  cfg_free(&cfg);
  puts("OK cfg_path_check");
  return 0;
}
