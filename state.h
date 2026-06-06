#ifndef NETREC_STATE_H
#define NETREC_STATE_H

#include <stdint.h>
#include <netinet/in.h>
#include <linux/if.h>

/* Статические лимиты MVP. При переполнении печатаем понятную ошибку. */
#ifndef NR_IFACE_MAX
#define NR_IFACE_MAX	2048
#endif
#ifndef NR_ADDR4_MAX
#define NR_ADDR4_MAX	8192
#endif
#ifndef NR_ROUTE4_MAX
#define NR_ROUTE4_MAX	32768
#endif
#ifndef NR_VXLAN_MAX
#define NR_VXLAN_MAX	1024
#endif
#ifndef NR_VLAN_MAX
#define NR_VLAN_MAX	4096
#endif
#ifndef NR_BR_PORT_MAX
#define NR_BR_PORT_MAX	32
#endif

/* Плоский desired_state: один YAML scenario без generic graph. */
struct desired_state {
	char scenario[32];

	char br_name[IFNAMSIZ];
	char br_addr[32];
	char br_ports[NR_BR_PORT_MAX][IFNAMSIZ];
	int n_br_ports;

	char up_ifname[IFNAMSIZ];

	char vlan_ifname[IFNAMSIZ];
	uint32_t vlan_id;
	char vlan_link[IFNAMSIZ];
	char vlan_bridge[IFNAMSIZ];

	char wg_ifname[IFNAMSIZ];
	char wg_peer_ip[INET_ADDRSTRLEN];
	char wg_route_dev[IFNAMSIZ];

	char vx_ifname[IFNAMSIZ];
	uint32_t vx_vni;
	char vx_remote[INET_ADDRSTRLEN];
	char vx_dev[IFNAMSIZ];
	char vx_bridge[IFNAMSIZ];
};

struct iface {
	char name[IFNAMSIZ];
	char kind[16];
	int ifindex;
	unsigned int flags;
	int has_carrier;
	int carrier;
	int master;
	int link;
	int has_link;
};

struct addr4 {
	int ifindex;
	uint32_t addr;
	int prefix;
};

struct route4 {
	uint32_t dst;
	int prefix;
	uint32_t gw;
	int has_gw;
	int oif;
};

struct vxlan {
	char ifname[IFNAMSIZ];
	int ifindex;
	uint32_t vni;
	int has_vni;
	uint32_t remote;
	int has_remote;
	int link;
	int has_link;
};

struct vlan {
	char ifname[IFNAMSIZ];
	int ifindex;
	uint32_t id;
	int has_id;
	int link;
	int has_link;
};

/* Снимок ядра. Fixed arrays намеренно: проще отлаживать и предсказуемее. */
struct real_state {
	struct iface ifaces[NR_IFACE_MAX];
	int n_ifaces;

	struct addr4 addr4[NR_ADDR4_MAX];
	int n_addr4;

	struct route4 route4[NR_ROUTE4_MAX];
	int n_route4;

	struct vxlan vxlan[NR_VXLAN_MAX];
	int n_vxlan;

	struct vlan vlan[NR_VLAN_MAX];
	int n_vlan;
};

struct diff {
	int miss;
	char msg[128];
	char act[256];
};

void rs_free(struct real_state *rs);
const struct iface *rs_find_iface(const struct real_state *rs, const char *name);
const struct iface *rs_find_iface_idx(const struct real_state *rs, int ifindex);
const struct vxlan *rs_find_vxlan(const struct real_state *rs, const char *name);
const struct vlan *rs_find_vlan(const struct real_state *rs, const char *name);

#endif
