#include <string.h>

#include "state.h"

void rs_free(struct real_state *rs)
{
	/* Пока malloc нет; функция оставлена как единая точка cleanup. */
	memset(rs, 0, sizeof(*rs));
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
