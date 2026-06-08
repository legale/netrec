#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "yaml.h"

static void cpy(char *dst, size_t sz, const char *src) {
  if (!sz)
    return;

  if (!src) {
    dst[0] = '\0';
    return;
  }

  snprintf(dst, sz, "%s", src);
}

static int parse_u32_str(const char *s, uint32_t *dst) {
  char *end;
  unsigned long v;

  if (!s || !s[0]) {
    fprintf(stderr, "cfg: missing integer value\n");
    return -EINVAL;
  }

  errno = 0;
  v = strtoul(s, &end, 0);
  if (errno || *end || v > UINT32_MAX) {
    fprintf(stderr, "cfg: bad integer %s\n", s);
    return -EINVAL;
  }

  *dst = (uint32_t)v;
  return 0;
}

static int is_scn(const struct desired_state *ds, const char *a, const char *b) {
  if (!strcmp(ds->scenario, a))
    return 1;
  if (b && !strcmp(ds->scenario, b))
    return 1;

  return 0;
}

static int get_str(struct cfg_node *obj, const char *key, char *dst, size_t sz, int req) {
  struct cfg_node *node;

  node = cfg_child(obj, key);
  if (!node) {
    if (!req) {
      dst[0] = '\0';
      return 0;
    }
    fprintf(stderr, "cfg: missing %s\n", key);
    return -EINVAL;
  }

  if (cfg_type(node) != CFG_STR) {
    fprintf(stderr, "cfg: %s must be string\n", key);
    return -EINVAL;
  }

  cpy(dst, sz, cfg_str(node));
  return 0;
}

static int get_u32(struct cfg_node *obj, const char *key, uint32_t *dst) {
  struct cfg_node *node;

  node = cfg_child(obj, key);
  if (!node || cfg_type(node) != CFG_STR) {
    fprintf(stderr, "cfg: missing %s\n", key);
    return -EINVAL;
  }

  return parse_u32_str(cfg_str(node), dst);
}

static int get_str_arr(struct cfg_node *obj, const char *key, char dst[][IFNAMSIZ], int *nr, int max) {
  struct cfg_node *arr;
  struct cfg_node *it;
  int n;

  *nr = 0;

  arr = cfg_child(obj, key);
  if (!arr)
    return 0;

  if (cfg_type(arr) != CFG_ARR) {
    fprintf(stderr, "cfg: %s must be array\n", key);
    return -EINVAL;
  }

  n = 0;
  for (it = cfg_first(arr); it; it = cfg_next(it)) {
    if (cfg_type(it) != CFG_STR) {
      fprintf(stderr, "cfg: %s items must be string\n", key);
      return -EINVAL;
    }
    if (n >= max) {
      fprintf(stderr, "cfg: too many %s, max=%d\n", key, max);
      return -E2BIG;
    }
    cpy(dst[n], IFNAMSIZ, cfg_str(it));
    n++;
  }

  *nr = n;
  return 0;
}

static int get_dns_arr(struct cfg_node *obj, const char *key, char dst[][INET_ADDRSTRLEN], int *nr, int max) {
  struct cfg_node *arr;
  struct cfg_node *it;
  int n;

  *nr = 0;

  arr = cfg_child(obj, key);
  if (!arr)
    return 0;

  if (cfg_type(arr) != CFG_ARR) {
    fprintf(stderr, "cfg: %s must be array\n", key);
    return -EINVAL;
  }

  n = 0;
  for (it = cfg_first(arr); it; it = cfg_next(it)) {
    if (cfg_type(it) != CFG_STR) {
      fprintf(stderr, "cfg: %s items must be string\n", key);
      return -EINVAL;
    }
    if (n >= max) {
      fprintf(stderr, "cfg: too many %s, max=%d\n", key, max);
      return -E2BIG;
    }
    cpy(dst[n], INET_ADDRSTRLEN, cfg_str(it));
    n++;
  }

  *nr = n;
  return 0;
}

