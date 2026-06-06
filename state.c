#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <arpa/inet.h>

#include "state.h"

void rs_free(struct real_state *rs)
{
	/* Пока malloc нет; функция оставлена как единая точка cleanup. */
	memset(rs, 0, sizeof(*rs));
}

int rs_load_resolv_conf(struct real_state *rs, const char *path)
{
	/* DNS не kernel-state. Пока читаем только global resolv.conf nameserver IPv4. */
	char line[256];
	char key[32];
	char val[64];
	uint32_t addr;
	FILE *f;

	rs->n_dns4 = 0;

	f = fopen(path, "r");
	if (!f) {
		return 0;
	}

	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%31s %63s", key, val) != 2) {
			continue;
		}
		if (strcmp(key, "nameserver")) {
			continue;
		}
		if (inet_pton(AF_INET, val, &addr) != 1) {
			continue;
		}
		if (rs->n_dns4 >= NR_DNS4_MAX) {
			fclose(f);
			fprintf(stderr, "resolv: too many IPv4 nameservers, max=%d\n",
				NR_DNS4_MAX);
			return -ENOSPC;
		}
		rs->dns4[rs->n_dns4++] = addr;
	}

	fclose(f);
	return 0;
}

const struct iface *rs_find_iface(const struct real_state *rs, const char *name)
{
	/* Линейный поиск достаточен для MVP и проще отладки. */
	int i;

	for (i = 0; i < rs->n_ifaces; i++) {
		if (!strcmp(rs->ifaces[i].name, name)) {
			return &rs->ifaces[i];
		}
	}

	return NULL;
}

const struct iface *rs_find_iface_idx(const struct real_state *rs, int ifindex)
{
	int i;

	for (i = 0; i < rs->n_ifaces; i++) {
		if (rs->ifaces[i].ifindex == ifindex) {
			return &rs->ifaces[i];
		}
	}

	return NULL;
}

const struct vxlan *rs_find_vxlan(const struct real_state *rs, const char *name)
{
	int i;

	for (i = 0; i < rs->n_vxlan; i++) {
		if (!strcmp(rs->vxlan[i].ifname, name)) {
			return &rs->vxlan[i];
		}
	}

	return NULL;
}

const struct vlan *rs_find_vlan(const struct real_state *rs, const char *name)
{
	int i;

	for (i = 0; i < rs->n_vlan; i++) {
		if (!strcmp(rs->vlan[i].ifname, name)) {
			return &rs->vlan[i];
		}
	}

	return NULL;
}

int rs_dns4_exists(const struct real_state *rs, uint32_t addr)
{
	int i;

	for (i = 0; i < rs->n_dns4; i++) {
		if (rs->dns4[i] == addr) {
			return 1;
		}
	}

	return 0;
}
