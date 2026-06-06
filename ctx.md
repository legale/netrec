# netrec context

Дата: 2026-06-06
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

    0 - diff нет и apply failures нет
    1 - diff есть или apply command failed
    2 - ошибка запуска, YAML, netlink или internal error

## Текущая реализация

Файлы:

    main.c
    yaml.c / yaml.h
    state.c / state.h
    netlink.c / netlink.h
    verify.c / verify.h
    Makefile
    examples/*.yaml
    README.md
    ctx.md

Поток выполнения:

    main.c
        parse argv
        -c config.yaml required
        default mode is dry-run
        --apply / -a enables execution of ACT commands

    yaml.c
        yaml_load_desired()
        read YAML through libyaml
        require root mapping
        read scenario first
        fill one flat struct desired_state
        validate only relations needed by selected scenario
        no generic config graph

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

## Desired state

Desired state читается из YAML через libyaml.

Структура фиксированная и плоская:

    struct desired_state

Поля сейчас:

    scenario
    bridge.name
    bridge.addr
    bridge.ports[]
    uplink.ifname
    vlan.ifname
    vlan.id
    vlan.link
    vlan.bridge
    wg.ifname
    wg.peer_ip
    wg.route_dev
    vxlan.ifname
    vxlan.vni
    vxlan.remote
    vxlan.dev
    vxlan.bridge

Ограничения:

    только IPv4
    один scenario на запуск
    один bridge
    один uplink
    один vlan
    один wg
    один vxlan
    bridge.ports: от 0 до 32 дополнительных member-интерфейсов

## Real state

Real state снимается из ядра через rtnetlink.

Сейчас собирается:

Interfaces:

    name
    ifindex
    kind
    flags IFF_UP
    carrier, если доступно
    master ifindex
    link ifindex

IPv4 addresses:

    ifindex
    addr/prefix

IPv4 routes:

    dst/prefix
    gateway, если задан
    oif

VXLAN:

    ifname
    ifindex
    vni
    remote
    link/dev ifindex

VLAN:

    ifname
    ifindex
    vlan id
    link ifindex

## Fixed limits

real_state использует fixed arrays намеренно. realloc сейчас не используется.

Лимиты в state.h:

    NR_IFACE_MAX 2048
    NR_ADDR4_MAX 8192
    NR_ROUTE4_MAX 32768
    NR_VXLAN_MAX 1024
    NR_VLAN_MAX 4096
    NR_BR_PORT_MAX 32

Если netlink dump превышает лимит, netrec должен печатать понятную ошибку
internal-limit, например:

    netlink: failed: too many IPv4 routes in netlink dump, max=32768

Нельзя возвращать простой strerror(ENOSPC), потому что это выглядит как ошибка
дискового места, а не лимит массива netrec.

## Поддерживаемые scenarios

### only_uplink

Alias:

    only_uplink_iface

YAML:

    scenario: only_uplink
    uplink:
      ifname: eth0

Checks:

    uplink exists
    uplink is up

Actions:

    ip link set <uplink> up

Если physical uplink отсутствует, netrec не создает его и печатает только
comment action:

    ACT # provide uplink iface <ifname>

### uplink_bridge

Alias:

    uplink_in_bridge

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

Checks:

    bridge exists and kind is bridge
    bridge has expected addr, if addr is set
    uplink exists
    uplink is up
    uplink master is bridge
    each bridge.ports iface exists
    each bridge.ports iface is up
    each bridge.ports iface master is bridge

Actions:

    ip link add <bridge> type bridge
    ip addr add <addr> dev <bridge>
    ip link set <uplink> up
    ip link set <uplink> master <bridge>
    ip link set <port> up
    ip link set <port> master <bridge>

Если bridge port отсутствует, netrec не создает его и печатает comment action:

    ACT # provide bridge port iface <ifname>

### vlan_bridge

Alias:

    vlan_in_bridge

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

Checks:

    bridge exists and kind is bridge
    bridge has expected addr, if addr is set
    uplink exists
    uplink is up
    vlan iface exists and kind is vlan
    vlan id matches
    vlan link matches uplink
    vlan is up
    vlan master is bridge
    each bridge.ports iface exists
    each bridge.ports iface is up
    each bridge.ports iface master is bridge

Actions:

    ip link add <bridge> type bridge
    ip addr add <addr> dev <bridge>
    ip link set <uplink> up
    ip link add link <link> name <vlan> type vlan id <id>
    ip link set <vlan> up
    ip link set <vlan> master <bridge>
    ip link set <port> up
    ip link set <port> master <bridge>

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
      route_dev: br-lan
    vxlan:
      ifname: vx100
      vni: 100
      remote: 10.20.30.40
      dev: wg0
      bridge: br-lan

Checks:

    bridge exists and kind is bridge
    bridge has expected addr, if addr is set
    uplink exists
    uplink is up
    exact route wg.peer_ip/32 over wg.route_dev exists
    wg iface exists
    wg iface is up
    vxlan iface exists and kind is vxlan
    vxlan vni matches
    vxlan remote matches
    vxlan parent dev matches wg.ifname
    vxlan is up
    vxlan master is bridge
    each bridge.ports iface exists
    each bridge.ports iface is up
    each bridge.ports iface master is bridge

Actions:

    ip link add <bridge> type bridge
    ip addr add <addr> dev <bridge>
    ip link set <uplink> up
    ip route add <peer_ip>/32 dev <route_dev>
    ip link add <wg> type wireguard
    ip link add <vxlan> type vxlan id <vni> remote <remote> dev <wg>
    ip link set <wg> up
    ip link set <vxlan> up
    ip link set <vxlan> master <bridge>
    ip link set <port> up
    ip link set <port> master <bridge>

WireGuard keys/peers сейчас не проверяются.

## Формат вывода

Формат должен оставаться стабильным для grep/tests:

    OK bridge br-lan exists
    MISS addr 10.10.10.1/24 dev br-lan
    ACT ip addr add 10.10.10.1/24 dev br-lan

    MISS route 1.2.3.4/32 dev br-lan
    ACT ip route add 1.2.3.4/32 dev br-lan

    MISS iface wg0
    ACT ip link add wg0 type wireguard

    MISS iface vx100
    ACT ip link add vx100 type vxlan id 100 remote 10.20.30.40 dev wg0
    ACT ip link set vx100 up
    ACT ip link set vx100 master br-lan

Verifier должен выводить все diff, не останавливаться на первом.

## Apply mode

Apply mode включается только явно:

    --apply
    -a

Default mode remains dry-run.

Поведение --apply сейчас:

    печатает те же ACT lines, что dry-run
    выполняет ACT commands сразу
    печатает APPLY_OK cmd=<cmd> при успехе
    печатает APPLY_FAIL rc=<rc> cmd=<cmd> при ошибке
    не выполняет ACT comment lines starting with '#'

Реализация:

    no /bin/sh
    no system()
    fork()
    execvp()
    waitpid()
    command string is split into argv by spaces/tabs

Ограничение осознанное: текущие generated commands не требуют quoting.
Это снижает риск shell injection от YAML values.

Текущие ограничения apply:

    no transaction rollback
    no lock yet
    no post-apply real_state reload yet
    no verify-after-apply yet
    real_state is captured once before actions
    run netrec again after --apply to verify final state

## Dependencies

libyaml-master.zip предоставлен пользователем.

libyaml разложен внутри проекта:

    netrec/deps/libyaml

Makefile собирает libyaml локально из deps, без системного libyaml-dev.

libnl-tiny-master.zip тоже был предоставлен пользователем, но сейчас не используется.
Текущая реализация использует прямой rtnetlink без libnl-tiny, потому что это
меньше слоев и проще отлаживать. libnl-tiny можно рассмотреть позже только если
он реально уменьшит код и риск.

## Build/test commands

Основная проверка:

    cd /mnt/data/netrec
    make clean
    make
    for f in examples/*.yaml; do ./netrec -c "$f" || true; done

Безопасная apply-проверка в текущем контейнере:

    ./netrec --apply -c examples/only_uplink.yaml

Она безопасна, если eth0 уже существует и up, потому что ACT command не
выполняется.

Архив проекта:

    cd /mnt/data
    tar -cjf netrec<idx>.tar.bz2 netrec

## Что сейчас НЕ поддержано

    daemon mode
    event loop
    UCI
    JSON
    Wi-Fi
    firewall
    DNS/DHCP
    netifd integration
    WireGuard keys/peers check
    multiple bridge objects
    multiple vlan objects
    multiple vxlan objects
    multiple wg objects
    multiple routes from YAML
    address mode static/dhcp schema
    deletion of extra addresses/routes/bridge ports
    policy routing
    route metrics
    multiple routing tables
    rollback for --apply

## Production-ready target

Итоговая цель - production-ready netrec с простой моделью без generic dependency
graph.

Требования:

    сохранять one-shot model
    сохранять fixed arrays, пока лимитов достаточно
    сохранять явные scenarios и линейный verifier
    иметь lock для --apply
    после --apply делать новый netlink snapshot и повторный dry-run verify
    возвращать ошибку, если после apply состояние не сошлось
    иметь понятные YAML errors до netlink/apply
    проверять kind интерфейса: bridge/vlan/vxlan/wireguard, где возможно
    не использовать shell для обычных ip actions
    поддержать shell/template command только там, где это явно задано config

## Address model target

Нужно поддержать два режима адресации.

Static address:

    iface:
      ifname: br-lan
      addr_mode: static
      addr: 10.10.10.1/24

Проверка:

    addr присутствует на интерфейсе
    если addr отсутствует, ACT ip addr add <addr> dev <ifname>

DHCP address:

    iface:
      ifname: br-wan
      addr_mode: dhcp
      dhcp_cmd: "udhcpc -i $iface -q -n"

Проверка MVP:

    iface exists
    iface up
    есть хотя бы один IPv4 address на iface
    если адреса нет, ACT показывает DHCP command с заменой $iface

Ограничение:

    поддержать только $iface -> interface name
    не добавлять generic template engine
    отдельно решить, допускается ли shell для dhcp_cmd
    если shell нужен, это должно быть явно видно в коде и документации

## Multiple routes target

Маршруты должны стать множественными.

YAML target:

    routes:
      - dst: 1.2.3.4/32
        dev: br-lan
      - dst: 10.20.0.0/16
        via: 10.10.10.254
        dev: br-lan
      - default: true
        via: 10.10.10.254
        dev: br-lan

Проверка:

    для каждого route проверить dst/prefix
    проверить oif/dev
    проверить gateway, если задан
    default route хранить как dst 0.0.0.0/0
    если route отсутствует, печатать ACT ip route add ...
    не удалять лишние маршруты на первом этапе

Ограничение:

    IPv4 only
    main table only на первом этапе
    policy routing, metrics и multiple tables позже, только если появится req

## IvanDriver integration target

netrec не должен становиться демоном.

IvanDriver или другой watcher должен:

    слушать RTM_NEWLINK/RTM_DELLINK
    слушать RTM_NEWADDR/RTM_DELADDR
    слушать RTM_NEWROUTE/RTM_DELROUTE
    на событие ставить dirty flag
    запускать netrec после debounce 300-500 ms
    перед --apply брать lock
    после --apply запускать dry-run verify
    при still MISS писать log/error и включать backoff

Главная идея:

    event only marks dirty
    netrec always reads full kernel state
    verifier decides from desired_state + real_state only

## Ближайший порядок работ

1. Добавить lock для --apply.
2. Добавить post-apply netlink reload и verify-after-apply.
3. Обновить README.md под фактическое поведение после каждого шага.
4. Добавить address model: static/dhcp.
5. Добавить multiple routes.
6. Добавить netns fixture tests без sleeps, pcap и железа.
