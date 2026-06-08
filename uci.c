#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "uci.h"
#include "yaml.h"

#define NR_UCI_ITEM_MAX 1024
#define UCI_PKG_SZ 16
#define UCI_SECT_SZ 64
#define UCI_OPT_SZ 64
#define UCI_VAL_SZ 256

struct uci_item {
  char pkg[UCI_PKG_SZ];
  char sect[UCI_SECT_SZ];
  char opt[UCI_OPT_SZ];
  char val[UCI_VAL_SZ];
  int is_type;
};

struct uci_db {
  struct uci_item item[NR_UCI_ITEM_MAX];
  int n_item;
};

static void cpy(char *dst, size_t sz, const char *src) {
  size_t len;

  if (!sz)
    return;

  if (!src) {
    dst[0] = '\0';
    return;
  }

  len = strlen(src);
  if (len >= sz)
    len = sz - 1;

  memcpy(dst, src, len);
  dst[len] = '\0';
}

static void trim_eol(char *s) {
  size_t len;

  len = strlen(s);
  while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r'))
    s[--len] = '\0';
}

static void strip_quotes(char *s) {
  size_t len;

  len = strlen(s);
  if (len >= 2 && s[0] == '\'' && s[len - 1] == '\'') {
    memmove(s, s + 1, len - 2);
    s[len - 2] = '\0';
  }
}

static int next_word(const char **src, char *dst, size_t sz) {
  const char *s;
  size_t len;

  s = *src;
  while (*s == ' ' || *s == '\t')
    s++;

  if (!*s) {
    *src = s;
    return 0;
  }

  len = 0;
  while (s[len] && s[len] != ' ' && s[len] != '\t')
    len++;

  if (len + 1 > sz)
    return -E2BIG;

  memcpy(dst, s, len);
  dst[len] = '\0';
  *src = s + len;
  return 1;
}

static int value_has_token(const char *val, const char *token) {
  const char *p;
  char word[UCI_VAL_SZ];
  int rc;

  if (!val || !token)
    return 0;

  p = val;
  for (;;) {
    rc = next_word(&p, word, sizeof(word));
    if (rc <= 0)
      return 0;
    if (!strcmp(word, token))
      return 1;
  }
}

static int db_add(struct uci_db *db, const char *pkg, const char *sect, const char *opt, const char *val, int is_type) {
  struct uci_item *it;

  if (db->n_item >= NR_UCI_ITEM_MAX) {
    fprintf(stderr, "uci: too many items, max=%d\n", NR_UCI_ITEM_MAX);
    return -E2BIG;
  }

  it = &db->item[db->n_item++];
  memset(it, 0, sizeof(*it));
  cpy(it->pkg, sizeof(it->pkg), pkg);
  cpy(it->sect, sizeof(it->sect), sect);
  cpy(it->opt, sizeof(it->opt), opt);
  cpy(it->val, sizeof(it->val), val);
  it->is_type = is_type;
  return 0;
}

