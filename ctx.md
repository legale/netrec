# netrec context

Дата: 2026-06-07
Каталог проекта: /mnt/data/netrec

## Назначение ctx.md

Этот файл отражает текущее состояние кода netrec.

Его нужно обновлять всякий раз, когда изменяется код, сценарии, YAML schema,
формат вывода, поведение --apply, ограничения, build/test команды или план
следующих работ.

ctx.md является рабочим контекстом для продолжения разработки. Если README.md,
код и ctx.md расходятся, сначала нужно выровнять ctx.md по фактическому коду,
а потом продолжать изменения.

## Текущая цель проекта

netrec - one-shot reconciler для Linux/Debian/OpenWrt-like систем.

Задача:

    читать desired network state из YAML
    снимать real state из ядра через rtnetlink
    сравнивать desired_state и real_state
    печатать стабильный diff в формате OK/MISS/ACT
    по --apply выполнять заранее сформированные repair actions
    после --apply повторно снять real_state и убедиться, что state сошелся

netrec не является daemon и не содержит event loop. Внешний watcher, cron,
systemd/openrc/procd hook или IvanDriver могут запускать netrec при событии.
Событие не должно содержать бизнес-логику ремонта. Событие только причина
заново снять полный kernel snapshot и прогнать verifier.

## Текущая команда запуска

Dry-run по умолчанию:

    ./netrec -c config.yaml

Apply:

    ./netrec --apply -c config.yaml
    ./netrec -a -c config.yaml

Return code:

    0 - diff нет, apply failures нет, post-apply verify сошелся
    1 - diff есть, apply command failed, YAML/netlink/internal error
    2 - ошибка аргументов командной строки

## Текущая реализация

