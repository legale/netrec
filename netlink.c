#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <linux/if_link.h>
#include <linux/if_vlan.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sys/socket.h>

#include "netlink.h"

#ifndef IFLA_VXLAN_MAX
#define IFLA_VXLAN_ID 1
#define IFLA_VXLAN_GROUP 2
#define IFLA_VXLAN_LINK 3
#define IFLA_VXLAN_MAX 3
#endif

#ifndef IFLA_VLAN_MAX
#define IFLA_VLAN_ID 1
#define IFLA_VLAN_MAX 1
#endif


static char nl_err_msg[160];

static int nl_limit(const char *name, int max)
{
	snprintf(nl_err_msg, sizeof(nl_err_msg),
		 "too many %s in netlink dump, max=%d", name, max);
	return -ENOSPC;
}

const char *nl_errstr(int rc)
{
	if (rc == -ENOSPC && nl_err_msg[0]) {
		return nl_err_msg;
	}

	return strerror(-rc);
}

static int nl_open(void)
{
	/* Один NETLINK_ROUTE socket на весь snapshot. */
	struct sockaddr_nl sa;
	int fd;
	int rc;

	fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
	if (fd < 0) {
		return -errno;
	}

	memset(&sa, 0, sizeof(sa));
	sa.nl_family = AF_NETLINK;

	rc = bind(fd, (struct sockaddr *)&sa, sizeof(sa));
	if (rc < 0) {
		rc = -errno;
		close(fd);
		return rc;
	}

	return fd;
}

static int nl_send(int fd, int type, const void *body, size_t body_len,
		   uint32_t seq)
{
	/* Все запросы dump-only: просим ядро отдать текущее состояние. */
	struct sockaddr_nl sa;
	char buf[256];
	struct nlmsghdr *nh = (struct nlmsghdr *)buf;
	int len;
	int rc;

	len = NLMSG_SPACE(body_len);
	if ((size_t)len > sizeof(buf)) {
		return -EINVAL;
	}

	memset(buf, 0, sizeof(buf));
	nh->nlmsg_len = NLMSG_LENGTH(body_len);
	nh->nlmsg_type = type;
	nh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	nh->nlmsg_seq = seq;
	memcpy(NLMSG_DATA(nh), body, body_len);

	memset(&sa, 0, sizeof(sa));
	sa.nl_family = AF_NETLINK;

	rc = sendto(fd, nh, nh->nlmsg_len, 0, (struct sockaddr *)&sa,
		    sizeof(sa));
	if (rc < 0) {
		return -errno;
	}
	if ((size_t)rc != nh->nlmsg_len) {
		return -EIO;
	}

	return 0;
}

static int nl_err(struct nlmsghdr *nh)
{
	struct nlmsgerr *err;

	if (nh->nlmsg_len < NLMSG_LENGTH(sizeof(*err))) {
		return -EIO;
	}

	err = NLMSG_DATA(nh);
	if (!err->error) {
		return 0;
	}

	return err->error;
}

static void parse_rtattr(struct rtattr **tb, int max, struct rtattr *rta,
			 int len)
{
	/* Простой разбор rtattr в таблицу по type; nested разбираются отдельно. */
	int type;

	memset(tb, 0, sizeof(*tb) * (max + 1));
	while (RTA_OK(rta, len)) {
		type = rta->rta_type & NLA_TYPE_MASK;
		if (type <= max) {
			tb[type] = rta;
		}
		rta = RTA_NEXT(rta, len);
	}
}


static void link_kind(struct rtattr **tb, char *dst, size_t len)
{
	struct rtattr *li[IFLA_INFO_MAX + 1];
	int plen;

	if (!len) {
		return;
	}

	dst[0] = '\0';
	if (!tb[IFLA_LINKINFO]) {
		return;
	}

	plen = RTA_PAYLOAD(tb[IFLA_LINKINFO]);
	parse_rtattr(li, IFLA_INFO_MAX, RTA_DATA(tb[IFLA_LINKINFO]), plen);
	if (!li[IFLA_INFO_KIND]) {
		return;
	}

	snprintf(dst, len, "%s", (char *)RTA_DATA(li[IFLA_INFO_KIND]));
}

static int add_iface(struct real_state *rs, struct ifinfomsg *ifi,
		     struct rtattr **tb)
{
	/* Базовая запись интерфейса: имя, flags, carrier, master, link. */
	struct iface *iface;

	if (!tb[IFLA_IFNAME]) {
		return 0;
	}
	if (rs->n_ifaces >= NR_IFACE_MAX) {
		return nl_limit("interfaces", NR_IFACE_MAX);
	}

