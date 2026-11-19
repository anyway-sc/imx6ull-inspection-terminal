#!/bin/sh
# 本项目板端启动入口；--check 只检查启动条件。
set -eu

VEHICLE_SERVER_APP=${VEHICLE_SERVER_APP:-/home/root/tcp_ping_server_app}
VEHICLE_ICM_MODULE=${VEHICLE_ICM_MODULE:-/home/root/icm20608-csfix1.ko}
VEHICLE_ICM_DEVICE=${VEHICLE_ICM_DEVICE:-/dev/icm20608}
VEHICLE_ICM_SYS_DIR=${VEHICLE_ICM_SYS_DIR:-/sys/module/icm20608}
VEHICLE_DHT_MODULE=${VEHICLE_DHT_MODULE:-/home/root/vehicle_dht11.ko}
VEHICLE_SR04_MODULE=${VEHICLE_SR04_MODULE:-/home/root/vehicle_sr04.ko}
# 以下路径可在隔离测试目录替换；板端默认始终为真实 sysfs。
VEHICLE_SYS_ROOT=${VEHICLE_SYS_ROOT:-/sys}
LC_ALL=C
export LC_ALL

usage()
{
    printf '%s\n' \
        '用法：sh start-server.sh [--check] [/dev/videoX]' \
        '  --check  只检查启动条件，不加载模块或启动服务。' \
        '  默认摄像头为 /dev/video2，服务保持前台运行。'
}

fail()
{
    printf '启动检查失败：%s\n' "$1" >&2
    exit 1
}

check_only=0
case ${1:-} in
    --check) check_only=1; shift ;;
    --help|-h) usage; exit 0 ;;
    --*) usage >&2; exit 2 ;;
esac
if [ "$#" -gt 1 ]; then
    usage >&2
    exit 2
fi
camera_device=${1:-/dev/video2}

# 先确认端口，避免已有服务时再加载设备模块。
command -v netstat >/dev/null 2>&1 || fail '找不到 netstat。'
command -v awk >/dev/null 2>&1 || fail '找不到 awk。'
if ! tcp_table=$(netstat -lntp 2>&1); then
    printf '%s\n' "$tcp_table" >&2
    fail '无法读取 TCP 监听状态，请检查当前终端权限。'
fi
if ! listeners=$(printf '%s\n' "$tcp_table" | awk '
    $1 ~ /^tcp/ && $4 ~ /:9000$/ && $6 == "LISTEN" { print }
'); then
    fail '无法解析 TCP 监听状态。'
fi
if [ -n "$listeners" ]; then
    printf '%s\n' '9000 端口已有监听进程：' "$listeners" >&2
    printf '%s\n' "$listeners" | awk '{ print $7 }' |
    while IFS=/ read -r listener_pid listener_name; do
        case $listener_pid in
            ''|*[!0-9]*) continue ;;
        esac
        ps -p "$listener_pid" -o pid,stat,wchan,args >&2 || :
    done
    printf '%s\n' '请核对已有服务；STAT=T 表示暂停，暂停仍占用端口。' >&2
    exit 1
fi
printf '%s\n' '9000 端口空闲。'

[ -f "$VEHICLE_SERVER_APP" ] && [ -x "$VEHICLE_SERVER_APP" ] ||
    fail "服务文件不存在或没有执行权限：$VEHICLE_SERVER_APP"
[ -c "$camera_device" ] && [ -r "$camera_device" ] && [ -w "$camera_device" ] ||
    fail "摄像头节点不存在或不可读写：$camera_device"

if [ ! -d "$VEHICLE_ICM_SYS_DIR" ]; then
    if [ "$check_only" -eq 1 ]; then
        fail 'ICM20608 驱动未加载；正常启动时将尝试加载已有 csfix1 模块。'
    fi
    [ -f "$VEHICLE_ICM_MODULE" ] && [ -r "$VEHICLE_ICM_MODULE" ] ||
        fail "找不到已验收的 ICM 模块：$VEHICLE_ICM_MODULE"
    command -v insmod >/dev/null 2>&1 || fail '找不到 insmod。'
    printf '正在加载 ICM20608 模块：%s\n' "$VEHICLE_ICM_MODULE"
    if ! insmod "$VEHICLE_ICM_MODULE"; then
        fail 'ICM 模块加载失败，请保留上面的错误及 dmesg 日志。'
    fi
fi

if ! icm_version=$(cat "$VEHICLE_ICM_SYS_DIR/version" 2>/dev/null); then
    fail '无法确认 ICM 驱动版本，请检查 dmesg 日志。'
fi
[ "$icm_version" = 'stage03-csfix1' ] ||
    fail "当前 ICM 驱动版本不匹配：$icm_version（预期 stage03-csfix1）"
