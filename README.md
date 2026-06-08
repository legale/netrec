netrec
======

One-shot reconcile utility for Linux/Debian/OpenWrt-like systems.

Default mode is dry-run. netrec reads desired state from YAML or from UCI dump
files, reads real state from the kernel through rtnetlink, prints OK/MISS lines
and ACT commands. Without --apply it does not change the system.

YAML input can be either one scenario mapping or a top-level sequence of
scenario mappings. UCI input is converted to YAML first and then goes through
the same YAML loader.

Build:

	make

Fast check:

	make check

Run dry-run:

	./netrec -c examples/wg_vxlan_bridge.yaml

Run UCI fixture dry-run:

	./netrec --source uci --uci-network uci/uci.network \
		--uci-wireless uci/uci.wireless

Run equivalent multi-scenario YAML fixture:

	./netrec -c examples/uci_fixture.yaml

Internal UCI conversion:

	uci2yml(network_dump, wireless_dump, yaml_path)

Run apply:

	./netrec --apply -c examples/wg_vxlan_bridge.yaml

Apply mode:

	uses /tmp/netrec.lock and fails if another apply is running
	executes ACT commands with fork/execvp, not through /bin/sh
	prints APPLY_OK or APPLY_FAIL for executed commands
	after successful ACT execution reloads kernel state and runs dry-run verify
	returns 0 only if post-apply verify has no MISS

Supported scenarios:

only_uplink

	scenario: only_uplink
	uplink:
	  ifname: eth0

Checks:

	uplink exists
	uplink is up
	optional routes[]

uplink_bridge

	scenario: uplink_bridge
	bridge:
	  name: br-lan
	  addr: 10.10.10.1/24
	  ports:
	    - eth1
	    - eth2
	uplink:
	  ifname: eth0

Checks/actions:

	bridge exists and type is bridge
	bridge is up
	bridge has static IPv4 address if addr/addr_mode static is set
	bridge has default gateway if static gateway is set
	bridge DNS entries are present in /etc/resolv.conf if static dns is set
	bridge has any IPv4 address if addr_mode dhcp is set
	each bridge.ports iface exists, is up, and has bridge as master
	uplink exists
	uplink is up
	uplink master is bridge
	optional routes[]

Bridge address modes:

	addr_mode absent + addr set means static
	addr_mode absent + addr absent means none
	addr_mode static requires addr
	addr_mode dhcp requires dhcp_cmd and forbids addr/gateway/dns
	addr_mode none forbids addr/gateway/dns

Static interface options:

	bridge.gateway is optional IPv4 default gateway
	bridge.dns is optional IPv4 nameserver list, max 4
	gateway/dns require static addr_mode

Static example:

	bridge:
	  name: br-lan
	  addr_mode: static
	  addr: 10.10.10.1/24
	  gateway: 10.10.10.254
	  dns:
	    - 192.0.2.53
	    - 192.0.2.54

Gateway is checked as IPv4 main-table default route over bridge.name and repaired with:

	ip route replace 0.0.0.0/0 via <gateway> dev <bridge>

DNS is checked against global /etc/resolv.conf nameserver IPv4 lines. DNS is
not kernel-state and is not safely interface-scoped here, so apply only prints a
comment ACT for missing DNS:

	ACT # set dns <dns> dev <bridge>

DHCP example:

	bridge:
	  name: br-lan
	  addr_mode: dhcp
	  dhcp_cmd: udhcpc -i $iface -q -n

$iface is replaced by bridge.name before execvp. No shell is used.

vlan_bridge

	scenario: vlan_bridge
	bridge:
	  name: br-lan
	  addr: 10.10.10.1/24
	  ports:
	    - eth1
	    - eth2
	uplink:
	  ifname: eth0
	vlan:
	  ifname: eth0.100
	  id: 100
	  link: eth0
	  bridge: br-lan

Checks/actions:

	bridge checks from uplink_bridge
	uplink checks from uplink_bridge
	vlan iface exists and type is vlan
	vlan id matches
	vlan link matches uplink
	vlan is up
	vlan master is bridge
	optional routes[]

wg_vxlan_bridge

	scenario: wg_vxlan_bridge
	bridge:
	  name: br-lan
	  addr: 10.10.10.1/24
	  ports:
	    - eth1
	    - eth2
	uplink:
	  ifname: eth0
	wg:
	  ifname: wg0
	  peer_ip: 1.2.3.4
	  route_dev: wg0
	vxlan:
	  ifname: vx100
	  vni: 100
	  remote: 10.20.30.40
	  dev: wg0
	  bridge: br-lan

Checks/actions:

	bridge checks from uplink_bridge
	uplink checks from uplink_bridge if uplink is configured
	exact route peer_ip/32 over route_dev exists
	wg iface exists
	wg iface is up
	vxlan iface exists and type is vxlan
	vxlan vni matches
	vxlan remote matches
	optional vxlan parent dev matches wg iface
	vxlan is up
	vxlan master is bridge
	optional routes[]

`vxlan.dev` is optional. If it is omitted, verifier does not require a parent
link and `ip link add ... type vxlan` is emitted without `dev`.

Optional routes:

	routes:
	  - dst: 0.0.0.0/0
	    via: 10.10.10.254
	    dev: br-lan
	  - dst: 1.2.3.4/32
	    dev: br-lan

Routes are IPv4 main-table routes. dst is required. dev is required. via is
optional. Missing routes are repaired with ip route replace.

Limits:

	IPv4 only
	one bridge/uplink/vlan/wg/vxlan object per scenario
	bridge.ports supports 0..32 additional bridge member interfaces
	routes supports 0..64 IPv4 routes
	no daemon mode
	YAML input supports one scenario or a top-level scenario sequence
	UCI desired_set path is UCI dump -> YAML file -> YAML loader
	UCI input adapter currently covers bridge/static, bridge/dhcp,
	bridge+wg+vxlan and wireless bridge members
	no JSON, firewall, netifd integration
	DNS apply is not implemented; only /etc/resolv.conf check exists
	WireGuard keys/peers are not checked
	DHCP success is detected only as any IPv4 address on the bridge iface
	DNS check is global /etc/resolv.conf, not true per-interface DNS state

Static limits are in state.h:

	NR_IFACE_MAX 2048
	NR_ADDR4_MAX 8192
	NR_ROUTE4_MAX 32768
	NR_VXLAN_MAX 1024
	NR_VLAN_MAX 4096
	NR_BR_PORT_MAX 32
	NR_DES_ROUTE4_MAX 64
	NR_DES_DNS4_MAX 4
	NR_DNS4_MAX 16

If a host exceeds a limit, netrec prints a specific internal-limit error instead
of a misleading generic ENOSPC message.