	iface = &rs->ifaces[rs->n_ifaces++];
	memset(iface, 0, sizeof(*iface));
	iface->ifindex = ifi->ifi_index;
	iface->flags = ifi->ifi_flags;
	snprintf(iface->name, sizeof(iface->name), "%s",
		 (char *)RTA_DATA(tb[IFLA_IFNAME]));
	link_kind(tb, iface->kind, sizeof(iface->kind));

	if (tb[IFLA_CARRIER] && RTA_PAYLOAD(tb[IFLA_CARRIER]) >= 1) {
		iface->has_carrier = 1;
		iface->carrier = *(uint8_t *)RTA_DATA(tb[IFLA_CARRIER]);
	}
	if (tb[IFLA_MASTER] && RTA_PAYLOAD(tb[IFLA_MASTER]) >= sizeof(uint32_t)) {
		iface->master = *(uint32_t *)RTA_DATA(tb[IFLA_MASTER]);
	}
	if (tb[IFLA_LINK] && RTA_PAYLOAD(tb[IFLA_LINK]) >= sizeof(uint32_t)) {
		iface->has_link = 1;
		iface->link = *(uint32_t *)RTA_DATA(tb[IFLA_LINK]);
	}

	return 0;
}

static int add_vxlan(struct real_state *rs, struct ifinfomsg *ifi,
		     struct rtattr **tb)
{
	/* VXLAN берём из IFLA_LINKINFO, а не из имени интерфейса. */
	struct rtattr *li[IFLA_INFO_MAX + 1];
	struct rtattr *vx[IFLA_VXLAN_MAX + 1];
	struct vxlan *v;
	const char *kind;
	int len;

	if (!tb[IFLA_LINKINFO] || !tb[IFLA_IFNAME]) {
		return 0;
	}

	len = RTA_PAYLOAD(tb[IFLA_LINKINFO]);
	parse_rtattr(li, IFLA_INFO_MAX, RTA_DATA(tb[IFLA_LINKINFO]), len);
	if (!li[IFLA_INFO_KIND]) {
		return 0;
	}

	kind = RTA_DATA(li[IFLA_INFO_KIND]);
	if (strcmp(kind, "vxlan")) {
		return 0;
	}
	if (!li[IFLA_INFO_DATA]) {
		return 0;
	}
	if (rs->n_vxlan >= NR_VXLAN_MAX) {
		return nl_limit("vxlan interfaces", NR_VXLAN_MAX);
	}

	v = &rs->vxlan[rs->n_vxlan++];
	memset(v, 0, sizeof(*v));
	v->ifindex = ifi->ifi_index;
	snprintf(v->ifname, sizeof(v->ifname), "%s",
		 (char *)RTA_DATA(tb[IFLA_IFNAME]));

	len = RTA_PAYLOAD(li[IFLA_INFO_DATA]);
	parse_rtattr(vx, IFLA_VXLAN_MAX, RTA_DATA(li[IFLA_INFO_DATA]), len);

	if (vx[IFLA_VXLAN_ID] &&
	    RTA_PAYLOAD(vx[IFLA_VXLAN_ID]) >= sizeof(uint32_t)) {
		v->has_vni = 1;
		v->vni = *(uint32_t *)RTA_DATA(vx[IFLA_VXLAN_ID]);
	}
	if (vx[IFLA_VXLAN_GROUP] &&
	    RTA_PAYLOAD(vx[IFLA_VXLAN_GROUP]) >= sizeof(uint32_t)) {
		v->has_remote = 1;
		v->remote = *(uint32_t *)RTA_DATA(vx[IFLA_VXLAN_GROUP]);
	}
	if (vx[IFLA_VXLAN_LINK] &&
	    RTA_PAYLOAD(vx[IFLA_VXLAN_LINK]) >= sizeof(uint32_t)) {
		v->has_link = 1;
		v->link = *(uint32_t *)RTA_DATA(vx[IFLA_VXLAN_LINK]);
	}

	return 0;
}


static int add_vlan(struct real_state *rs, struct ifinfomsg *ifi,
		    struct rtattr **tb)
{
	/* VLAN id хранится во вложенном IFLA_INFO_DATA, parent link - в IFLA_LINK. */
	struct rtattr *li[IFLA_INFO_MAX + 1];
	struct rtattr *vl[IFLA_VLAN_MAX + 1];
	struct vlan *v;
	const char *kind;
	int len;

	if (!tb[IFLA_LINKINFO] || !tb[IFLA_IFNAME]) {
		return 0;
	}

	len = RTA_PAYLOAD(tb[IFLA_LINKINFO]);
	parse_rtattr(li, IFLA_INFO_MAX, RTA_DATA(tb[IFLA_LINKINFO]), len);
	if (!li[IFLA_INFO_KIND]) {
		return 0;
	}

