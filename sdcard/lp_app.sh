#!/bin/sh
# buddy3d-local boot script.
#
# The camera's init (/oem/usr/bin/RkLunch.sh) copies /mnt/sdcard/lp_app.sh to
# /tmp and runs it *instead of* the stock Prusa app. Everything here runs from
# RAM (/tmp) and the SD card; nothing on the camera's flash is modified, so
# removing the card restores stock behaviour completely.

SD=/mnt/sdcard
BASE=$SD/buddy3d
CONF_SRC=$SD/camera.conf
RUN=/tmp/buddy3d
BIN=$RUN/bin
LOGDIR=$BASE/logs
LOG=$LOGDIR/boot.log

mkdir -p "$RUN" "$BIN" "$LOGDIR"

# Keep the last 3 boot logs
[ -f "$LOG.1" ] && mv -f "$LOG.1" "$LOG.2"
[ -f "$LOG" ] && mv -f "$LOG" "$LOG.1"
exec >"$LOG" 2>&1

log() { echo "$(date '+%Y-%m-%d %H:%M:%S') $*"; }
sync_soon() { sync & }

log "buddy3d-local starting (uptime $(cut -d' ' -f1 /proc/uptime)s)"

# ------------------------------------------------------------------ config ---
if [ ! -f "$CONF_SRC" ]; then
    log "ERROR: $CONF_SRC missing; copy buddy3d/camera.conf.example there"
fi
# strip CRs (Windows editors) and source
tr -d '\r' < "$CONF_SRC" > "$RUN/camera.conf" 2>/dev/null
. "$RUN/camera.conf"

: "${hostname:=buddy3d}" "${ip_mode:=dhcp}" "${lan_only:=0}" "${timezone:=UTC0}"
: "${telnet_enabled:=0}" "${ssh_enabled:=1}" "${ssh_port:=22}" "${debug:=0}"
[ "$debug" = 1 ] && set -x

export TZ="$timezone"
echo "$TZ" > "$RUN/TZ"

# FAT has no exec bits: run binaries from tmpfs
for f in "$BASE"/bin/*; do
    [ -f "$f" ] && cp "$f" "$BIN/" && chmod 755 "$BIN/$(basename "$f")"
done
for l in dropbear dropbearkey scp; do ln -sf dropbearmulti "$BIN/$l"; done
export PATH="$BIN:$PATH:/oem/usr/bin:/oem/usr/sbin"

# Periodic flush so logs survive a power cut
( while true; do sleep 10; sync; done ) &

# helpers: busybox here has no pidof
is_running() { ps | grep -v grep | grep -q "$1"; }
killname() { for p in $(ps | grep -v grep | grep "$1" | awk '{print $1}'); do kill "$p" 2>/dev/null; done; }

# --------------------------------------------------------------- hostname ---
hostname "$hostname"

# ---------------------------------------------------------------- network ---
# The stock init loads the Wi-Fi driver and, ~1s after starting us, launches
# eth_monitor.sh (its own udhcpc, and it blanks the IP on link loss). We run
# our own DHCP client, so reap that for the first 30s, whenever it shows up.
killall wpa_supplicant 2>/dev/null
(
    n=0
    while [ $n -lt 30 ]; do
        killname eth_monitor.sh
        [ -f /var/run/wlan0.pid ] && kill "$(cat /var/run/wlan0.pid)" 2>/dev/null && rm -f /var/run/wlan0.pid
        sleep 1; n=$((n + 1))
    done
) &

i=0
while [ ! -e /sys/class/net/wlan0 ] && [ $i -lt 30 ]; do sleep 1; i=$((i + 1)); done
[ -e /sys/class/net/wlan0 ] || log "ERROR: wlan0 not present"
ifconfig wlan0 up

# 64 hex chars = raw PSK (unquoted); anything else = passphrase (quoted)
case "$wifi_psk" in
    *[!0-9a-fA-F]*) psk_line="psk=\"$wifi_psk\"" ;;
    *) if [ ${#wifi_psk} -eq 64 ]; then psk_line="psk=$wifi_psk"; else psk_line="psk=\"$wifi_psk\""; fi ;;
esac
mkdir -p /var/run/wpa_supplicant
cat > "$RUN/wpa_supplicant.conf" <<EOF
ctrl_interface=/var/run/wpa_supplicant
network={
	scan_ssid=1
	ssid="$wifi_ssid"
	$psk_line
	key_mgmt=WPA-PSK
}
EOF
chmod 600 "$RUN/wpa_supplicant.conf"
log "wifi: joining '$wifi_ssid'"
wpa_supplicant -B -Dnl80211 -iwlan0 -c"$RUN/wpa_supplicant.conf" -f "$LOGDIR/wpa_supplicant.log" 2>/dev/null ||
    wpa_supplicant -B -Dnl80211 -iwlan0 -c"$RUN/wpa_supplicant.conf"

# ------------------------------------------------------------------- camd ---
# Supervise the camera daemon; restart it if it dies. Its log is size-capped.
# Started before the network is up: it doesn't need it, and its status LED
# blinks until Wi-Fi is connected. The LED is switched off whenever camd is not
# running, so "LED off" means "camd down".
CAMD_LOG=$LOGDIR/camd.log
led_off() { [ -e /sys/class/gpio/gpio56/value ] && echo 0 > /sys/class/gpio/gpio56/value; }
(
    backoff=2
    while true; do
        # `touch /tmp/buddy3d/camd.pause` + `killall camd` to stop it for debugging
        if [ -f "$RUN/camd.pause" ]; then led_off; sleep 2; continue; fi
        if [ -f "$CAMD_LOG" ] && [ "$(wc -c < "$CAMD_LOG")" -gt 1048576 ]; then
            mv -f "$CAMD_LOG" "$CAMD_LOG.1"
        fi
        echo "$(date '+%Y-%m-%d %H:%M:%S') starting camd" >> "$CAMD_LOG"
        start=$(cut -d. -f1 /proc/uptime)
        "$BIN/camd" -c "$RUN/camera.conf" >> "$CAMD_LOG" 2>&1 </dev/null
        rc=$?
        ran=$(( $(cut -d. -f1 /proc/uptime) - start ))
        echo "$(date '+%Y-%m-%d %H:%M:%S') camd exited rc=$rc after ${ran}s" >> "$CAMD_LOG"
        led_off
        sync
        # stable for a minute -> quick restart; otherwise back off up to 60s
        if [ $ran -gt 60 ]; then backoff=2; else backoff=$((backoff * 2)); [ $backoff -gt 60 ] && backoff=60; fi
        sleep $backoff
    done
) &
log "camd: supervisor started"

wait_for_ip() {
    n=0
    while [ $n -lt "$1" ]; do
        ip=$(ifconfig wlan0 2>/dev/null | sed -n 's/.*inet addr:\([0-9.]*\).*/\1/p')
        [ -n "$ip" ] && return 0
        sleep 1; n=$((n + 1))
    done
    return 1
}