Файлы верхнего уровня:

    main.c
    yaml.c / yaml.h
    state.c / state.h
    netlink.c / netlink.h
    verify.c / verify.h
    Makefile
    examples/*.yaml
    tests/*.sh
    README.md
    ctx.md

Поток выполнения:

    main.c
        parse argv
        -c config.yaml required
        default mode is dry-run
        --apply / -a enables execution of ACT commands
        for --apply acquire /tmp/netrec.lock with flock LOCK_EX | LOCK_NB
        load desired_set
        load real_state through rtnetlink
        run verifier for each desired_state from the set over one real_state snapshot
        if --apply and actions succeeded and diff existed:
            print POST_VERIFY
            reload real_state
            run verifier for the same desired_set again in dry-run mode
            return 0 only if no MISS remains

    yaml.c
        yaml_load_desired_set()
        yaml_load_desired()
        read YAML through libyaml
        require root mapping
        read scenario first
        current YAML path fills desired_set with one flat desired_state
        validate only relations needed by selected scenario
        no generic config graph
        optional routes[] can be present for every scenario

    netlink.c
        nl_load_state()
        open NETLINK_ROUTE socket
        RTM_GETLINK dump
            fill iface list
            extract kind, carrier, master, link
            extract vxlan attrs from IFLA_LINKINFO
            extract vlan attrs from IFLA_LINKINFO
        RTM_GETADDR dump
            fill IPv4 addresses only
        RTM_GETROUTE dump
            fill IPv4 main-table routes only
        no system modification here

    state.c
        linear lookup helpers over fixed arrays
        no hash tables
        no dynamic allocation
        rs_load_resolv_conf()
        read /etc/resolv.conf nameserver IPv4 lines into real_state
        DNS is global user-space state, not kernel state

    verify.c
        verify_state()
        explicit dispatch by scenario
        compare desired_state against real_state
        print stable OK/MISS lines
        print ACT line for each missing/wrong item
        in dry-run stop there
        in --apply execute ACT command through fork/execvp/waitpid
        no shell
        no system()
        no rollback
        reports apply failure through act_fail output parameter

## Desired state

Desired state ядра verifier теперь подается как fixed-size desired_set.

Сейчас YAML через libyaml по-прежнему поддержан, но пока заполняет только
один элемент desired_set. Это оставляет verifier независимым от будущего UCI
adapter и не ломает текущий `make check`.

Структура фиксированная и плоская:

    struct desired_set
        state[]
        n_state

    struct desired_state
        scenario
        bridge fields, including static addr/gateway/dns
        uplink fields
        vlan fields
        wg fields
        vxlan fields
        routes[]

Нет generic DAG, registry, callback framework, hashmap, realloc.

## Supported scenarios

### only_uplink

YAML:

    scenario: only_uplink
    uplink:
      ifname: eth0

Alias:

    only_uplink_iface

Checks:

    uplink iface exists
    uplink iface is up
    optional routes[]

Actions:

    missing iface: ACT # provide uplink iface <ifname>
    down iface: ip link set <ifname> up

### uplink_bridge

YAML:

    scenario: uplink_bridge
    bridge:
      name: br-lan
      addr: 10.10.10.1/24
      ports:
        - eth1
        - eth2
    uplink:
      ifname: eth0

Alias:

    uplink_in_bridge

Checks:

    bridge exists
    bridge type is bridge
    bridge is up
    bridge address mode is satisfied
    static bridge gateway is satisfied if configured
    static bridge DNS is present in /etc/resolv.conf if configured
    uplink exists
    uplink is up
    uplink master is bridge
    each bridge.ports iface exists
    each bridge.ports iface is up
    each bridge.ports iface master is bridge
    optional routes[]

Actions:

    missing bridge: ip link add <br> type bridge; ip link set <br> up
    wrong bridge type: ip link del <br>; ip link add <br> type bridge; ip link set <br> up
    bridge down: ip link set <br> up
    missing static addr: ip addr add <addr> dev <br>
    missing static gateway: ip route replace 0.0.0.0/0 via <gateway> dev <br>
    missing static DNS: ACT # set dns <dns> dev <br>
    missing dhcp addr: expanded bridge.dhcp_cmd
    iface down: ip link set <ifname> up
    wrong master: ip link set <ifname> master <br>

### vlan_bridge

YAML:

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

Alias:

    vlan_in_bridge

Rules:

    vlan.link must equal uplink.ifname
    vlan.bridge must equal bridge.name
    if vlan.link absent, it defaults to uplink.ifname
    if vlan.bridge absent, it defaults to bridge.name

Checks/actions:

    all uplink_bridge checks/actions except uplink master is not forced
    vlan iface exists and type is vlan
    vlan id matches
    vlan link matches desired link
    vlan is up
    vlan master is bridge
    optional routes[]

VLAN id/link are treated as immutable. If wrong, verifier emits recreate actions:

    ip link del <vlan>
    ip link add link <link> name <vlan> type vlan id <id>
    ip link set <vlan> up
    ip link set <vlan> master <bridge>

### wg_vxlan_bridge

YAML:

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

Rules:

    wg.route_dev is explicit and can differ from bridge.name
    vxlan.dev is optional; if set, it must equal wg.ifname
    vxlan.bridge must equal bridge.name
    uplink section is optional

Checks/actions:

    all uplink_bridge checks/actions if uplink is configured
    exact route wg.peer_ip/32 dev wg.route_dev exists
    wg iface exists
    wg iface is up
    vxlan iface exists and type is vxlan
    vxlan vni matches
    vxlan remote matches
    optional vxlan parent dev matches wg iface
    vxlan is up
    vxlan master is bridge
    optional routes[]

VXLAN vni/remote/dev are treated as immutable. If wrong, verifier emits recreate
actions:

    ip link del <vxlan>
    ip link add <vxlan> type vxlan id <vni> remote <remote> [dev <dev>]
    ip link set <vxlan> up
    ip link set <vxlan> master <bridge>

WireGuard keys and peers are not configured yet. wg iface creation is only:

    ip link add <wg> type wireguard

## Bridge address model

bridge.addr_mode values:

    absent
    none
    static
    dhcp

Rules:

    addr_mode absent + addr set => static
    addr_mode absent + addr absent => none
    addr_mode static requires bridge.addr
    addr_mode none forbids bridge.addr/gateway/dns
    addr_mode dhcp requires bridge.dhcp_cmd and forbids bridge.addr/gateway/dns

Static mode check:

    exact IPv4 addr/prefix exists on bridge iface
    optional bridge.gateway exists as default IPv4 main-table route via bridge
    optional bridge.dns entries exist in /etc/resolv.conf nameserver IPv4 lines

Static mode action:

    ip addr add <addr> dev <bridge>
    ip route replace 0.0.0.0/0 via <gateway> dev <bridge>
    ACT # set dns <dns> dev <bridge>

DNS apply is intentionally not implemented yet. DNS is not kernel state and is
not safely per-interface in generic Linux. Current DNS support is parse + verify
against global /etc/resolv.conf + non-executable ACT comment. WDA integration
must decide how to apply DNS on OpenWrt: UCI/netifd, resolv.conf.d, dnsmasq, or
an explicit small helper.

Static example:

    bridge:
      name: br-lan
      addr_mode: static
      addr: 10.10.10.1/24
      gateway: 10.10.10.254
      dns:
        - 192.0.2.53
        - 192.0.2.54

DHCP mode check:

    any IPv4 address exists on bridge iface

DHCP mode action:

    bridge.dhcp_cmd after replacing literal $iface by bridge.name

Example:

    bridge:
      name: br-lan
      addr_mode: dhcp
      dhcp_cmd: udhcpc -i $iface -q -n

No shell is used. Command string is split by spaces/tabs and executed through
execvp. Quotes are not supported.

## Optional routes[]

routes[] is optional for every scenario.

YAML:

    routes:
      - dst: 0.0.0.0/0
        via: 10.10.10.254
        dev: br-lan
      - dst: 1.2.3.4/32
        dev: br-lan

Rules:

    dst is required, IPv4 prefix string
    dev is required
    via is optional IPv4 address
    table is always main
    IPv6 is not supported
    route count limit is NR_DES_ROUTE4_MAX

Check:

    exact dst/prefix + dev + optional via in real_state route dump

Action:

    ip route replace <dst> via <via> dev <dev>
    ip route replace <dst> dev <dev>

If dev does not exist in the old snapshot, verifier still emits the route repair
command. This allows one --apply run to create bridge/dev first and then add the
route before POST_VERIFY.

## Current examples

    examples/only_uplink.yaml
    examples/uplink_bridge.yaml
    examples/uplink_bridge_dhcp.yaml
    examples/uplink_bridge_routes.yaml
    examples/uplink_bridge_static_full.yaml
    examples/vlan_bridge.yaml
    examples/wg_vxlan_bridge.yaml

## Build and tests

Build:

    make

Clean:

    make clean

Fast regression:

    make check

make check runs:

    tests/smoke_check.sh
        runs all examples on current host
        accepts rc 0 or 1
        fails on rc outside 0/1
        checks DHCP $iface substitution output
        checks routes output
        checks static gateway/DNS output

    tests/netns_check.sh
        creates isolated network namespace when permissions allow it
        creates veth ifaces eth0/eth1/eth2
        tests only_uplink OK
        tests --apply uplink_bridge from missing bridge
        requires POST_VERIFY in apply output
        tests dry-run uplink_bridge after apply
        tests optional routes[] OK
        tests DHCP dry-run ACT command
        skips cleanly if netns permission is unavailable

Current container result on 2026-06-06:

    make check
    OK smoke_check
    SKIP no netns permission

Current result after desired_set prep on 2026-06-07:

    make check
    rc=0

Current UCI scope on 2026-06-07:

    file-based UCI input adapter exists
    input is uci show network + uci show wireless dumps
    adapter fills desired_set and keeps verifier UCI-agnostic
    current bridge coverage:
        static bridge -> uplink_bridge
        dhcp bridge -> uplink_bridge
        proto none bridge with vxlan port + matching wireguard peer -> wg_vxlan_bridge
    wireless wifi-iface entries are added to bridge.ports by matching wireless.network
    live uci execution is not added yet

## Apply behavior

Dry-run:

    prints OK/MISS/ACT
    does not execute ACT
    returns 0 if no MISS
    returns 1 if MISS exists

Apply:

    obtains /tmp/netrec.lock
    if lock is already held, prints "apply: another netrec apply is running"
    executes non-comment ACT commands
    comment ACT commands start with '#', are printed, but not executed
    prints APPLY_OK for successful command
    prints APPLY_FAIL rc=<rc> cmd=<cmd> for failed command
    if any command failed, returns 1 and does not claim convergence
    if commands were needed and succeeded, prints POST_VERIFY
    reloads real_state from kernel
    runs verifier in dry-run mode
    returns 0 only if post-apply verify has no MISS

No rollback exists. This is intentional for current stage.

## Static limits

Defined in state.h:

    NR_IFACE_MAX        2048
    NR_ADDR4_MAX        8192
    NR_ROUTE4_MAX       32768
    NR_VXLAN_MAX        1024
    NR_VLAN_MAX         4096
    NR_BR_PORT_MAX      32
    NR_DES_ROUTE4_MAX   64
    NR_DES_DNS4_MAX     4
    NR_DNS4_MAX         16

On overflow, netlink/yaml loaders return a specific error instead of silently
truncating state.

## Current non-goals

    daemon mode
    event loop
    UCI parser
    JSON parser
    firewall
    DNS apply
    Wi-Fi
    netifd integration
    WireGuard key/peer config
    IPv6
    dynamic allocation
    generic graph solver
    deletion of extra addresses/routes/ports
    rollback

## Important design choices

One-shot reconciler is intentional. External event systems should not encode
repair logic. They should only trigger netrec.

Verifier owns the repair plan. The same verifier prints dry-run ACT and executes
those ACT commands in --apply. This keeps dry-run and apply behavior aligned.

real_state is a full kernel snapshot, not event delta. This is the main reliability
property.

Fixed arrays are intentional. Limits are explicit and fail closed.

No shell is used for repair actions. This limits quoting flexibility but removes
shell injection and quoting ambiguity.

## Current risks / known limits

Command splitter supports only spaces/tabs. Quoting is not supported.

DHCP mode only checks that some IPv4 address exists on the bridge. It does not
validate lease source, gateway, DNS, lease time, or DHCP server identity.

DNS check is global /etc/resolv.conf, not true per-interface state. Missing DNS
causes MISS and comment ACT, but --apply will not fix it. If DNS remains missing,
post-apply verify fails. This is fail-closed until WDA/OpenWrt DNS apply policy is
chosen.

Static addr action uses ip addr add. Extra old addresses are not removed.

Routes are checked as exact routes in main table only. Extra wrong routes are not
removed.

Route dst should be canonical network prefix. Non-canonical input can fail exact
match after kernel normalization.

WireGuard interface creation may fail if wireguard support is unavailable.

VXLAN/VLAN creation depends on kernel support and iproute2 support.

## Next minimal work

    add UCI input adapter that fills desired_set but does not leak UCI into verifier
    add route dst canonicalization or reject non-canonical dst
    add explicit command length overflow detection in act()
    add tests for YAML validation failures
    add netns route/apply tests on a host with CAP_NET_ADMIN
    decide whether static addr should use ip addr replace/flush policy
    decide whether to support extra route deletion later


## Внедрение в WDA

Цель первого внедрения - использовать netrec как внешний safety reconciler рядом
с wda, а не переписывать wda и не встраивать netrec как библиотеку.

Граница ответственности:

    wda получает событие или видит подозрение на рассинхрон
    wda ставит reconcile_needed
    debounce timer запускает netrec
    netrec сам читает desired YAML
    netrec сам снимает полный real_state
    netrec сам печатает OK/MISS/ACT
    netrec сам делает --apply и POST_VERIFY, если режим apply включен
    wda только логирует rc/output и обновляет метрики

Не запускать netrec на каждый netlink/ubus event напрямую. Нужен debounce, иначе
будет storm и ложные гонки между netifd, kernel и netrec.

Минимальный режим запуска из wda:

    off       - не запускать netrec
    dry_run   - запускать без --apply, только логировать MISS/ACT
    apply     - запускать --apply только после dry-run пилота

Первый production path:

    wda не трогает verifier internals
    netrec получает UCI source mode и сам строит desired_set
    wda запускает /usr/sbin/netrec --source uci
    stdout/stderr сохраняется коротким хвостом в лог/статус
    rc=0 значит state OK
    rc=1 в dry-run значит найден diff или ошибка, текст вывода обязателен
    apply запускать только через feature flag

Метрики в wda:

    last_netrec_rc
    last_netrec_ms
    last_netrec_ok_ts
    last_netrec_fail_ts
    last_netrec_fail_cnt
    last_netrec_act_cnt
    last_netrec_msg или хвост вывода

Безопасный apply allowlist на первом этапе:

    ip link set <if> up
    ip link set <if> master <bridge>
    ip link add <bridge> type bridge
    ip addr add <addr> dev <bridge>
    ip route replace ... dev <bridge>
    bridge member add через ip link set master

Не включать destructive repair без отдельного решения:

    delete чужие addr
    delete чужие route
    delete лишние bridge ports
    flush iface
    network restart
    DNS запись в системные файлы

Что нужно до включения apply в wda:

    собрать netrec в OpenWrt SDK под musl
    проверить запуск на точке
    сделать uci/runtime -> desired YAML generator
    прогнать dry-run 1-2 дня на реальных конфигурациях
    проверить, что после ручной поломки bridge/addr/route netrec дает правильный ACT
    отдельно решить OpenWrt DNS apply policy

Для static interface generator должен уметь писать:

    bridge.addr_mode: static
    bridge.addr: <ip>/<prefix>
    bridge.gateway: <gw>, если задан gateway
    bridge.dns: list IPv4 DNS, если задан DNS

Для DHCP interface generator должен писать:

    bridge.addr_mode: dhcp
    bridge.dhcp_cmd: команда с $iface

Важно: bridge.gateway и bridge.dns сейчас разрешены только для static mode.
