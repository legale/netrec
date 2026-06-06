netrec
======

MVP reconcile utility for Linux/Debian.

Default mode is dry-run. It reads desired state from YAML, reads real state
from the kernel through rtnetlink, prints OK/MISS lines and ACT commands.
Without --apply it does not change the system.

Build:

	make

Run dry-run:

	./netrec -c examples/wg_vxlan_bridge.yaml

Run apply:

	./netrec --apply -c examples/wg_vxlan_bridge.yaml

Apply mode executes only ACT commands. Commands are executed with fork/execvp,
not through /bin/sh. The program still prints the same ACT lines, plus
APPLY_OK or APPLY_FAIL.

Supported scenarios:

only_uplink

	scenario: only_uplink
	uplink:
	  ifname: eth0

Checks:

	uplink exists
	uplink is up

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

Checks:

	bridge exists and type is bridge
	bridge has addr if addr is set
	each bridge.ports iface exists, is up, and has bridge as master
	uplink exists
	uplink is up
	uplink master is bridge

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

Checks:

	bridge exists and type is bridge
	bridge has addr if addr is set
	each bridge.ports iface exists, is up, and has bridge as master
	uplink exists
	uplink is up
	vlan iface exists and type is vlan
	vlan id matches
	vlan link matches uplink
	vlan is up
	vlan master is bridge

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
	  route_dev: br-lan
	vxlan:
	  ifname: vx100
	  vni: 100
	  remote: 10.20.30.40
	  dev: wg0
	  bridge: br-lan

Checks:

	bridge exists and type is bridge
	bridge has addr if addr is set
	each bridge.ports iface exists, is up, and has bridge as master
	uplink exists
	uplink is up
	exact route peer_ip/32 over route_dev exists
	wg iface exists
	wg iface is up
	vxlan iface exists and type is vxlan
	vxlan vni matches
	vxlan remote matches
	vxlan parent dev matches wg iface
	vxlan is up
	vxlan master is bridge

Limits:

	IPv4 only
	one object of each type
	bridge.ports supports 0..32 additional bridge member interfaces
	no daemon mode
	no UCI, JSON, Wi-Fi, firewall, DNS, DHCP, netifd integration
	WireGuard keys/peers are not checked

Static limits
-------------

real_state uses fixed arrays, not realloc. Current limits are in state.h:

    NR_IFACE_MAX 2048
    NR_ADDR4_MAX 8192
    NR_ROUTE4_MAX 32768
    NR_VXLAN_MAX 1024
    NR_VLAN_MAX 4096
    NR_BR_PORT_MAX 32

If a host exceeds a limit, netrec prints a specific internal-limit error instead
of a misleading generic ENOSPC message.