[ -c "$VEHICLE_ICM_DEVICE" ] && [ -r "$VEHICLE_ICM_DEVICE" ] ||
    fail "ICM 驱动已加载，但设备节点不存在或不可读：$VEHICLE_ICM_DEVICE；请检查 dmesg。"
printf 'ICM 驱动版本：%s；设备：%s\n' "$icm_version" "$VEHICLE_ICM_DEVICE"
printf '摄像头节点：%s\n' "$camera_device"

ensure_sensor_module()
{
    sensor_name=$1
    sensor_module=$2
    sensor_version=$3
    sensor_sys="$VEHICLE_SYS_ROOT/module/$sensor_name"
    if [ ! -d "$sensor_sys" ]; then
        [ "$check_only" -eq 0 ] || fail "$sensor_name 未加载；正常启动会尝试加载已有模块。"
        [ -r "$sensor_module" ] || fail "找不到传感器模块：$sensor_module"
        printf '正在加载模块：%s\n' "$sensor_module"
        insmod "$sensor_module" || fail "$sensor_name 加载失败，请保留错误和 dmesg。"
    fi
    actual_version=$(cat "$sensor_sys/version") || fail "无法读取 $sensor_name 版本。"
    [ "$actual_version" = "$sensor_version" ] || fail "$sensor_name 版本不匹配：$actual_version"
    [ -r "$VEHICLE_SYS_ROOT/class/misc/$sensor_name/value" ] || fail "$sensor_name 的读取接口不存在。"
}

# 先核对 GPIO1 所属的运行配置；旧设备树下不尝试驱动这根线。
dt_root="$VEHICLE_SYS_ROOT/firmware/devicetree/base"
for legacy_node in dht11 ds18b20; do
    legacy_status=$(tr '\000' '\n' < "$dt_root/$legacy_node/status") ||
        fail "无法核对旧 $legacy_node 节点；请使用已验收的 GPIO1 DTB。"
    [ "$legacy_status" = disabled ] || fail "旧 $legacy_node 未停用；请使用 GPIO1 DTB。"
done
pinmux_file="$VEHICLE_SYS_ROOT/kernel/debug/pinctrl/20e0000.iomuxc/pinmux-pins"
gpio1_mux=$(grep 'MX6UL_PAD_GPIO1_IO01' "$pinmux_file") || fail '无法核对 GPIO1 的 pinctrl 配置。'
printf '%s\n' "$gpio1_mux" | grep -q 'group hoggrp-1' || fail 'GPIO1 未应用已验收的默认引脚组。'
gpio_base=$(cat "$VEHICLE_SYS_ROOT/class/gpio/gpiochip0/base") || fail '缺少 GPIO1 控制器。'
gpio_label=$(cat "$VEHICLE_SYS_ROOT/class/gpio/gpiochip0/label") || fail '无法读取 GPIO1 控制器标签。'
[ "$gpio_base" = 0 ] && [ "$gpio_label" = 209c000.gpio ] || fail 'GPIO1 控制器映射不匹配。'
gpio_dir="$VEHICLE_SYS_ROOT/class/gpio/gpio1"
if [ "$check_only" -eq 0 ]; then
    if [ ! -d "$gpio_dir" ]; then
        printf '1\n' > "$VEHICLE_SYS_ROOT/class/gpio/export" || fail '导出 GPIO1 失败。'
    fi
    printf 'in\n' > "$gpio_dir/direction" || fail '设置 GPIO1 输入失败。'
    printf '0\n' > "$gpio_dir/active_low" || fail '设置 GPIO1 原始电平读取失败。'
fi
gpio_direction=$(cat "$gpio_dir/direction") || fail 'GPIO1 未导出，请正常启动以准备输入。'
gpio_inverted=$(cat "$gpio_dir/active_low") || fail '无法核对 GPIO1 电平极性。'
[ "$gpio_direction" = in ] && [ "$gpio_inverted" = 0 ] || fail 'GPIO1 必须是输入、active_low=0。'
[ -r "$gpio_dir/value" ] || fail 'GPIO1 value 不可读。'
printf '%s\n' 'SR501 输入就绪：GPIO1/JP6-17；上电预热后再开始哨兵监测。'

ensure_sensor_module vehicle_dht11 "$VEHICLE_DHT_MODULE" stage05-gpio2-v1
ensure_sensor_module vehicle_sr04 "$VEHICLE_SR04_MODULE" stage05-gpio4-9-v1

if [ "$check_only" -eq 1 ]; then
    printf '%s\n' '启动条件检查通过；未启动服务。'
    exit 0
fi

printf '%s\n' '检查通过，启动前台服务。停止请按 Ctrl+C；Ctrl+Z 会暂停并继续占用端口。'
# exec 用服务进程替换脚本进程，保留当前终端及前台作业关系。
exec "$VEHICLE_SERVER_APP" "$camera_device"