static int db_load_file(struct uci_db *db, const char *path) {
  char line[1024];
  FILE *f;

  f = fopen(path, "rb");
  if (!f) {
    perror(path);
    return -errno;
  }

  while (fgets(line, sizeof(line), f)) {
    char *eq;
    char *dot1;
    char *dot2;
    char *val;
    char pkg[UCI_PKG_SZ];
    char sect[UCI_SECT_SZ];
    char opt[UCI_OPT_SZ];
    size_t len;
    int rc;

    if (!strchr(line, '\n') && !feof(f)) {
      fprintf(stderr, "uci: line too long in %s\n", path);
      fclose(f);
      return -E2BIG;
    }

    trim_eol(line);
    if (!line[0])
      continue;

    eq = strchr(line, '=');
    dot1 = strchr(line, '.');
    if (!eq || !dot1 || dot1 > eq) {
      fprintf(stderr, "uci: bad line in %s: %s\n", path, line);
      fclose(f);
      return -EINVAL;
    }

    memset(pkg, 0, sizeof(pkg));
    memset(sect, 0, sizeof(sect));
    memset(opt, 0, sizeof(opt));

    len = dot1 - line;
    if (len + 1 > sizeof(pkg)) {
      fprintf(stderr, "uci: package too long in %s: %s\n", path, line);
      fclose(f);
      return -E2BIG;
    }
    memcpy(pkg, line, len);
    pkg[len] = '\0';

    dot2 = memchr(dot1 + 1, '.', (size_t)(eq - dot1 - 1));
    if (dot2) {
      len = dot2 - (dot1 + 1);
      if (len + 1 > sizeof(sect)) {
        fprintf(stderr, "uci: section too long in %s: %s\n", path, line);
        fclose(f);
        return -E2BIG;
      }
      memcpy(sect, dot1 + 1, len);
      sect[len] = '\0';

      len = eq - (dot2 + 1);
      if (len + 1 > sizeof(opt)) {
        fprintf(stderr, "uci: option too long in %s: %s\n", path, line);
        fclose(f);
        return -E2BIG;
      }
      memcpy(opt, dot2 + 1, len);
      opt[len] = '\0';
    } else {
      len = eq - (dot1 + 1);
      if (len + 1 > sizeof(sect)) {
        fprintf(stderr, "uci: section too long in %s: %s\n", path, line);
        fclose(f);
        return -E2BIG;
      }
      memcpy(sect, dot1 + 1, len);
      sect[len] = '\0';
    }

    val = eq + 1;
    while (*val == ' ' || *val == '\t')
      val++;
    strip_quotes(val);

    rc = db_add(db, pkg, sect, opt, val, dot2 ? 0 : 1);
    if (rc) {
      fclose(f);
      return rc;
    }
  }

  fclose(f);
  return 0;
}

static const char *db_get(const struct uci_db *db, const char *pkg, const char *sect, const char *opt) {
  int i;

  for (i = db->n_item - 1; i >= 0; i--) {
    if (db->item[i].is_type)
      continue;
    if (strcmp(db->item[i].pkg, pkg))
      continue;
    if (strcmp(db->item[i].sect, sect))
      continue;
    if (strcmp(db->item[i].opt, opt))
      continue;
    return db->item[i].val;
  }

  return NULL;
}

static const char *find_iface_by_device(const struct uci_db *db, const char *br_name) {
  int i;

  for (i = 0; i < db->n_item; i++) {
    const char *dev;
    const char *proto;

    if (!db->item[i].is_type)
      continue;
    if (strcmp(db->item[i].pkg, "network"))
      continue;
    if (strcmp(db->item[i].val, "interface"))
      continue;

    dev = db_get(db, "network", db->item[i].sect, "device");
    if (!dev || strcmp(dev, br_name))
      continue;

    proto = db_get(db, "network", db->item[i].sect, "proto");
    if (!proto || !proto[0])
      continue;

    return db->item[i].sect;
  }

  return NULL;
}

static int add_bridge_port(struct desired_state *ds, const char *ifname) {
  int i;

  if (!ifname || !ifname[0])
    return 0;
  if (ds->up_ifname[0] && !strcmp(ds->up_ifname, ifname))
    return 0;
  if (ds->vx_ifname[0] && !strcmp(ds->vx_ifname, ifname))
    return 0;

  for (i = 0; i < ds->n_br_ports; i++) {
    if (!strcmp(ds->br_ports[i], ifname))
      return 0;
  }

  if (ds->n_br_ports >= NR_BR_PORT_MAX) {
    fprintf(stderr, "uci: too many bridge ports for %s, max=%d\n",
            ds->br_name, NR_BR_PORT_MAX);
    return -E2BIG;
  }

  cpy(ds->br_ports[ds->n_br_ports], sizeof(ds->br_ports[0]), ifname);
  ds->n_br_ports++;
  return 0;
}

