#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <arpa/inet.h>
#include <linux/if.h>

#include "verify.h"

struct vctx {
	int apply;
	int act_fail;
};

static int parse_addr_prefix(const char *s, uint32_t *addr, int *prefix)
{
	char buf[64];
	char *p;
	char *end;
	long v;

	if (!s || !*s) {
		return -EINVAL;
	}

	snprintf(buf, sizeof(buf), "%s", s);
	p = strchr(buf, '/');
	if (!p) {
		return -EINVAL;
	}
	*p++ = '\0';

	if (inet_pton(AF_INET, buf, addr) != 1) {
		return -EINVAL;
	}

	errno = 0;
	v = strtol(p, &end, 10);
	if (errno || *end || v < 0 || v > 32) {
		return -EINVAL;
	}

	*prefix = v;
	return 0;
}

static int parse_ip4(const char *s, uint32_t *addr)
{
	if (inet_pton(AF_INET, s, addr) != 1) {
		return -EINVAL;
	}

	return 0;
}

static int route_exact(const struct real_state *rs, uint32_t dst,
		       int prefix, int oif, uint32_t gw, int has_gw)
{
	int i;

	if (oif <= 0) {
		return 0;
	}

	for (i = 0; i < rs->n_route4; i++) {
		if (rs->route4[i].oif != oif) {
			continue;
		}
		if (rs->route4[i].prefix != prefix) {
			continue;
		}
		if (rs->route4[i].dst != dst) {
			continue;
		}
		if (has_gw && (!rs->route4[i].has_gw || rs->route4[i].gw != gw)) {
			continue;
		}
		if (!has_gw && rs->route4[i].has_gw) {
			continue;
		}
		return 1;
	}

	return 0;
}

static int addr_exists(const struct real_state *rs, int ifindex, uint32_t addr,
		       int prefix)
{
	int i;

	for (i = 0; i < rs->n_addr4; i++) {
		if (rs->addr4[i].ifindex != ifindex) {
			continue;
		}
		if (rs->addr4[i].prefix != prefix) {
			continue;
		}
		if (rs->addr4[i].addr == addr) {
			return 1;
		}
	}

	return 0;
}

static int addr_any_exists(const struct real_state *rs, int ifindex)
{
	int i;

	for (i = 0; i < rs->n_addr4; i++) {
		if (rs->addr4[i].ifindex == ifindex) {
			return 1;
		}
	}

	return 0;
}

static int subst_iface(char *dst, size_t sz, const char *src, const char *ifname)
{
	static const char key[] = "$iface";
	size_t n = 0;
	size_t klen = sizeof(key) - 1;
	size_t ilen;

	if (!sz) {
		return -ENOSPC;
	}

	ilen = strlen(ifname);
	while (*src) {
		if (!strncmp(src, key, klen)) {
			if (n + ilen >= sz) {
				return -ENOSPC;
			}
			memcpy(dst + n, ifname, ilen);
			n += ilen;
			src += klen;
			continue;
		}

		if (n + 1 >= sz) {
			return -ENOSPC;
		}
		dst[n++] = *src++;
	}

	dst[n] = '\0';
	return 0;
}

static int ok(const char *fmt, ...)
{
	va_list ap;

	printf("OK ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");

	return 0;
}

static int miss(const char *fmt, ...)
{
	va_list ap;

	printf("MISS ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");

	return 1;
}

static int cmd_split(char *cmd, char **argv, int max)
{
	int argc = 0;
	char *p = cmd;

	while (*p) {
		while (*p == ' ' || *p == '\t') {
			*p++ = '\0';
		}
		if (!*p) {
			break;
		}
		if (argc + 1 >= max) {
			return -E2BIG;
		}
		argv[argc++] = p;
		while (*p && *p != ' ' && *p != '\t') {
			p++;
		}
	}

	argv[argc] = NULL;
	return argc;
}

