#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "run.h"
#include "watch.h"

static void usage(const char *prog) {
  fprintf(stderr,
          "usage: %s [-a|--apply] [-w|--watch|--daemon] [--debounce-ms ms] "
          "[-c config.yaml] [--source yaml|uci] [--uci-bin path] "
          "[--uci-network path --uci-wireless path]\n"
          "       source=yaml requires -c\n"
          "       source=uci without paths runs uci show network/wireless\n",
          prog);
}

int main(int argc, char **argv) {
  struct netrec_run_cfg run;
  const char *cfg = NULL;
  const char *debounce_s = NULL;
  const char *source = "yaml";
  const char *uci_bin = "uci";
  const char *uci_network = NULL;
  const char *uci_wireless = NULL;
  int apply = 0;
  int debounce_ms = 0;
  int watch = 0;
  int i;
  int rc;

  setup_syslog2("netrec", LOG_NOTICE, true);

  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-a") || !strcmp(argv[i], "--apply")) {
      apply = 1;
      continue;
    }
    if (!strcmp(argv[i], "-w") || !strcmp(argv[i], "--watch") ||
        !strcmp(argv[i], "--daemon")) {
      watch = 1;
      continue;
    }

    if (!strcmp(argv[i], "-c") && i + 1 < argc) {
      cfg = argv[++i];
      continue;
    }
    if (!strcmp(argv[i], "--debounce-ms") && i + 1 < argc) {
      debounce_s = argv[++i];
      continue;
    }
    if (!strcmp(argv[i], "--source") && i + 1 < argc) {
      source = argv[++i];
      continue;
    }
    if (!strcmp(argv[i], "--uci-bin") && i + 1 < argc) {
      uci_bin = argv[++i];
      continue;
    }
    if (!strcmp(argv[i], "--uci-network") && i + 1 < argc) {
      uci_network = argv[++i];
      continue;
    }
    if (!strcmp(argv[i], "--uci-wireless") && i + 1 < argc) {
      uci_wireless = argv[++i];
      continue;
    }

    usage(argv[0]);
    return 2;
  }

  if (!strcmp(source, "yaml") && !cfg) {
    usage(argv[0]);
    return 2;
  }
  if (strcmp(source, "yaml") && strcmp(source, "uci")) {
    usage(argv[0]);
    return 2;
  }
  if (!strcmp(source, "uci") && (!!uci_network != !!uci_wireless)) {
    usage(argv[0]);
    return 2;
  }
  if (debounce_s) {
    char *end;

    rc = 0;
    debounce_ms = (int)strtol(debounce_s, &end, 10);
    if (!debounce_s[0] || *end || debounce_ms < 0) {
      usage(argv[0]);
      return 2;
    }
  }

  memset(&run, 0, sizeof(run));
  run.cfg_path = cfg;
  run.source = source;
  run.uci_bin = uci_bin;
  run.uci_network = uci_network;
  run.uci_wireless = uci_wireless;
  run.apply = apply;

  if (watch)
    return netrec_watch(&run, debounce_ms);

  rc = netrec_run_once(&run);
  return rc ? 1 : 0;
}
