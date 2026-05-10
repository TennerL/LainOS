#!/bin/sh
set -eu

IFACE=${IFACE:-}
PXE_IP=${PXE_IP:-192.168.67.1}
DHCP_RANGE=${DHCP_RANGE:-192.168.67.50,192.168.67.150,12h}
TFTP_ROOT=${TFTP_ROOT:-build/pxe}
BOOT_FILE=${BOOT_FILE:-BOOTX64.EFI}
CONFIGURE_IFACE=${CONFIGURE_IFACE:-0}

case "$TFTP_ROOT" in
    /*) ;;
    *) TFTP_ROOT="$(pwd)/$TFTP_ROOT" ;;
esac

if [ -z "$IFACE" ]; then
    echo "usage: IFACE=<network-interface> $0" >&2
    echo "optional: PXE_IP=$PXE_IP DHCP_RANGE=$DHCP_RANGE CONFIGURE_IFACE=1" >&2
    exit 2
fi

DNSMASQ=${DNSMASQ:-}
if [ -z "$DNSMASQ" ]; then
    if command -v dnsmasq >/dev/null 2>&1; then
        DNSMASQ=$(command -v dnsmasq)
    elif [ -x /opt/homebrew/sbin/dnsmasq ]; then
        DNSMASQ=/opt/homebrew/sbin/dnsmasq
    elif [ -x /usr/local/sbin/dnsmasq ]; then
        DNSMASQ=/usr/local/sbin/dnsmasq
    fi
fi

if [ -z "$DNSMASQ" ]; then
    echo "dnsmasq is required. Install it on the host, then rerun this script." >&2
    exit 1
fi

if [ ! -f "$TFTP_ROOT/$BOOT_FILE" ]; then
    echo "$TFTP_ROOT/$BOOT_FILE is missing; run: make pxe" >&2
    exit 1
fi

if [ "$CONFIGURE_IFACE" = "1" ]; then
    if command -v ip >/dev/null 2>&1; then
        ip addr add "$PXE_IP/24" dev "$IFACE" 2>/dev/null || true
        ip link set "$IFACE" up
    elif command -v ifconfig >/dev/null 2>&1; then
        ifconfig "$IFACE" "$PXE_IP" netmask 255.255.255.0 up
    else
        echo "CONFIGURE_IFACE=1 needs either ip or ifconfig." >&2
        exit 1
    fi
fi

echo "Serving UEFI PXE on $IFACE from $TFTP_ROOT/$BOOT_FILE"
echo "Server IP: $PXE_IP  DHCP range: $DHCP_RANGE"
echo "Firmware boot file: $BOOT_FILE"
echo "If dnsmasq logs Arch:00000, select the motherboard's UEFI PXE boot entry."

exec "$DNSMASQ" \
    --no-daemon \
    --log-dhcp \
    --interface="$IFACE" \
    --bind-interfaces \
    --dhcp-authoritative \
    --dhcp-range="$DHCP_RANGE" \
    --dhcp-option=option:router,"$PXE_IP" \
    --dhcp-option-force=option:tftp-server,"$PXE_IP" \
    --dhcp-option-force=option:bootfile-name,"$BOOT_FILE" \
    --dhcp-match=set:efi64,option:client-arch,7 \
    --dhcp-match=set:efi64,option:client-arch,9 \
    --dhcp-boot="$BOOT_FILE",,"$PXE_IP" \
    --enable-tftp \
    --tftp-root="$TFTP_ROOT"
