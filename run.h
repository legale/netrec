#ifndef NETREC_RUN_H
#define NETREC_RUN_H

struct netrec_run_cfg {
  const char *cfg_path;
  const char *source;
  const char *uci_network;
  const char *uci_wireless;
  int apply;
};

int netrec_run_once(const struct netrec_run_cfg *cfg);

#endif