static int load_routes(struct cfg_node *root, struct desired_state *ds) {
  struct cfg_node *routes;
  struct cfg_node *it;
  struct desired_route4 *rt;
  int n;
  int rc;

  ds->n_routes = 0;

  routes = cfg_child(root, "routes");
  if (!routes)
    return 0;

  if (cfg_type(routes) != CFG_OBJ) {
    fprintf(stderr, "cfg: routes must be object\n");
    return -EINVAL;
  }

  n = 0;
  for (it = cfg_first(routes); it; it = cfg_next(it)) {
    if (cfg_type(it) != CFG_OBJ) {
      fprintf(stderr, "cfg: routes.%s must be object\n", cfg_name(it));
      return -EINVAL;
    }
    if (n >= NR_DES_ROUTE4_MAX) {
      fprintf(stderr, "cfg: too many routes, max=%d\n", NR_DES_ROUTE4_MAX);
      return -E2BIG;
    }

    rt = &ds->routes[n];
    memset(rt, 0, sizeof(*rt));

    rc = get_str(it, "dst", rt->dst, sizeof(rt->dst), 1);
    if (rc)
      return rc;
    rc = get_str(it, "dev", rt->dev, sizeof(rt->dev), 1);
    if (rc)
      return rc;
    rc = get_str(it, "via", rt->via, sizeof(rt->via), 0);
    if (rc)
      return rc;
    rt->has_via = !!rt->via[0];
    n++;
  }

  ds->n_routes = n;
  return 0;
}

static int load_bridge(struct cfg_node *root, struct desired_state *ds) {
  struct cfg_node *br;
  char mode[16];
  int rc;

  br = cfg_child(root, "bridge");
  if (!br || cfg_type(br) != CFG_OBJ) {
    fprintf(stderr, "cfg: bridge required\n");
    return -EINVAL;
  }

  rc = get_str(br, "name", ds->br_name, sizeof(ds->br_name), 1);
  if (rc)
    return rc;
  rc = get_str(br, "addr", ds->br_addr, sizeof(ds->br_addr), 0);
  if (rc)
    return rc;
  rc = get_str(br, "gateway", ds->br_gateway, sizeof(ds->br_gateway), 0);
  if (rc)
    return rc;
  rc = get_dns_arr(br, "dns", ds->br_dns, &ds->n_br_dns, NR_DES_DNS4_MAX);
  if (rc)
    return rc;
  rc = get_str(br, "addr_mode", mode, sizeof(mode), 0);
  if (rc)
    return rc;
  rc = get_str(br, "dhcp_cmd", ds->br_dhcp_cmd, sizeof(ds->br_dhcp_cmd), 0);
  if (rc)
    return rc;

  if (!mode[0]) {
    ds->br_addr_mode = ds->br_addr[0] ? ADDR_STATIC : ADDR_NONE;
  } else if (!strcmp(mode, "static")) {
    ds->br_addr_mode = ADDR_STATIC;
    if (!ds->br_addr[0]) {
      fprintf(stderr, "cfg: bridge.addr required for static\n");
      return -EINVAL;
    }
  } else if (!strcmp(mode, "dhcp")) {
    ds->br_addr_mode = ADDR_DHCP;
    if (ds->br_addr[0]) {
      fprintf(stderr, "cfg: bridge.addr not allowed for dhcp\n");
      return -EINVAL;
    }
    if (!ds->br_dhcp_cmd[0]) {
      fprintf(stderr, "cfg: bridge.dhcp_cmd required for dhcp\n");
      return -EINVAL;
    }
  } else if (!strcmp(mode, "none")) {
    ds->br_addr_mode = ADDR_NONE;
    if (ds->br_addr[0]) {
      fprintf(stderr, "cfg: bridge.addr not allowed for none\n");
      return -EINVAL;
    }
  } else {
    fprintf(stderr, "cfg: bad bridge.addr_mode=%s\n", mode);
    return -EINVAL;
  }

  if (ds->br_gateway[0] && ds->br_addr_mode != ADDR_STATIC) {
    fprintf(stderr, "cfg: bridge.gateway requires static addr_mode\n");
    return -EINVAL;
  }
  if (ds->n_br_dns && ds->br_addr_mode != ADDR_STATIC) {
    fprintf(stderr, "cfg: bridge.dns requires static addr_mode\n");
    return -EINVAL;
  }

  return get_str_arr(br, "ports", ds->br_ports, &ds->n_br_ports, NR_BR_PORT_MAX);
}

