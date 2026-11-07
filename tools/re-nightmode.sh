#!/bin/sh
# Reverse-engineering helper (runs ON the camera): start the stock lp_app in a
# sandbox for DURATION seconds and log every change to GPIOs, PWMs and ADC
# readings, so we can see how it drives the IR LEDs / IR-cut / status LED.
#
# Sandbox: no default route and empty DNS (LAN only - nothing reaches Prusa or
# the internet), and /userdata is a RAM copy (nothing written to flash).
# Our network and camd are restored afterwards no matter what lp_app did.
#
# usage: setsid sh re-nightmode.sh [DURATION] </dev/null >/dev/null 2>&1 &
DURATION=${1:-90}
OUT=/mnt/sdcard/buddy3d/logs/re-nightmode.log
RUN=/tmp/buddy3d

exec >"$OUT" 2>&1
echo "=== start $(date) duration ${DURATION}s"

snapshot() {
    g=$(grep -E 'gpio-[0-9]+ ' /sys/kernel/debug/gpio | sed 's/  */ /g' | tr '\n' ';')
    p=""
    for c in /sys/class/pwm/pwmchip*; do
        n=${c##*pwmchip}
        if [ -d "$c/pwm0" ]; then
            p="$p pwm$n:en=$(cat $c/pwm0/enable) duty=$(cat $c/pwm0/duty_cycle)/$(cat $c/pwm0/period)"
        fi
    done
    a="adc0=$(cat /sys/bus/iio/devices/iio:device0/in_voltage0_raw) adc1=$(cat /sys/bus/iio/devices/iio:device0/in_voltage1_raw)"
    echo "$g |$p | $a"
}

# --- sandbox ---
touch $RUN/camd.pause
killall camd 2>/dev/null; sleep 2; killall -9 camd 2>/dev/null
GW=$(route -n | awk '$1=="0.0.0.0"{print $2; exit}')
route del default 2>/dev/null
( while true; do route del default 2>/dev/null; sleep 1; done ) &
ROUTE_KILLER=$!
: > $RUN/resolv.empty
mount --bind $RUN/resolv.empty /etc/resolv.conf 2>/dev/null
mkdir -p $RUN/userdata && cp -a /userdata/. $RUN/userdata/ && mount --bind $RUN/userdata /userdata
echo "sandbox ready (gw was $GW)"
echo "baseline: $(snapshot)"

cd /tmp
lp_app --noshell > $RUN/lp_app.out 2>&1 &
LP=$!
echo "lp_app pid $LP"

last=""
t=0
while [ $t -lt $((DURATION * 2)) ]; do
    s=$(snapshot)
    # report ADC changes only when significant, everything else on any change
    key=$(echo "$s" | sed 's/adc1=\([0-9]*\)[0-9]\{2\}/adc1=\1xx/; s/adc0=\([0-9]*\)[0-9]\{2\}/adc0=\1xx/')
    if [ "$key" != "$last" ]; then
        echo "t=$((t / 2)).$((t % 2 * 5))s $s"
        last=$key
    fi
    t=$((t + 1))
    usleep 500000
done

# --- memory snapshot (for comparison with camd) ---
echo "=== memory: lp_app pid $LP"
grep -E "VmPeak|VmSize|VmRSS|RssAnon|RssFile|Threads" /proc/$LP/status
grep -E "MemTotal|MemFree|MemAvailable|^Cached|Slab|CmaTotal|CmaFree" /proc/meminfo
awk '/^[0-9a-f]+-/{name=$6} /^Rss:/{rss[name]+=$2} END{for(n in rss) if(rss[n]>=200) printf "%6d kB %s\n", rss[n], (n==""?"[anon]":n)}' /proc/$LP/smaps | sort -rn | head -12

# --- restore ---
echo "=== stopping lp_app"
kill $LP 2>/dev/null; sleep 2; killall -9 lp_app 2>/dev/null
echo "final: $(snapshot)"
grep -iE 'ldr|night|day|ir|light|gpio|pwm|led' $RUN/lp_app.out | head -100
umount /userdata 2>/dev/null
umount /etc/resolv.conf 2>/dev/null
kill $ROUTE_KILLER 2>/dev/null
# lp_app may have replaced our Wi-Fi setup; restart it
killall wpa_supplicant hostapd udhcpd 2>/dev/null
sleep 1
wpa_supplicant -B -Dnl80211 -iwlan0 -c$RUN/wpa_supplicant.conf
ps | grep -q '[u]dhcpc -i wlan0 -b' || udhcpc -i wlan0 -b -x hostname:"$(hostname)"
sleep 8
[ -n "$GW" ] && route add default gw "$GW" wlan0 2>/dev/null
rm -f $RUN/camd.pause
echo "=== done $(date)"
sync