static int cmd_exec(const char *cmd)
{
	/* --apply выполняет argv напрямую: без shell, без system(). */
	char buf[256];
	char *argv[32];
	pid_t pid;
	int status;
	int argc;

	if (cmd[0] == '#') {
		return 0;
	}

	snprintf(buf, sizeof(buf), "%s", cmd);
	argc = cmd_split(buf, argv, 32);
	if (argc <= 0) {
		return argc ? argc : -EINVAL;
	}

	pid = fork();
	if (pid < 0) {
		return -errno;
	}
	if (!pid) {
		execvp(argv[0], argv);
		_exit(127);
	}

	for (;;) {
		if (waitpid(pid, &status, 0) < 0) {
			if (errno == EINTR) {
				continue;
			}
			return -errno;
		}
		break;
	}

	if (WIFEXITED(status)) {
		return WEXITSTATUS(status);
	}
	if (WIFSIGNALED(status)) {
		return 128 + WTERMSIG(status);
	}

	return -EIO;
}

static void act(struct vctx *ctx, const char *fmt, ...)
{
	/* ACT всегда печатается; выполняется только при --apply. */
	char cmd[256];
	va_list ap;
	int rc;

	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);

	printf("ACT %s\n", cmd);

	if (!ctx->apply || cmd[0] == '#') {
		return;
	}

	rc = cmd_exec(cmd);
	if (rc) {
		printf("APPLY_FAIL rc=%d cmd=%s\n", rc, cmd);
		ctx->act_fail = 1;
		return;
	}

	printf("APPLY_OK cmd=%s\n", cmd);
}

static int check_bridge(const struct desired_state *ds,
			const struct real_state *rs, struct vctx *ctx)
{
	const struct iface *br;
	int diff = 0;

	br = rs_find_iface(rs, ds->br_name);
	if (br && !strcmp(br->kind, "bridge")) {
		ok("bridge %s exists", ds->br_name);
		if (br->flags & IFF_UP) {
			ok("bridge %s up", ds->br_name);
		} else {
			miss("bridge %s up", ds->br_name);
			act(ctx, "ip link set %s up", ds->br_name);
			diff++;
		}
		return diff;
	}
	if (br) {
		miss("bridge %s type bridge", ds->br_name);
		act(ctx, "ip link del %s", ds->br_name);
		act(ctx, "ip link add %s type bridge", ds->br_name);
		act(ctx, "ip link set %s up", ds->br_name);
		return 1;
	}

	miss("bridge %s exists", ds->br_name);
	act(ctx, "ip link add %s type bridge", ds->br_name);
	act(ctx, "ip link set %s up", ds->br_name);
	return 1;
}

static int check_bridge_addr(const struct desired_state *ds,
			     const struct real_state *rs, struct vctx *ctx)
{
	const struct iface *br;
	char cmd[256];
	uint32_t addr;
	int prefix;

	if (ds->br_addr_mode == ADDR_NONE) {
		return 0;
	}

	br = rs_find_iface(rs, ds->br_name);
	if (ds->br_addr_mode == ADDR_DHCP) {
		if (br && addr_any_exists(rs, br->ifindex)) {
			ok("dhcp addr dev %s", ds->br_name);
			return 0;
		}

		miss("dhcp addr dev %s", ds->br_name);
		if (subst_iface(cmd, sizeof(cmd), ds->br_dhcp_cmd, ds->br_name)) {
			act(ctx, "# bad yaml bridge.dhcp_cmd too long");
			return 1;
		}
		act(ctx, "%s", cmd);
		return 1;
	}

	if (parse_addr_prefix(ds->br_addr, &addr, &prefix)) {
		miss("addr %s dev %s", ds->br_addr, ds->br_name);
		act(ctx, "# bad yaml bridge.addr %s", ds->br_addr);
		return 1;
	}

	if (br && addr_exists(rs, br->ifindex, addr, prefix)) {
		ok("addr %s dev %s", ds->br_addr, ds->br_name);
		return 0;
	}

	miss("addr %s dev %s", ds->br_addr, ds->br_name);
	act(ctx, "ip addr add %s dev %s", ds->br_addr, ds->br_name);
	return 1;
}