static int add_wireless_ports(const struct uci_db *db, const char *net_name, struct desired_state *ds) {
  int i;

  for (i = 0; i < db->n_item; i++) {
    const char *disabled;
    const char *ifname;
    const char *network;
    int rc;

    if (!db->item[i].is_type)
      continue;
    if (strcmp(db->item[i].pkg, "wireless"))
      continue;
    if (strcmp(db->item[i].val, "wifi-iface"))
      continue;

    disabled = db_get(db, "wireless", db->item[i].sect, "disabled");
    if (disabled && strcmp(disabled, "0"))
      continue;

    network = db_get(db, "wireless", db->item[i].sect, "network");
    if (!network || !value_has_token(network, net_name))
      continue;

    ifname = db_get(db, "wireless", db->item[i].sect, "ifname");
    if (!ifname || !ifname[0])
      continue;

    rc = add_bridge_port(ds, ifname);
    if (rc)
      return rc;
  }

  return 0;
}

static int mask_to_prefix(const char *mask, int *prefix) {
  struct in_addr in;
  uint32_t v;
  int bits;
  int zero_seen;

  if (!mask || !prefix)
    return -EINVAL;
  if (inet_pton(AF_INET, mask, &in) != 1)
    return -EINVAL;

  v = ntohl(in.s_addr);
  bits = 0;
  zero_seen = 0;
  while (bits < 32) {
    if (v & (1U << (31 - bits))) {
      if (zero_seen)
        return -EINVAL;
    } else {
      zero_seen = 1;
    }
    bits++;
  }

  bits = 0;
  while (v & 0x80000000U) {
    bits++;
    v <<= 1;
  }

  *prefix = bits;
  return 0;
}

static int split_ip_list(const char *src, char dst[][INET_ADDRSTRLEN], int *nr, int max, const char *tag) {
  const char *p;
  char word[32];
  int n;
  int rc;

  *nr = 0;
  if (!src || !src[0])
    return 0;

  n = 0;
  p = src;
  for (;;) {
    rc = next_word(&p, word, sizeof(word));
    if (rc == 0)
      break;
    if (rc < 0) {
      fprintf(stderr, "uci: bad %s item length\n", tag);
      return rc;
    }
    if (n >= max) {
      fprintf(stderr, "uci: too many %s items, max=%d\n", tag, max);
      return -E2BIG;
    }
    cpy(dst[n], INET_ADDRSTRLEN, word);
    n++;
  }

  *nr = n;
  return 0;
}

static int parse_u32_str(const char *s, uint32_t *v) {
  char *end;
  unsigned long n;

  if (!s || !s[0])
    return -EINVAL;

  errno = 0;
  n = strtoul(s, &end, 0);
  if (errno || *end || n > UINT32_MAX)
    return -EINVAL;

  *v = (uint32_t)n;
  return 0;
}

static int add_state(struct desired_set *set, struct desired_state **ds) {
  if (set->n_state >= NR_DES_STATE_MAX) {
    fprintf(stderr, "uci: too many desired states, max=%d\n",
            NR_DES_STATE_MAX);
    return -E2BIG;
  }

  *ds = &set->state[set->n_state++];
  memset(*ds, 0, sizeof(**ds));
  return 0;
}

