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

echo "OK smoke_check"
