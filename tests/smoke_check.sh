#!/bin/sh
set -eu

cd "$(dirname "$0")/.."
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT INT TERM

run_cfg()
{
	cfg="$1"
	out="$tmp/out"

	if ./netrec -c "$cfg" >"$out" 2>&1; then
		rc=0
	else
		rc=$?
	fi

	case "$rc" in
	0|1) ;;
	*)
		echo "FAIL $cfg rc=$rc"
		cat "$out"
		exit 1
		;;
	esac

	grep -Eq '^(OK|MISS|ACT) ' "$out" || {
		echo "FAIL $cfg no verifier output"
		cat "$out"
		exit 1
	}
}

for f in examples/*.yaml; do
	run_cfg "$f"
done

if ./netrec --source uci --uci-network uci/uci.network \
	--uci-wireless uci/uci.wireless >"$tmp/out" 2>&1; then
	rc=0
else
	rc=$?
fi

case "$rc" in
0|1) ;;
*)
	echo "FAIL uci rc=$rc"
	cat "$tmp/out"
	exit 1
	;;
esac

grep -q 'bridge br-lan exists' "$tmp/out" || {
	echo "FAIL uci br-lan"
	cat "$tmp/out"
	exit 1
}
grep -q 'bridge br-mgmt exists' "$tmp/out" || {
	echo "FAIL uci br-mgmt"
	cat "$tmp/out"
	exit 1
}
grep -q 'route 100.100.2.1/32 dev br3_6' "$tmp/out" || {
	echo "FAIL uci wg route"
	cat "$tmp/out"
	exit 1
}
grep -q 'ip link add wan_vx3 type vxlan id 3 remote 100.100.2.1 dev wg1' \
	"$tmp/out" || {
	echo "FAIL uci vxlan"
	cat "$tmp/out"
	exit 1
}

./netrec -c examples/uplink_bridge_dhcp.yaml >"$tmp/out" 2>&1 || true
grep -q 'ACT udhcpc -i br-lan -q -n' "$tmp/out" || {
	echo "FAIL dhcp substitution"
	cat "$tmp/out"
	exit 1
}

./netrec -c examples/uplink_bridge_routes.yaml >"$tmp/out" 2>&1 || true
grep -q 'route 0.0.0.0/0 via 10.10.10.254 dev br-lan' "$tmp/out" || {
	echo "FAIL route output"
	cat "$tmp/out"
	exit 1
}

./netrec -c examples/uplink_bridge_static_full.yaml >"$tmp/out" 2>&1 || true
grep -q 'gateway 10.10.10.254 dev br-lan' "$tmp/out" || {
	echo "FAIL static gateway output"
	cat "$tmp/out"
	exit 1
}
grep -q 'dns 192.0.2.53 dev br-lan' "$tmp/out" || {
	echo "FAIL static dns output"
	cat "$tmp/out"
	exit 1
}

echo "OK smoke_check"