static int cfg_quote(FILE *f, const char *s) {
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

static int cfg_line_str(FILE *f, const char *path, const char *val) {
  int rc;

  if (fprintf(f, "%s = ", path) < 0)
    return -EIO;
  rc = cfg_quote(f, val);
  if (rc)
    return rc;
  if (fputc('\n', f) == EOF)
    return -EIO;

  return 0;
}

static int cfg_line_u32(FILE *f, const char *path, uint32_t val) {
  if (fprintf(f, "%s = \"%u\"\n", path, val) < 0)
    return -EIO;

  return 0;
}

static int cfg_path(char *dst, size_t sz, const char *id, const char *tail) {
  if (snprintf(dst, sz, "state.%s.%s", id, tail) >= (int)sz)
    return -E2BIG;

  return 0;
}

static int cfg_state_id(const struct desired_state *ds, char *dst, size_t sz, int idx) {
  const char *src;
  size_t n;

  src = ds->br_name[0] ? ds->br_name : ds->scenario;
  n = 0;

  if (src && src[0]) {
    for (; *src && n + 1 < sz; src++) {
      if (n == 0) {
        if ((*src >= 'A' && *src <= 'Z') ||
            (*src >= 'a' && *src <= 'z') || *src == '_') {
          dst[n++] = *src;
        } else if ((*src >= '0' && *src <= '9')) {
          dst[n++] = '_';
          if (n + 1 >= sz)
            return -E2BIG;
          dst[n++] = *src;
        } else {
          dst[n++] = '_';
        }
        continue;
      }

      if ((*src >= 'A' && *src <= 'Z') || (*src >= 'a' && *src <= 'z') ||
          (*src >= '0' && *src <= '9') || *src == '_') {
        dst[n++] = *src;
      } else {
        dst[n++] = '_';
      }
    }
    if (*src)
      return -E2BIG;
  }

  if (!n) {
    if (snprintf(dst, sz, "s%d", idx) >= (int)sz)
      return -E2BIG;
  } else {
    dst[n] = '\0';
  }

  return 0;
}

static int cfg_state(FILE *f, const struct desired_state *ds, int idx) {
  char id[IFNAMSIZ];
  char path[256];
  int i;
  int rc;

  rc = cfg_state_id(ds, id, sizeof(id), idx);
  if (rc)
    return rc;

  rc = cfg_path(path, sizeof(path), id, "scenario");
  if (rc)
    return rc;
  rc = cfg_line_str(f, path, ds->scenario);
  if (rc)
    return rc;

  if (ds->br_name[0]) {
    rc = cfg_path(path, sizeof(path), id, "bridge.name");
    if (rc)
      return rc;
    rc = cfg_line_str(f, path, ds->br_name);
    if (rc)
      return rc;

    if (ds->br_addr_mode == ADDR_STATIC) {
      rc = cfg_path(path, sizeof(path), id, "bridge.addr_mode");
      if (rc)
        return rc;
      rc = cfg_line_str(f, path, "static");
      if (rc)
        return rc;
      rc = cfg_path(path, sizeof(path), id, "bridge.addr");
      if (rc)
        return rc;
      rc = cfg_line_str(f, path, ds->br_addr);
      if (rc)
        return rc;
      if (ds->br_gateway[0]) {
        rc = cfg_path(path, sizeof(path), id, "bridge.gateway");
        if (rc)
          return rc;
        rc = cfg_line_str(f, path, ds->br_gateway);
        if (rc)
          return rc;
      }
      for (i = 0; i < ds->n_br_dns; i++) {
        rc = cfg_path(path, sizeof(path), id, "bridge.dns[]");
        if (rc)
          return rc;
        rc = cfg_line_str(f, path, ds->br_dns[i]);
        if (rc)
          return rc;
      }
    } else if (ds->br_addr_mode == ADDR_DHCP) {
      rc = cfg_path(path, sizeof(path), id, "bridge.addr_mode");
      if (rc)
        return rc;
      rc = cfg_line_str(f, path, "dhcp");
      if (rc)
        return rc;
      rc = cfg_path(path, sizeof(path), id, "bridge.dhcp_cmd");
      if (rc)
        return rc;
      rc = cfg_line_str(f, path, ds->br_dhcp_cmd);
      if (rc)
        return rc;
    } else {
      rc = cfg_path(path, sizeof(path), id, "bridge.addr_mode");
      if (rc)
        return rc;
      rc = cfg_line_str(f, path, "none");
      if (rc)
        return rc;
    }

    for (i = 0; i < ds->n_br_ports; i++) {
      rc = cfg_path(path, sizeof(path), id, "bridge.ports[]");
      if (rc)
        return rc;
      rc = cfg_line_str(f, path, ds->br_ports[i]);
      if (rc)
        return rc;
    }
  }

  if (ds->up_ifname[0]) {
    rc = cfg_path(path, sizeof(path), id, "uplink.ifname");
    if (rc)
      return rc;
    rc = cfg_line_str(f, path, ds->up_ifname);
    if (rc)
      return rc;
  }

  if (ds->vlan_ifname[0]) {
    rc = cfg_path(path, sizeof(path), id, "vlan.ifname");
    if (rc)
      return rc;
    rc = cfg_line_str(f, path, ds->vlan_ifname);
    if (rc)
      return rc;
    rc = cfg_path(path, sizeof(path), id, "vlan.id");
    if (rc)
      return rc;
    rc = cfg_line_u32(f, path, ds->vlan_id);
    if (rc)
      return rc;
    if (ds->vlan_link[0]) {
      rc = cfg_path(path, sizeof(path), id, "vlan.link");
      if (rc)
        return rc;
      rc = cfg_line_str(f, path, ds->vlan_link);
      if (rc)
        return rc;
    }
    if (ds->vlan_bridge[0]) {
      rc = cfg_path(path, sizeof(path), id, "vlan.bridge");
      if (rc)
        return rc;
      rc = cfg_line_str(f, path, ds->vlan_bridge);
      if (rc)
        return rc;
    }
  }

  if (ds->wg_ifname[0]) {
    rc = cfg_path(path, sizeof(path), id, "wg.ifname");
    if (rc)
      return rc;
    rc = cfg_line_str(f, path, ds->wg_ifname);
    if (rc)
      return rc;
    rc = cfg_path(path, sizeof(path), id, "wg.peer_ip");
    if (rc)
      return rc;
    rc = cfg_line_str(f, path, ds->wg_peer_ip);
    if (rc)
      return rc;
    rc = cfg_path(path, sizeof(path), id, "wg.route_dev");
    if (rc)
      return rc;
    rc = cfg_line_str(f, path, ds->wg_route_dev);
    if (rc)
      return rc;
  }

  if (ds->vx_ifname[0]) {
    rc = cfg_path(path, sizeof(path), id, "vxlan.ifname");
    if (rc)
      return rc;
    rc = cfg_line_str(f, path, ds->vx_ifname);
    if (rc)
      return rc;
    rc = cfg_path(path, sizeof(path), id, "vxlan.vni");
    if (rc)
      return rc;
    rc = cfg_line_u32(f, path, ds->vx_vni);
    if (rc)
      return rc;
    rc = cfg_path(path, sizeof(path), id, "vxlan.remote");
    if (rc)
      return rc;
    rc = cfg_line_str(f, path, ds->vx_remote);
    if (rc)
      return rc;
    if (ds->vx_dev[0]) {
      rc = cfg_path(path, sizeof(path), id, "vxlan.dev");
      if (rc)
        return rc;
      rc = cfg_line_str(f, path, ds->vx_dev);
      if (rc)
        return rc;
    }
    rc = cfg_path(path, sizeof(path), id, "vxlan.bridge");
    if (rc)
      return rc;
    rc = cfg_line_str(f, path, ds->vx_bridge);
    if (rc)
      return rc;
  }

  for (i = 0; i < ds->n_routes; i++) {
    char route_path[64];

    if (snprintf(route_path, sizeof(route_path), "routes.r%d.dst", i) >= (int)sizeof(route_path))
      return -E2BIG;
    rc = cfg_path(path, sizeof(path), id, route_path);
    if (rc)
      return rc;
    rc = cfg_line_str(f, path, ds->routes[i].dst);
    if (rc)
      return rc;

    if (snprintf(route_path, sizeof(route_path), "routes.r%d.dev", i) >= (int)sizeof(route_path))
      return -E2BIG;
    rc = cfg_path(path, sizeof(path), id, route_path);
    if (rc)
      return rc;
    rc = cfg_line_str(f, path, ds->routes[i].dev);
    if (rc)
      return rc;

    if (ds->routes[i].has_via) {
      if (snprintf(route_path, sizeof(route_path), "routes.r%d.via", i) >= (int)sizeof(route_path))
        return -E2BIG;
      rc = cfg_path(path, sizeof(path), id, route_path);
      if (rc)
        return rc;
      rc = cfg_line_str(f, path, ds->routes[i].via);
      if (rc)
        return rc;
    }
  }

  return ferror(f) ? -EIO : 0;
}

static int cfg_set(FILE *f, const struct desired_set *set) {
  int i;
  int rc;

  for (i = 0; i < set->n_state; i++) {
    rc = cfg_state(f, &set->state[i], i);
    if (rc)
      return rc;
  }

  return 0;
}

static int fill_bridge_proto(const struct uci_db *db, const char *if_sect, struct desired_state *ds) {
  const char *dns;
  const char *gateway;
  const char *ipaddr;
  const char *mask;
  const char *proto;
  int prefix;
  int rc;

  proto = db_get(db, "network", if_sect, "proto");
  if (!proto || !proto[0]) {
    fprintf(stderr, "uci: missing proto for network.%s\n", if_sect);
    return -EINVAL;
  }

  if (!strcmp(proto, "static")) {
    ipaddr = db_get(db, "network", if_sect, "ipaddr");
    mask = db_get(db, "network", if_sect, "netmask");
    if (!ipaddr || !mask) {
      fprintf(stderr, "uci: static bridge %s needs ipaddr+netmask\n",
              ds->br_name);
      return -EINVAL;
    }

    rc = mask_to_prefix(mask, &prefix);
    if (rc) {
      fprintf(stderr, "uci: bad netmask %s for bridge %s\n", mask,
              ds->br_name);
      return rc;
    }

    ds->br_addr_mode = ADDR_STATIC;
    snprintf(ds->br_addr, sizeof(ds->br_addr), "%s/%d", ipaddr, prefix);

    gateway = db_get(db, "network", if_sect, "gateway");
    if (gateway)
      cpy(ds->br_gateway, sizeof(ds->br_gateway), gateway);

    dns = db_get(db, "network", if_sect, "dns");
    rc = split_ip_list(dns, ds->br_dns, &ds->n_br_dns, NR_DES_DNS4_MAX,
                       "dns");
    if (rc)
      return rc;

    return 0;
  }

  if (!strcmp(proto, "dhcp")) {
    ds->br_addr_mode = ADDR_DHCP;
    cpy(ds->br_dhcp_cmd, sizeof(ds->br_dhcp_cmd),
        "udhcpc -i $iface -q -n");
    return 0;
  }

  if (!strcmp(proto, "none")) {
    ds->br_addr_mode = ADDR_NONE;
    return 0;
  }

  fprintf(stderr, "uci: unsupported proto=%s for bridge %s\n", proto,
          ds->br_name);
  return -EINVAL;
}

static int add_device_ports(const char *ports, struct desired_state *ds, int skip_first) {
  const char *p;
  char word[IFNAMSIZ];
  int first;
  int rc;

  if (!ports || !ports[0])
    return 0;

  first = 1;
  p = ports;
  for (;;) {
    rc = next_word(&p, word, sizeof(word));
    if (rc == 0)
      return 0;
    if (rc < 0) {
      fprintf(stderr, "uci: bridge port name too long for %s\n", ds->br_name);
      return rc;
    }
    if (skip_first && first) {
      first = 0;
      continue;
    }
    first = 0;
    rc = add_bridge_port(ds, word);
    if (rc)
      return rc;
  }
}

static int first_device_port(const char *ports, char *dst, size_t sz) {
  const char *p;
  int rc;

  dst[0] = '\0';
  if (!ports || !ports[0])
    return -ENOENT;

  p = ports;
  rc = next_word(&p, dst, sz);
  if (rc <= 0)
    return rc ? rc : -ENOENT;

  return 0;
}

static int find_wg_for_remote(const struct uci_db *db, const char *remote, char *wg_ifname, size_t sz) {
  char remote32[32];
  int found;
  int i;

  snprintf(remote32, sizeof(remote32), "%s/32", remote);

  for (i = 0; i < db->n_item; i++) {
    const char *allowed_ips;
    const char *type;

    if (!db->item[i].is_type)
      continue;
    if (strcmp(db->item[i].pkg, "network"))
      continue;

    type = db->item[i].val;
    if (strncmp(type, "wireguard_", 10))
      continue;

    allowed_ips = db_get(db, "network", db->item[i].sect, "allowed_ips");
    if (!allowed_ips)
      continue;
    if (!value_has_token(allowed_ips, remote32) &&
        !value_has_token(allowed_ips, remote))
      continue;

    cpy(wg_ifname, sz, type + 10);
    return 0;
  }

  found = 0;
  for (i = 0; i < db->n_item; i++) {
    const char *proto;

    if (!db->item[i].is_type)
      continue;
    if (strcmp(db->item[i].pkg, "network"))
      continue;
    if (strcmp(db->item[i].val, "interface"))
      continue;

    proto = db_get(db, "network", db->item[i].sect, "proto");
    if (!proto || strcmp(proto, "wireguard"))
      continue;

    cpy(wg_ifname, sz, db->item[i].sect);
    found++;
  }

  if (found == 1)
    return 0;

  fprintf(stderr, "uci: unable to resolve wireguard iface for remote %s\n",
          remote);
  return -EINVAL;
}

static int build_uplink_bridge(const struct uci_db *net,
                               const struct uci_db *wifi,
                               const char *dev_sect, const char *if_sect,
                               const char *br_name, struct desired_set *set) {
  struct desired_state *ds;
  const char *ports;
  int rc;

  rc = add_state(set, &ds);
  if (rc)
    return rc;

  cpy(ds->scenario, sizeof(ds->scenario), "uplink_bridge");
  cpy(ds->br_name, sizeof(ds->br_name), br_name);

  rc = fill_bridge_proto(net, if_sect, ds);
  if (rc)
    return rc;

  ports = db_get(net, "network", dev_sect, "ports");
  rc = first_device_port(ports, ds->up_ifname, sizeof(ds->up_ifname));
  if (rc) {
    fprintf(stderr, "uci: bridge %s needs at least one uplink port\n",
            br_name);
    return rc;
  }

  rc = add_device_ports(ports, ds, 1);
  if (rc)
    return rc;

  return add_wireless_ports(wifi, if_sect, ds);
}

static int build_wg_vxlan_bridge(const struct uci_db *net,
                                 const struct uci_db *wifi,
                                 const char *dev_sect, const char *if_sect,
                                 const char *br_name, struct desired_set *set) {
  struct desired_state *ds;
  const char *peeraddr;
  const char *ports;
  const char *proto;
  const char *vid;
  char wg_ifname[IFNAMSIZ];
  int rc;

  memset(wg_ifname, 0, sizeof(wg_ifname));

  ports = db_get(net, "network", dev_sect, "ports");

  rc = add_state(set, &ds);
  if (rc)
    return rc;

  cpy(ds->scenario, sizeof(ds->scenario), "wg_vxlan_bridge");
  cpy(ds->br_name, sizeof(ds->br_name), br_name);
  ds->br_addr_mode = ADDR_NONE;

  rc = first_device_port(ports, ds->vx_ifname, sizeof(ds->vx_ifname));
  if (rc) {
    fprintf(stderr, "uci: bridge %s needs vxlan port\n", br_name);
    return rc;
  }

  proto = db_get(net, "network", ds->vx_ifname, "proto");
  if (!proto || strcmp(proto, "vxlan")) {
    fprintf(stderr, "uci: network.%s must be proto=vxlan\n", ds->vx_ifname);
    return -EINVAL;
  }

  vid = db_get(net, "network", ds->vx_ifname, "vid");
  peeraddr = db_get(net, "network", ds->vx_ifname, "peeraddr");
  if (!vid || !peeraddr) {
    fprintf(stderr, "uci: network.%s needs vid+peeraddr\n", ds->vx_ifname);
    return -EINVAL;
  }

  rc = parse_u32_str(vid, &ds->vx_vni);
  if (rc) {
    fprintf(stderr, "uci: bad vid %s for %s\n", vid, ds->vx_ifname);
    return rc;
  }

  rc = find_wg_for_remote(net, peeraddr, wg_ifname, sizeof(wg_ifname));
  if (rc)
    return rc;

  cpy(ds->wg_ifname, sizeof(ds->wg_ifname), wg_ifname);
  cpy(ds->wg_peer_ip, sizeof(ds->wg_peer_ip), peeraddr);
  cpy(ds->wg_route_dev, sizeof(ds->wg_route_dev), wg_ifname);
  cpy(ds->vx_remote, sizeof(ds->vx_remote), peeraddr);
  cpy(ds->vx_bridge, sizeof(ds->vx_bridge), br_name);

  rc = add_device_ports(ports, ds, 1);
  if (rc)
    return rc;

  return add_wireless_ports(wifi, if_sect, ds);
}

static int uci_build_desired_set(const char *network_path, const char *wireless_path, struct desired_set *set) {
  struct uci_db net;
  struct uci_db wifi;
  int i;
  int rc;

  memset(&net, 0, sizeof(net));
  memset(&wifi, 0, sizeof(wifi));
  memset(set, 0, sizeof(*set));

  rc = db_load_file(&net, network_path);
  if (rc)
    return rc;

  rc = db_load_file(&wifi, wireless_path);
  if (rc)
    return rc;

  for (i = 0; i < net.n_item; i++) {
    const char *br_name;
    const char *dev_type;
    const char *if_sect;
    const char *proto;

    if (!net.item[i].is_type)
      continue;
    if (strcmp(net.item[i].pkg, "network"))
      continue;
    if (strcmp(net.item[i].val, "device"))
      continue;

    dev_type = db_get(&net, "network", net.item[i].sect, "type");
    if (!dev_type || strcmp(dev_type, "bridge"))
      continue;

    br_name = db_get(&net, "network", net.item[i].sect, "name");
    if (!br_name || !br_name[0]) {
      fprintf(stderr, "uci: bridge device %s missing name\n", net.item[i].sect);
      return -EINVAL;
    }

    if_sect = find_iface_by_device(&net, br_name);
    if (!if_sect) {
      fprintf(stderr, "uci: no interface bound to bridge %s\n", br_name);
      return -EINVAL;
    }

    proto = db_get(&net, "network", if_sect, "proto");
    if (!proto || !proto[0]) {
      fprintf(stderr, "uci: bridge %s missing proto on network.%s\n", br_name, if_sect);
      return -EINVAL;
    }

    if (!strcmp(proto, "static") || !strcmp(proto, "dhcp")) {
      rc = build_uplink_bridge(&net, &wifi, net.item[i].sect, if_sect, br_name, set);
    } else if (!strcmp(proto, "none")) {
      rc = build_wg_vxlan_bridge(&net, &wifi, net.item[i].sect, if_sect, br_name, set);
    } else {
      fprintf(stderr, "uci: unsupported bridge proto=%s on %s\n", proto, br_name);
      return -EINVAL;
    }

    if (rc)
      return rc;
  }

  if (!set->n_state) {
    fprintf(stderr, "uci: no desired states built\n");
    return -EINVAL;
  }

  return 0;
}

static int uci2yml_file(const char *network_path, const char *wireless_path, FILE *f) {
  struct desired_set set;
  int rc;

  rc = uci_build_desired_set(network_path, wireless_path, &set);
  if (rc)
    return rc;

  return cfg_set(f, &set);
}

int uci2yml(const char *network_path, const char *wireless_path, const char *yaml_path) {
  FILE *f;
  int rc;

  f = fopen(yaml_path, "wb");
  if (!f) {
    perror(yaml_path);
    return -errno;
  }

  rc = uci2yml_file(network_path, wireless_path, f);
  if (!rc && fflush(f) < 0)
    rc = -errno;
  if (fclose(f) < 0 && !rc)
    rc = -errno;

  if (rc)
    unlink(yaml_path);

  return rc;
}

int uci_load_desired_set(const char *network_path, const char *wireless_path, struct desired_set *set) {
  char tmp[] = "/tmp/netrec-uci-XXXXXX";
  FILE *f;
  int fd;
  int rc;

  fd = mkstemp(tmp);
  if (fd < 0) {
    fprintf(stderr, "uci: mkstemp failed: %s\n", strerror(errno));
    return -errno;
  }

  f = fdopen(fd, "wb");
  if (!f) {
    rc = -errno;
    close(fd);
    unlink(tmp);
    fprintf(stderr, "uci: fdopen failed: %s\n", strerror(-rc));
    return rc;
  }

  rc = uci2yml_file(network_path, wireless_path, f);
  if (!rc && fflush(f) < 0)
    rc = -errno;
  if (fclose(f) < 0 && !rc)
    rc = -errno;
  if (rc) {
    unlink(tmp);
    return rc;
  }

  rc = yaml_load_desired_set(tmp, set);
  unlink(tmp);
  return rc;
}