static int check_bridge_gateway(const struct desired_state *ds,
				const struct real_state *rs, struct vctx *ctx)
{
	const struct iface *br;
	uint32_t gw;

	if (!ds->br_gateway[0]) {
		return 0;
	}
	if (parse_ip4(ds->br_gateway, &gw)) {
		miss("gateway %s dev %s", ds->br_gateway, ds->br_name);
		act(ctx, "# bad yaml bridge.gateway %s", ds->br_gateway);
		return 1;
	}

	br = rs_find_iface(rs, ds->br_name);
	if (br && route_exact(rs, 0, 0, br->ifindex, gw, 1)) {
		ok("gateway %s dev %s", ds->br_gateway, ds->br_name);
		return 0;
	}

	miss("gateway %s dev %s", ds->br_gateway, ds->br_name);
	act(ctx, "ip route replace 0.0.0.0/0 via %s dev %s",
	    ds->br_gateway, ds->br_name);
	return 1;
}

static int check_bridge_dns(const struct desired_state *ds,
			    const struct real_state *rs, struct vctx *ctx)
{
	uint32_t addr;
	int diff = 0;
	int i;

	for (i = 0; i < ds->n_br_dns; i++) {
		if (parse_ip4(ds->br_dns[i], &addr)) {
			miss("dns %s dev %s", ds->br_dns[i], ds->br_name);
			act(ctx, "# bad yaml bridge.dns %s", ds->br_dns[i]);
			diff++;
			continue;
		}
		if (rs_dns4_exists(rs, addr)) {
			ok("dns %s dev %s", ds->br_dns[i], ds->br_name);
			continue;
		}

		miss("dns %s dev %s", ds->br_dns[i], ds->br_name);
		act(ctx, "# set dns %s dev %s", ds->br_dns[i], ds->br_name);
		diff++;
	}

	return diff;
}

static int check_wg_peer_route(const struct desired_state *ds,
			       const struct real_state *rs, struct vctx *ctx)
{
	const struct iface *dev;
	uint32_t ip;

	if (parse_ip4(ds->wg_peer_ip, &ip)) {
		miss("route %s/32 dev %s", ds->wg_peer_ip, ds->wg_route_dev);
		act(ctx, "# bad yaml wg.peer_ip %s", ds->wg_peer_ip);
		return 1;
	}

	dev = rs_find_iface(rs, ds->wg_route_dev);
	if (dev && route_exact(rs, ip, 32, dev->ifindex, 0, 0)) {
		ok("route %s/32 dev %s", ds->wg_peer_ip, ds->wg_route_dev);
		return 0;
	}

	miss("route %s/32 dev %s", ds->wg_peer_ip, ds->wg_route_dev);
	act(ctx, "ip route replace %s/32 dev %s", ds->wg_peer_ip, ds->wg_route_dev);
	return 1;
}

static int check_one_route(const struct desired_route4 *rt,
			   const struct real_state *rs, struct vctx *ctx)
{
	const struct iface *dev;
	uint32_t dst;
	uint32_t via = 0;
	int prefix;

	if (parse_addr_prefix(rt->dst, &dst, &prefix)) {
		miss("route %s dev %s", rt->dst, rt->dev);
		act(ctx, "# bad yaml route.dst %s", rt->dst);
		return 1;
	}
	if (rt->has_via && parse_ip4(rt->via, &via)) {
		miss("route %s via %s dev %s", rt->dst, rt->via, rt->dev);
		act(ctx, "# bad yaml route.via %s", rt->via);
		return 1;
	}

	dev = rs_find_iface(rs, rt->dev);
	if (dev && route_exact(rs, dst, prefix, dev->ifindex, via, rt->has_via)) {
		if (rt->has_via) {
			ok("route %s via %s dev %s", rt->dst, rt->via, rt->dev);
		} else {
			ok("route %s dev %s", rt->dst, rt->dev);
		}
		return 0;
	}

	if (rt->has_via) {
		miss("route %s via %s dev %s", rt->dst, rt->via, rt->dev);
		act(ctx, "ip route replace %s via %s dev %s",
		    rt->dst, rt->via, rt->dev);
	} else {
		miss("route %s dev %s", rt->dst, rt->dev);
		act(ctx, "ip route replace %s dev %s", rt->dst, rt->dev);
	}

	return 1;
}

