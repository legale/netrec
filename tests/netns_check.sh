#!/bin/sh
set -eu

cd "$(dirname "$0")/.."

if ! command -v ip >/dev/null 2>&1; then
	echo "SKIP no ip"
	exit 0
fi

ns="nrtest_$$"
tmp="$(mktemp -d)"
bin="$PWD/netrec"

cleanup()
{
	ip netns del "$ns" >/dev/null 2>&1 || true
	rm -rf "$tmp"
}

fail()
{
	echo "FAIL $1"
	if [ -f "$tmp/out" ]; then
		cat "$tmp/out"
	fi
	exit 1
}

add_if()
{
	name="$1"
	id="$2"
	a="nr$${id}a"
	b="nr$${id}b"

	ip link add "$a" type veth peer name "$b"
	ip link set "$a" netns "$ns"
	ip link set "$b" netns "$ns"
	ip -n "$ns" link set "$a" name "$name"
	ip -n "$ns" link set "$b" name "${name}p"
	ip -n "$ns" link set "$name" up
	ip -n "$ns" link set "${name}p" up
}

trap cleanup EXIT INT TERM

if ! ip netns add "$ns" >/dev/null 2>&1; then
	echo "SKIP no netns permission"
	exit 0
fi

add_if eth0 0
add_if eth1 1
add_if eth2 2

if ! ip netns exec "$ns" "$bin" -c examples/only_uplink.yaml >"$tmp/out" 2>&1; then
	fail only_uplink
fi
grep -q "OK uplink eth0 exists" "$tmp/out" || fail only_uplink_output

if ! ip netns exec "$ns" "$bin" --apply -c examples/uplink_bridge.yaml >"$tmp/out" 2>&1; then
	fail apply_uplink_bridge
fi
grep -q "POST_VERIFY" "$tmp/out" || fail apply_post_verify

if ! ip netns exec "$ns" "$bin" -c examples/uplink_bridge.yaml >"$tmp/out" 2>&1; then
	fail uplink_bridge_ok
fi
grep -q "OK addr 10.10.10.1/24 dev br-lan" "$tmp/out" || fail uplink_bridge_addr

ip -n "$ns" route replace 0.0.0.0/0 via 10.10.10.254 dev br-lan
ip -n "$ns" route replace 1.2.3.4/32 dev br-lan
if ! ip netns exec "$ns" "$bin" -c examples/uplink_bridge_routes.yaml >"$tmp/out" 2>&1; then
	fail routes_ok
fi
grep -q "OK route 0.0.0.0/0 via 10.10.10.254 dev br-lan" "$tmp/out" || fail route_default

grep -q "OK route 1.2.3.4/32 dev br-lan" "$tmp/out" || fail route_host

ip -n "$ns" addr flush dev br-lan
if ip netns exec "$ns" "$bin" -c examples/uplink_bridge_dhcp.yaml >"$tmp/out" 2>&1; then
	fail dhcp_should_miss
fi
grep -q "ACT udhcpc -i br-lan -q -n" "$tmp/out" || fail dhcp_action

echo "OK netns_check"