static int load_uplink(struct cfg_node *root, struct desired_state *ds) {
  struct cfg_node *up;

  up = cfg_child(root, "uplink");
  if (!up || cfg_type(up) != CFG_OBJ) {
    fprintf(stderr, "cfg: uplink required\n");
    return -EINVAL;
  }

  return get_str(up, "ifname", ds->up_ifname, sizeof(ds->up_ifname), 1);
}

static int load_vlan(struct cfg_node *root, struct desired_state *ds) {
  struct cfg_node *vl;
  int rc;

  vl = cfg_child(root, "vlan");
  if (!vl || cfg_type(vl) != CFG_OBJ) {
    fprintf(stderr, "cfg: vlan required\n");
    return -EINVAL;
  }

  rc = get_str(vl, "ifname", ds->vlan_ifname, sizeof(ds->vlan_ifname), 1);
  if (rc)
    return rc;
  rc = get_u32(vl, "id", &ds->vlan_id);
  if (rc)
    return rc;
  rc = get_str(vl, "link", ds->vlan_link, sizeof(ds->vlan_link), 0);
  if (rc)
    return rc;
  rc = get_str(vl, "bridge", ds->vlan_bridge, sizeof(ds->vlan_bridge), 0);
  if (rc)
    return rc;

  if (!ds->vlan_link[0])
    cpy(ds->vlan_link, sizeof(ds->vlan_link), ds->up_ifname);
  if (!ds->vlan_bridge[0])
    cpy(ds->vlan_bridge, sizeof(ds->vlan_bridge), ds->br_name);

  if (strcmp(ds->vlan_link, ds->up_ifname)) {
    fprintf(stderr, "cfg: vlan.link must equal uplink.ifname\n");
    return -EINVAL;
  }
  if (strcmp(ds->vlan_bridge, ds->br_name)) {
    fprintf(stderr, "cfg: vlan.bridge must equal bridge.name\n");
    return -EINVAL;
  }

  return 0;
}

static int load_wg_vxlan(struct cfg_node *root, struct desired_state *ds) {
  struct cfg_node *wg;
  struct cfg_node *vx;
  int rc;

  wg = cfg_child(root, "wg");
  vx = cfg_child(root, "vxlan");
  if (!wg || !vx || cfg_type(wg) != CFG_OBJ || cfg_type(vx) != CFG_OBJ) {
    fprintf(stderr, "cfg: wg/vxlan required\n");
    return -EINVAL;
  }

  rc = get_str(wg, "ifname", ds->wg_ifname, sizeof(ds->wg_ifname), 1);
  if (rc)
    return rc;
  rc = get_str(wg, "peer_ip", ds->wg_peer_ip, sizeof(ds->wg_peer_ip), 1);
  if (rc)
    return rc;
  rc = get_str(wg, "route_dev", ds->wg_route_dev, sizeof(ds->wg_route_dev), 1);
  if (rc)
    return rc;

  rc = get_str(vx, "ifname", ds->vx_ifname, sizeof(ds->vx_ifname), 1);
  if (rc)
    return rc;
  rc = get_u32(vx, "vni", &ds->vx_vni);
  if (rc)
    return rc;
  rc = get_str(vx, "remote", ds->vx_remote, sizeof(ds->vx_remote), 1);
  if (rc)
    return rc;
  rc = get_str(vx, "dev", ds->vx_dev, sizeof(ds->vx_dev), 0);
  if (rc)
    return rc;
  rc = get_str(vx, "bridge", ds->vx_bridge, sizeof(ds->vx_bridge), 1);
  if (rc)
    return rc;

  if (ds->vx_dev[0] && strcmp(ds->vx_dev, ds->wg_ifname)) {
    fprintf(stderr, "cfg: vxlan.dev must equal wg.ifname\n");
    return -EINVAL;
  }
  if (strcmp(ds->vx_bridge, ds->br_name)) {
    fprintf(stderr, "cfg: vxlan.bridge must equal bridge.name\n");
    return -EINVAL;
  }

  return 0;
}