static int check_routes(const struct desired_state *ds,
			const struct real_state *rs, struct vctx *ctx)
{
	int diff = 0;
	int i;

	for (i = 0; i < ds->n_routes; i++) {
		diff += check_one_route(&ds->routes[i], rs, ctx);
	}

	return diff;
}

static int check_iface_exists(const char *tag, const char *name,
			      const struct iface *iface)
{
	if (iface) {
		ok("%s %s exists", tag, name);
		return 0;
	}

	return miss("%s %s exists", tag, name);
}

static int check_iface_up(const char *name, const struct iface *iface,
			  struct vctx *ctx)
{
	if (!iface) {
		miss("iface %s", name);
		return 1;
	}
	if (iface->flags & IFF_UP) {
		ok("iface %s up", name);
		return 0;
	}

	miss("iface %s up", name);
	act(ctx, "ip link set %s up", name);
	return 1;
}

static int check_master(const char *ifname, const struct iface *iface,
			const char *br_name, const struct real_state *rs,
			struct vctx *ctx)
{
	const struct iface *br;

	if (!iface) {
		return 0;
	}

	br = rs_find_iface(rs, br_name);
	if (br && iface->master == br->ifindex) {
		ok("iface %s master %s", ifname, br_name);
		return 0;
	}

	miss("iface %s master %s", ifname, br_name);
	act(ctx, "ip link set %s master %s", ifname, br_name);
	return 1;
}

static int check_bridge_ports(const struct desired_state *ds,
			      const struct real_state *rs, struct vctx *ctx)
{
	/* bridge.ports - дополнительные порты моста, кроме обязательного uplink. */
	const struct iface *port;
	int diff = 0;
	int i;

	for (i = 0; i < ds->n_br_ports; i++) {
		port = rs_find_iface(rs, ds->br_ports[i]);
		diff += check_iface_exists("bridge-port", ds->br_ports[i], port);
		if (!port) {
			act(ctx, "# provide bridge port iface %s", ds->br_ports[i]);
			continue;
		}

		diff += check_iface_up(ds->br_ports[i], port, ctx);
		diff += check_master(ds->br_ports[i], port, ds->br_name, rs, ctx);
	}

	return diff;
}

static void act_add_vxlan(const struct desired_state *ds, struct vctx *ctx)
{
	act(ctx, "ip link add %s type vxlan id %u remote %s dev %s",
	    ds->vx_ifname, ds->vx_vni, ds->vx_remote, ds->vx_dev);
}

static int check_vxlan_attrs(const struct desired_state *ds,
			     const struct real_state *rs,
			     const struct iface *vx_if,
			     const struct vxlan *vx,
			     struct vctx *ctx, int *recreate)
{
	/* VNI/remote/dev нельзя безопасно менять частично: проще пересоздать. */
	const struct iface *dev;
	uint32_t remote;
	int diff = 0;

	*recreate = 0;

	if (!vx) {
		if (vx_if) {
			miss("iface %s type vxlan", ds->vx_ifname);
			act(ctx, "ip link del %s", ds->vx_ifname);
		} else {
			miss("iface %s", ds->vx_ifname);
		}
		act_add_vxlan(ds, ctx);
		act(ctx, "ip link set %s up", ds->vx_ifname);
		act(ctx, "ip link set %s master %s", ds->vx_ifname, ds->vx_bridge);
		*recreate = 1;
		return 1;
	}

	ok("iface %s exists", ds->vx_ifname);

	if (vx->has_vni && vx->vni == ds->vx_vni) {
		ok("vxlan %s vni %u", ds->vx_ifname, ds->vx_vni);
	} else {
		miss("vxlan %s vni %u", ds->vx_ifname, ds->vx_vni);
		diff++;
		*recreate = 1;
	}

	if (parse_ip4(ds->vx_remote, &remote)) {
		miss("vxlan %s remote %s", ds->vx_ifname, ds->vx_remote);
		act(ctx, "# bad yaml vxlan.remote %s", ds->vx_remote);
		diff++;
	} else if (vx->has_remote && vx->remote == remote) {
		ok("vxlan %s remote %s", ds->vx_ifname, ds->vx_remote);
	} else {
		miss("vxlan %s remote %s", ds->vx_ifname, ds->vx_remote);
		diff++;
		*recreate = 1;
	}

