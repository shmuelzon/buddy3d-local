#!/bin/bash
# Interactive IR test (runs on the PC). Each step: N beeps on the camera
# speaker, set IR LED PWM + GPIO55 (+ optional GPIO54), read back, grab a frame.
#   tools/ir-test.sh "8000 0" "8000 1" "0 1" "0 0"     # "pwm g55 [g54]"
set -u
cd "$(dirname "$0")/.."
CAM=${CAM:-buddy3d}
C="python3 -I tools/cam.py run"
OUT=build/test
mkdir -p $OUT
retry() { for i in 1 2 3 4 5; do "$@" && return 0; sleep 2; done; return 1; }
beep() {
    local d; d=$(awk "BEGIN{printf \"%.2f\", $1*0.4}")
    ffmpeg -v error -f lavfi -i "aevalsrc='if(lt(mod(t,0.4),0.15),0.7*sin(2*PI*880*t),0)':d=$d" \
        -f s16le -ar 16000 -ac 1 - | timeout 10 nc -q 1 "$CAM" 7777
}
retry $C '[ -d /sys/class/gpio/gpio55 ] || { echo 55 > /sys/class/gpio/export; echo out > /sys/class/gpio/gpio55/direction; }
[ -d /sys/class/pwm/pwmchip7/pwm0 ] || { echo 0 > /sys/class/pwm/pwmchip7/export; echo 10000 > /sys/class/pwm/pwmchip7/pwm0/period; }
echo 1 > /sys/class/pwm/pwmchip7/pwm0/enable' >/dev/null || { echo "camera unreachable"; exit 1; }
n=0
for s in "$@"; do
    n=$((n + 1)); set -- $s; t0=$(date +%s)
    beep $n
    g54=${3:-1}
    got=$(retry $C "echo $1 > /sys/class/pwm/pwmchip7/pwm0/duty_cycle; echo $2 > /sys/class/gpio/gpio55/value; echo $g54 > /sys/class/gpio/gpio54/value; echo \$(cat /sys/class/pwm/pwmchip7/pwm0/duty_cycle) \$(cat /sys/class/gpio/gpio55/value) \$(cat /sys/class/gpio/gpio54/value)")
    echo "$(date +%T) step $n: wanted pwm=$1 g55=$2 g54=$g54, readback: $(echo "$got" | tail -1 | tr -d '> ')"
    sleep 3
    timeout 20 ffmpeg -v error -rtsp_transport tcp -i rtsp://$CAM/live -frames:v 1 -vf scale=480:-1 -update 1 -y $OUT/ir_$n.jpg
    while [ $(( $(date +%s) - t0 )) -lt 12 ]; do sleep 1; done
done