if [ "$ip_mode" = static ]; then
    # wait for association, then configure
    n=0
    while [ "$(cat /sys/class/net/wlan0/carrier 2>/dev/null)" != 1 ] && [ $n -lt 60 ]; do sleep 1; n=$((n + 1)); done
    ifconfig wlan0 "$static_ip" netmask "${static_netmask:-255.255.255.0}" up
    [ -n "$static_gateway" ] && route add default gw "$static_gateway" wlan0
    [ -n "$static_dns" ] && echo "nameserver $static_dns" > /etc/resolv.conf 2>/dev/null
else
    # -b: background if no lease yet; keeps renewing
    udhcpc -i wlan0 -b -x hostname:"$hostname" -p /var/run/udhcpc.wlan0.pid
fi

if wait_for_ip 60; then
    log "network: wlan0 $ip"
else
    log "ERROR: no IP address after 60s (check wifi_ssid / wifi_psk, 2.4 GHz only)"
fi

if [ "$lan_only" = 1 ]; then
    log "network: lan_only=1, removing default route"
    ( while true; do route del default 2>/dev/null; sleep 30; done ) &
fi

# ------------------------------------------------------------------- time ---
# The stock init starts ntpd against pool.ntp.org; replace it with ours.
killall ntpd 2>/dev/null
if [ -n "$ntp_server" ]; then
    printf 'server %s iburst\n' "$ntp_server" > "$RUN/ntp.conf"
    ntpd -g -c "$RUN/ntp.conf" -p /var/run/ntpd.pid
    log "ntp: $ntp_server"
fi

# ---------------------------------------------------------- remote shells ---
if [ "$telnet_enabled" = 1 ]; then
    ( while true; do telnetd -F -l /bin/sh; sleep 5; done ) >/dev/null 2>&1 &
    log "telnet: enabled (unauthenticated root shell on port 23)"
fi

if [ "$ssh_enabled" = 1 ]; then
    # dropbear insists the authorized_keys path up to $HOME is root-owned and
    # not group/other writable; / is neither on this rootfs, so give root a
    # private tmpfs home (RAM only, gone on reboot).
    mount | grep -q ' /root ' || mount -t tmpfs -o mode=700,size=1m tmpfs /root
    mkdir -p -m 700 /root/.ssh
    if [ -f "$BASE/authorized_keys" ]; then
        tr -d '\r' < "$BASE/authorized_keys" > /root/.ssh/authorized_keys
        chmod 600 /root/.ssh/authorized_keys
    else
        log "ssh: WARNING no $BASE/authorized_keys; nobody can log in"
    fi
    cat > /root/.profile <<EOF
export PATH="$PATH"
export TZ="$TZ"
EOF
    # persistent host key on the SD card so the fingerprint survives reboots
    HOSTKEY=$BASE/ssh_host_ed25519_key
    [ -s "$HOSTKEY" ] || dropbearkey -t ed25519 -f "$HOSTKEY" >/dev/null 2>&1
    cp "$HOSTKEY" "$RUN/hostkey" && chmod 600 "$RUN/hostkey"
    ( while true; do dropbear -F -E -r "$RUN/hostkey" -p "$ssh_port"; sleep 5; done ) \
        >>"$LOGDIR/dropbear.log" 2>&1 &
    log "ssh: dropbear on port $ssh_port"
fi


log "boot complete"
sync
# Return to the stock init (it expects lp_app to run in the background).
exit 0