	kind = RTA_DATA(li[IFLA_INFO_KIND]);
	if (strcmp(kind, "vlan")) {
		return 0;
	}
	if (!li[IFLA_INFO_DATA]) {
		return 0;
	}
	if (rs->n_vlan >= NR_VLAN_MAX) {
		return nl_limit("vlan interfaces", NR_VLAN_MAX);
	}

	v = &rs->vlan[rs->n_vlan++];
	memset(v, 0, sizeof(*v));
	v->ifindex = ifi->ifi_index;
	snprintf(v->ifname, sizeof(v->ifname), "%s",
		 (char *)RTA_DATA(tb[IFLA_IFNAME]));

	if (tb[IFLA_LINK] && RTA_PAYLOAD(tb[IFLA_LINK]) >= sizeof(uint32_t)) {
		v->has_link = 1;
		v->link = *(uint32_t *)RTA_DATA(tb[IFLA_LINK]);
	}

	len = RTA_PAYLOAD(li[IFLA_INFO_DATA]);
	parse_rtattr(vl, IFLA_VLAN_MAX, RTA_DATA(li[IFLA_INFO_DATA]), len);

	if (vl[IFLA_VLAN_ID] &&
	    RTA_PAYLOAD(vl[IFLA_VLAN_ID]) >= sizeof(uint16_t)) {
		v->has_id = 1;
		v->id = *(uint16_t *)RTA_DATA(vl[IFLA_VLAN_ID]);
	}

	return 0;
}

static int parse_link_msg(struct real_state *rs, struct nlmsghdr *nh)
{
	struct rtattr *tb[IFLA_MAX + 1];
	struct ifinfomsg *ifi;
	int len;
	int rc;

	if (nh->nlmsg_len < NLMSG_LENGTH(sizeof(*ifi))) {
		return -EIO;
	}

	ifi = NLMSG_DATA(nh);
	len = IFLA_PAYLOAD(nh);
	parse_rtattr(tb, IFLA_MAX, IFLA_RTA(ifi), len);

	rc = add_iface(rs, ifi, tb);
	if (rc) {
		return rc;
	}

	rc = add_vxlan(rs, ifi, tb);
	if (rc) {
		return rc;
	}

	return add_vlan(rs, ifi, tb);
}

static int parse_addr_msg(struct real_state *rs, struct nlmsghdr *nh)
{
	struct rtattr *tb[IFA_MAX + 1];
	struct ifaddrmsg *ifa;
	struct addr4 *a;
	struct rtattr *src;
	int len;

	if (nh->nlmsg_len < NLMSG_LENGTH(sizeof(*ifa))) {
		return -EIO;
	}

	ifa = NLMSG_DATA(nh);
	if (ifa->ifa_family != AF_INET) {
		return 0;
	}

	len = IFA_PAYLOAD(nh);
	parse_rtattr(tb, IFA_MAX, IFA_RTA(ifa), len);

	src = tb[IFA_LOCAL] ? tb[IFA_LOCAL] : tb[IFA_ADDRESS];
	if (!src || RTA_PAYLOAD(src) < sizeof(uint32_t)) {
		return 0;
	}
	if (rs->n_addr4 >= NR_ADDR4_MAX) {
		return nl_limit("IPv4 addresses", NR_ADDR4_MAX);
	}

	a = &rs->addr4[rs->n_addr4++];
	memset(a, 0, sizeof(*a));
	a->ifindex = ifa->ifa_index;
	a->prefix = ifa->ifa_prefixlen;
	a->addr = *(uint32_t *)RTA_DATA(src);

	return 0;
}

static int route_table(struct rtmsg *rtm, struct rtattr **tb)
{
	if (tb[RTA_TABLE] && RTA_PAYLOAD(tb[RTA_TABLE]) >= sizeof(uint32_t)) {
		return *(uint32_t *)RTA_DATA(tb[RTA_TABLE]);
	}

	return rtm->rtm_table;
}