	dev = rs_find_iface(rs, ds->vx_dev);
	if (dev && vx->has_link && vx->link == dev->ifindex) {
		ok("vxlan %s dev %s", ds->vx_ifname, ds->vx_dev);
	} else {
		miss("vxlan %s dev %s", ds->vx_ifname, ds->vx_dev);
		diff++;
		*recreate = 1;
	}

	if (*recreate) {
		act(ctx, "ip link del %s", ds->vx_ifname);
		act_add_vxlan(ds, ctx);
		act(ctx, "ip link set %s up", ds->vx_ifname);
		act(ctx, "ip link set %s master %s", ds->vx_ifname, ds->vx_bridge);
		return diff;
	}

	diff += check_iface_up(ds->vx_ifname, vx_if, ctx);
	return diff;
}

static void act_add_vlan(const struct desired_state *ds, struct vctx *ctx)
{
	act(ctx, "ip link add link %s name %s type vlan id %u",
	    ds->vlan_link, ds->vlan_ifname, ds->vlan_id);
}

static int check_vlan_attrs(const struct desired_state *ds,
			    const struct real_state *rs,
			    const struct iface *vl_if,
			    const struct vlan *vl,
			    struct vctx *ctx, int *recreate)
{
	/* VLAN id/link тоже считаем immutable для MVP: diff => recreate. */
	const struct iface *link;
	int diff = 0;

	*recreate = 0;

	if (!vl) {
		if (vl_if) {
			miss("iface %s type vlan", ds->vlan_ifname);
			act(ctx, "ip link del %s", ds->vlan_ifname);
		} else {
			miss("iface %s", ds->vlan_ifname);
		}
		act_add_vlan(ds, ctx);
		act(ctx, "ip link set %s up", ds->vlan_ifname);
		act(ctx, "ip link set %s master %s",
		    ds->vlan_ifname, ds->vlan_bridge);
		*recreate = 1;
		return 1;
	}

	ok("iface %s exists", ds->vlan_ifname);

	if (vl->has_id && vl->id == ds->vlan_id) {
		ok("vlan %s id %u", ds->vlan_ifname, ds->vlan_id);
	} else {
		miss("vlan %s id %u", ds->vlan_ifname, ds->vlan_id);
		diff++;
		*recreate = 1;
	}

	link = rs_find_iface(rs, ds->vlan_link);
	if (link && vl->has_link && vl->link == link->ifindex) {
		ok("vlan %s link %s", ds->vlan_ifname, ds->vlan_link);
	} else {
		miss("vlan %s link %s", ds->vlan_ifname, ds->vlan_link);
		diff++;
		*recreate = 1;
	}

	if (*recreate) {
		act(ctx, "ip link del %s", ds->vlan_ifname);
		act_add_vlan(ds, ctx);
		act(ctx, "ip link set %s up", ds->vlan_ifname);
		act(ctx, "ip link set %s master %s",
		    ds->vlan_ifname, ds->vlan_bridge);
		return diff;
	}

	diff += check_iface_up(ds->vlan_ifname, vl_if, ctx);
	return diff;
}

static int verify_only_uplink(const struct desired_state *ds,
			      const struct real_state *rs, struct vctx *ctx)
{
	const struct iface *up;
	int diff = 0;

	up = rs_find_iface(rs, ds->up_ifname);
	diff += check_iface_exists("uplink", ds->up_ifname, up);
	if (!up) {
		act(ctx, "# provide uplink iface %s", ds->up_ifname);
		return diff;
	}

	diff += check_iface_up(ds->up_ifname, up, ctx);
	return diff;
}

static int verify_uplink_bridge(const struct desired_state *ds,
				const struct real_state *rs, struct vctx *ctx)
{
	const struct iface *up;
	int diff = 0;

	diff += check_bridge(ds, rs, ctx);
	diff += check_bridge_addr(ds, rs, ctx);
	diff += check_bridge_gateway(ds, rs, ctx);
	diff += check_bridge_dns(ds, rs, ctx);
	diff += verify_only_uplink(ds, rs, ctx);

	up = rs_find_iface(rs, ds->up_ifname);
	diff += check_master(ds->up_ifname, up, ds->br_name, rs, ctx);
	diff += check_bridge_ports(ds, rs, ctx);
	return diff;
}