static int load_state(struct cfg_node *root, struct desired_state *ds) {
  struct cfg_node *up;
  int rc;

  memset(ds, 0, sizeof(*ds));

  rc = get_str(root, "scenario", ds->scenario, sizeof(ds->scenario), 1);
  if (rc)
    return rc;

  if (is_scn(ds, "only_uplink", "only_uplink_iface")) {
    rc = load_uplink(root, ds);
  } else if (is_scn(ds, "uplink_bridge", "uplink_in_bridge")) {
    rc = load_bridge(root, ds);
    if (!rc)
      rc = load_uplink(root, ds);
  } else if (is_scn(ds, "vlan_bridge", "vlan_in_bridge")) {
    rc = load_bridge(root, ds);
    if (!rc)
      rc = load_uplink(root, ds);
    if (!rc)
      rc = load_vlan(root, ds);
  } else if (!strcmp(ds->scenario, "wg_vxlan_bridge")) {
    rc = load_bridge(root, ds);
    up = cfg_child(root, "uplink");
    if (!rc && up)
      rc = load_uplink(root, ds);
    if (!rc)
      rc = load_wg_vxlan(root, ds);
  } else {
    fprintf(stderr, "cfg: unsupported scenario=%s\n", ds->scenario);
    return -EINVAL;
  }

  if (rc)
    return rc;

  return load_routes(root, ds);
}

int yaml_load_desired(const char *path, struct desired_state *ds) {
  struct desired_set set;
  int rc;

  rc = yaml_load_desired_set(path, &set);
  if (rc)
    return rc;
  if (set.n_state != 1) {
    fprintf(stderr, "cfg: %s contains %d desired states, expected 1\n", path, set.n_state);
    return -EINVAL;
  }

  *ds = set.state[0];
  return 0;
}

int yaml_load_desired_set(const char *path, struct desired_set *set) {
  struct cfg cfg;
  struct cfg_node *root;
  struct cfg_node *states;
  struct cfg_node *it;
  int rc;
  int n;

  memset(set, 0, sizeof(*set));
  cfg_init(&cfg);

  rc = cfg_load(&cfg, path);
  if (rc)
    return rc;

  root = &cfg.root;
  if (cfg_child(root, "scenario")) {
    set->n_state = 1;
    rc = load_state(root, &set->state[0]);
    if (rc)
      set->n_state = 0;
    cfg_free(&cfg);
    return rc;
  }

  states = cfg_child(root, "state");
  if (!states || cfg_type(states) != CFG_OBJ) {
    fprintf(stderr, "cfg: root must contain scenario or state.<id>\n");
    cfg_free(&cfg);
    return -EINVAL;
  }

  n = 0;
  for (it = cfg_first(states); it; it = cfg_next(it)) {
    if (cfg_type(it) != CFG_OBJ) {
      fprintf(stderr, "cfg: state.%s must be object\n", cfg_name(it));
      cfg_free(&cfg);
      return -EINVAL;
    }
    if (n >= NR_DES_STATE_MAX) {
      fprintf(stderr, "cfg: too many desired states, max=%d\n", NR_DES_STATE_MAX);
      cfg_free(&cfg);
      return -E2BIG;
    }
    rc = load_state(it, &set->state[n]);
    if (rc) {
      cfg_free(&cfg);
      return rc;
    }
    n++;
  }

  if (!n) {
    fprintf(stderr, "cfg: state is empty\n");
    cfg_free(&cfg);
    return -EINVAL;
  }

  set->n_state = n;
  cfg_free(&cfg);
  return 0;
}