static int parse_route_msg(struct real_state *rs, struct nlmsghdr *nh)
{
	/* MVP хранит только IPv4 routes из main table. */
	struct rtattr *tb[RTA_MAX + 1];
	struct rtmsg *rtm;
	struct route4 *r;
	int len;

	if (nh->nlmsg_len < NLMSG_LENGTH(sizeof(*rtm))) {
		return -EIO;
	}

	rtm = NLMSG_DATA(nh);
	if (rtm->rtm_family != AF_INET) {
		return 0;
	}
	if (rtm->rtm_type != RTN_UNICAST && rtm->rtm_type != RTN_LOCAL) {
		return 0;
	}

	len = RTM_PAYLOAD(nh);
	parse_rtattr(tb, RTA_MAX, RTM_RTA(rtm), len);
	if (route_table(rtm, tb) != RT_TABLE_MAIN) {
		return 0;
	}
	if (rs->n_route4 >= NR_ROUTE4_MAX) {
		return nl_limit("IPv4 routes", NR_ROUTE4_MAX);
	}

	r = &rs->route4[rs->n_route4++];
	memset(r, 0, sizeof(*r));
	r->prefix = rtm->rtm_dst_len;

	if (tb[RTA_DST] && RTA_PAYLOAD(tb[RTA_DST]) >= sizeof(uint32_t)) {
		r->dst = *(uint32_t *)RTA_DATA(tb[RTA_DST]);
	}
	if (tb[RTA_GATEWAY] &&
	    RTA_PAYLOAD(tb[RTA_GATEWAY]) >= sizeof(uint32_t)) {
		r->has_gw = 1;
		r->gw = *(uint32_t *)RTA_DATA(tb[RTA_GATEWAY]);
	}
	if (tb[RTA_OIF] && RTA_PAYLOAD(tb[RTA_OIF]) >= sizeof(uint32_t)) {
		r->oif = *(uint32_t *)RTA_DATA(tb[RTA_OIF]);
	}

	return 0;
}

static int recv_dump(int fd, uint32_t seq, struct real_state *rs, int type)
{
	/* Принимаем multipart dump до NLMSG_DONE с проверкой seq. */
	char buf[16384];
	struct nlmsghdr *nh;
	ssize_t len;
	int rem;
	int rc;

	for (;;) {
		len = recv(fd, buf, sizeof(buf), 0);
		if (len < 0) {
			if (errno == EINTR) {
				continue;
			}
			return -errno;
		}
		if (!len) {
			return -EIO;
		}

		rem = len;
		for (nh = (struct nlmsghdr *)buf; NLMSG_OK(nh, rem);
		     nh = NLMSG_NEXT(nh, rem)) {
			if (nh->nlmsg_seq != seq) {
				continue;
			}

			if (nh->nlmsg_type == NLMSG_DONE) {
				return 0;
			}
			if (nh->nlmsg_type == NLMSG_ERROR) {
				return nl_err(nh);
			}
			if (nh->nlmsg_type != type) {
				continue;
			}

			switch (type) {
			case RTM_NEWLINK:
				rc = parse_link_msg(rs, nh);
				break;
			case RTM_NEWADDR:
				rc = parse_addr_msg(rs, nh);
				break;
			case RTM_NEWROUTE:
				rc = parse_route_msg(rs, nh);
				break;
			default:
				rc = -EINVAL;
				break;
			}
			if (rc) {
				return rc;
			}
		}

		if (rem) {
			return -EIO;
		}
	}
}

static int dump_links(int fd, uint32_t *seq, struct real_state *rs)
{
	struct rtgenmsg msg;
	int rc;

	memset(&msg, 0, sizeof(msg));
	msg.rtgen_family = AF_UNSPEC;

	rc = nl_send(fd, RTM_GETLINK, &msg, sizeof(msg), ++(*seq));
	if (rc) {
		return rc;
	}

	return recv_dump(fd, *seq, rs, RTM_NEWLINK);
}

static int dump_addr4(int fd, uint32_t *seq, struct real_state *rs)
{
	struct ifaddrmsg msg;
	int rc;

	memset(&msg, 0, sizeof(msg));
	msg.ifa_family = AF_INET;

	rc = nl_send(fd, RTM_GETADDR, &msg, sizeof(msg), ++(*seq));
	if (rc) {
		return rc;
	}

	return recv_dump(fd, *seq, rs, RTM_NEWADDR);
}

static int dump_route4(int fd, uint32_t *seq, struct real_state *rs)
{
	struct rtmsg msg;
	int rc;

	memset(&msg, 0, sizeof(msg));
	msg.rtm_family = AF_INET;
	msg.rtm_table = RT_TABLE_MAIN;

	rc = nl_send(fd, RTM_GETROUTE, &msg, sizeof(msg), ++(*seq));
	if (rc) {
		return rc;
	}

	return recv_dump(fd, *seq, rs, RTM_NEWROUTE);
}

int nl_load_state(struct real_state *rs)
{
	/* Порядок важен: link нужен раньше addr/route для проверок по ifindex. */
	uint32_t seq = 0;
	int fd;
	int rc;

	memset(rs, 0, sizeof(*rs));

	fd = nl_open();
	if (fd < 0) {
		return fd;
	}

	rc = dump_links(fd, &seq, rs);
	if (rc) {
		goto out;
	}
	rc = dump_addr4(fd, &seq, rs);
	if (rc) {
		goto out;
	}
	rc = dump_route4(fd, &seq, rs);
	if (rc) {
		goto out;
	}

out:
	close(fd);
	return rc;
}