static int verify_vlan_bridge(const struct desired_state *ds,
			      const struct real_state *rs, struct vctx *ctx)
{
	const struct iface *vl_if;
	const struct vlan *vl;
	int diff = 0;
	int recreate = 0;

	diff += check_bridge(ds, rs, ctx);
	diff += check_bridge_addr(ds, rs, ctx);
	diff += check_bridge_gateway(ds, rs, ctx);
	diff += check_bridge_dns(ds, rs, ctx);
	diff += verify_only_uplink(ds, rs, ctx);
	diff += check_bridge_ports(ds, rs, ctx);

	vl_if = rs_find_iface(rs, ds->vlan_ifname);
	vl = rs_find_vlan(rs, ds->vlan_ifname);
	diff += check_vlan_attrs(ds, rs, vl_if, vl, ctx, &recreate);
	if (!recreate) {
		diff += check_master(ds->vlan_ifname, vl_if, ds->vlan_bridge, rs, ctx);
	}
	return diff;
}

static int verify_wg_vxlan_bridge(const struct desired_state *ds,
				   const struct real_state *rs, struct vctx *ctx)
{
	const struct iface *wg;
	const struct iface *vx_if;
	const struct vxlan *vx;
	int diff = 0;
	int recreate = 0;

	diff += check_bridge(ds, rs, ctx);
	diff += check_bridge_addr(ds, rs, ctx);
	diff += check_bridge_gateway(ds, rs, ctx);
	diff += check_bridge_dns(ds, rs, ctx);
	if (ds->up_ifname[0]) {
		diff += verify_only_uplink(ds, rs, ctx);
	}
	diff += check_bridge_ports(ds, rs, ctx);
	diff += check_wg_peer_route(ds, rs, ctx);

	wg = rs_find_iface(rs, ds->wg_ifname);
	if (wg) {
		ok("iface %s exists", ds->wg_ifname);
	} else {
		diff += miss("iface %s", ds->wg_ifname);
		act(ctx, "ip link add %s type wireguard", ds->wg_ifname);
	}
	if (wg) {
		diff += check_iface_up(ds->wg_ifname, wg, ctx);
	}

	vx_if = rs_find_iface(rs, ds->vx_ifname);
	vx = rs_find_vxlan(rs, ds->vx_ifname);
	diff += check_vxlan_attrs(ds, rs, vx_if, vx, ctx, &recreate);
	if (!recreate) {
		diff += check_master(ds->vx_ifname, vx_if, ds->vx_bridge, rs, ctx);
	}
	return diff;
}

int verify_state(const struct desired_state *ds, const struct real_state *rs,
		 int apply, int *act_fail)
{
	/* Один явный dispatch по scenario, без registry/callback framework. */
	struct vctx ctx;
	int diff;

	memset(&ctx, 0, sizeof(ctx));
	ctx.apply = apply;

	if (!strcmp(ds->scenario, "only_uplink") ||
	    !strcmp(ds->scenario, "only_uplink_iface")) {
		diff = verify_only_uplink(ds, rs, &ctx);
	} else if (!strcmp(ds->scenario, "uplink_bridge") ||
		   !strcmp(ds->scenario, "uplink_in_bridge")) {
		diff = verify_uplink_bridge(ds, rs, &ctx);
	} else if (!strcmp(ds->scenario, "vlan_bridge") ||
		   !strcmp(ds->scenario, "vlan_in_bridge")) {
		diff = verify_vlan_bridge(ds, rs, &ctx);
	} else if (!strcmp(ds->scenario, "wg_vxlan_bridge")) {
		diff = verify_wg_vxlan_bridge(ds, rs, &ctx);
	} else {
		fprintf(stderr, "verify: unsupported scenario=%s\n", ds->scenario);
		return 1;
	}

	diff += check_routes(ds, rs, &ctx);

	if (act_fail) {
		*act_fail = ctx.act_fail;
	}

	return diff;
}
