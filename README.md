# buddy3d-local

A local-only replacement for the stock software on the **Prusa Buddy3D camera**
(Rockchip RV1103, Core One). It runs entirely from the SD card: no Prusa cloud,
no internet, nothing written to the camera's flash. Remove the card and reboot,
and the camera is exactly as it was.

What you get:

- **RTSP** video, H.264 or H.265: `rtsp://<camera>/live`
- **JPEG stills** over HTTP: `http://<camera>/still`
- **Speaker** input over TCP/HTTP (talk-back / announcements). The camera has
  **no microphone**, so audio is one-way.
- **Night mode**: IR-cut filter, IR LEDs and black-and-white image, switched by the light sensor
- **On-screen display**: fixed text and/or a timestamp, burned in by the encoder
- **Status LED**: blinks until Wi-Fi is connected, then solid; off if `camd` is down
- Wi-Fi (DHCP or static), NTP, timezone, hostname, optional "LAN only" mode
- **SSH** (dropbear, key-only) and optional telnet

## How it works

The camera's init script (`/oem/usr/bin/RkLunch.sh`) runs `/mnt/sdcard/lp_app.sh`
*instead of* the stock `lp_app` when that file exists on the card. Our
`lp_app.sh` copies its binaries to RAM, joins Wi-Fi, starts NTP/SSH, and
supervises `camd`, a small daemon built on Rockchip's media stack (the camera's
own `/oem/usr/lib` libraries and `/etc/iqfiles` sensor tuning). Everything
lives in `/tmp` (RAM) and on the card.

## Quick install (prebuilt)

1. Download `buddy3d-local-<version>.zip` (or `.tar.gz`) from the
   [releases](../../releases) page.
2. FAT32-format a microSD card and extract the archive **to the root of the card**.
3. Edit `camera.conf` on the card: at least `wifi_ssid` and `wifi_psk` (2.4 GHz
   only). Every option is documented in that file.
4. For SSH, put your public key in `buddy3d/authorized_keys` on the card.
5. Insert the card and power-cycle the camera.

## Build

Needs a Linux x86-64 host with `git`, `make`, `curl`, `tar`, `bzip2`. No Docker,
nothing installed system-wide; everything lands in `deps/` and `build/`.

```sh
make            # first run fetches ~370 MB (toolchain + SDK subset) and dropbear
```

The Rockchip toolchain, Rockit/rkaiq headers and `librtsp` come from a pinned,
sparse checkout of the [Luckfox Pico SDK](https://github.com/LuckfoxTECH/luckfox-pico)
(same RV1103 SoC and uClibc). To reuse an existing checkout:
`make LUCKFOX_SDK=~/code/luckfox-pico`.

The result is `out/sdcard/`:

```
lp_app.sh                 boot hook
camera.conf               your settings
buddy3d/bin/camd          camera daemon
buddy3d/bin/dropbearmulti ssh server
buddy3d/authorized_keys   (optional) SSH public keys
```

### Configuration

[`sdcard/camera.conf.example`](sdcard/camera.conf.example) documents every
option with its default and allowed values. Copy it to `sdcard/camera.conf`
and edit; that file and `sdcard/authorized_keys` are git-ignored (Wi-Fi
password, keys) and `make` copies them onto the card image. Without a
`sdcard/camera.conf` the example is used. You can also pass
`AUTHORIZED_KEYS=~/.ssh/id_ed25519.pub`.

## Install

1. FAT32-format a microSD card.
2. `make deploy SD=/path/to/mounted/card` (or copy `out/sdcard/*` to the card root).
3. Edit `camera.conf` on the card: at least `wifi_ssid` and `wifi_psk`. The
   camera is 2.4 GHz only.
4. Insert, power-cycle. It shows up as `buddy3d` (DHCP) about 15 s after boot.

Logs are on the card in `buddy3d/logs/` (`boot.log`, `camd.log`,
`wpa_supplicant.log`, `dropbear.log`).

**Do not** put `cus_update_ota.tar` / `fac_update_ota.tar` on the card: the
stock init reboots into recovery and flashes them.

## Using it

### Video

```sh
ffplay -rtsp_transport tcp rtsp://buddy3d/live
```

Settings: `video_codec` (`h264`/`h265`), `video_width`/`video_height`,
`video_bitrate` (kbps), `video_rc` (`cbr`/`vbr`), `video_gop`, `video_flip`,
`video_fps`. The sensor runs at 1080p up to 30 fps (read from its driver at
startup); `video_fps=0` encodes everything it delivers, a lower value makes
the encoder drop frames evenly. Auto exposure may slow the sensor in low light.

### JPEG stills

```sh
curl -o still.jpg http://buddy3d/still
```

Each request takes the current frame and encodes it with the hardware JPEG
encoder (~100 ms on the camera, no CPU cost, no dropped video frames). The
on-screen text/timestamp is included. Settings: `http_port` (default 80, 0
disables) and `still_quality` (1..99, default 80). A 1080p still is ~230 KB.

### Night mode

`ir_mode=auto|day|night`. Night moves the IR-cut filter out of the way
(`ir_cut=1`), turns on the IR LEDs (`ir_brightness`, %) and switches the ISP to
the sensor's black-and-white "gray" tuning (`ir_gray=1`). Day puts the filter
back in front of the sensor for true colours.
In auto mode the light sensor is averaged and a new state must persist for
`ir_switch_delay` seconds; thresholds are `ir_night_threshold` /
`ir_day_threshold` (sensor reads ~950 in a lit room, ~1020 in the dark).
The IR-cut filter is a bistable coil on the TMI8150B motor driver (see the
hardware notes); `camd` pulses it for `ir_cut_pulse_ms` (200 ms) on each switch.

### On-screen display

```sh
osd_text="Office - Core One"      # fixed text, printable ASCII
osd_text_position="tl"            # tl | tr | bl | br
osd_time="%Y-%m-%d %H:%M:%S"      # strftime format, local time (see timezone)
osd_time_position="tr"
osd_font_size=32                  # 16 | 32 | 64
```

Items sharing a position are stacked (text, then time). White text with a
dark outline, drawn with the [Spleen](https://github.com/fcambus/spleen)
bitmap font (BSD-2-Clause, licence shipped as `buddy3d/LICENSE.spleen-font`).

### Status LED

`led_mode=status` (default) blinks at 1 Hz while Wi-Fi is not associated or
has no IP, and is solid once connected. `camd` starts before the network, so
this covers the initial connection. The boot script turns the LED off whenever
`camd` is not running. `led_mode=on|off` forces it.

### Speaker

Port 7777 (`speaker_port`) accepts, one stream at a time (a new connection
pre-empts the current one):

```sh
# raw PCM, s16le 16 kHz mono (the default when no header is sent)
ffmpeg -i doorbell.mp3 -f s16le -ar 16000 -ac 1 - | nc buddy3d 7777

# any WAV file (PCM 8/16-bit, A-law, u-law; any rate / channels)
nc buddy3d 7777 < chime.wav
curl --data-binary @chime.wav http://buddy3d:7777/

# HTTP with explicit format, e.g. G.711 A-law 8 kHz, streamed (chunked)
ffmpeg -i speech.mp3 -f alaw -ar 8000 -ac 1 - | \
    curl -T - 'http://buddy3d:7777/?format=alaw&rate=8000'
```

Query parameters: `format=s16le|s16be|u8|alaw|ulaw`, `rate`, `channels`, `volume` (0..100).

### Home Assistant / go2rtc / Scrypted

[go2rtc](https://github.com/AlexxIT/go2rtc) (built into Home Assistant) can do
talk-back audio by combining the RTSP stream with an `exec` backchannel
(untested so far; go2rtc pipes the talk-back audio to the command's stdin):

```yaml
go2rtc:
  streams:
    buddy3d:
      - rtsp://buddy3d/live
      - "exec:ffmpeg -f alaw -ar 8000 -ac 1 -i - -f s16le -ar 16000 -ac 1 tcp://buddy3d:7777#backchannel=1"
```

For one-off announcements (TTS, doorbell chimes), a `shell_command` that pipes
`ffmpeg` into `nc buddy3d 7777` (or `curl --data-binary`) is enough.

For a Home Assistant generic camera use `rtsp://buddy3d/live` as the stream
source and `http://buddy3d/still` as the still image URL.

Scrypted: add it as an RTSP camera with `rtsp://buddy3d/live`.

### Shell

- SSH: `ssh root@buddy3d` with a key from `buddy3d/authorized_keys`. The host key
  is generated on first boot and kept on the card.
- Telnet (`telnet_enabled=1`) is an **unauthenticated** root shell. Use it only
  for bring-up.
- Stop `camd` for debugging: `touch /tmp/buddy3d/camd.pause; killall camd`
  (`rm` the file to let the supervisor restart it).
- `camd -A` lists the audio codec mixer controls; set them with `audio_mixer=`.

## Development

`tools/cam.py` talks to the camera over telnet (`CAM=<address>`, default
`buddy3d`):

```sh
tools/cam.py run 'uptime'
tools/cam.py push build/camd /tmp/x
make push            # rebuild camd, upload it, let the supervisor restart it
```

`tools/re-nightmode.sh` runs the stock `lp_app` in a sandbox (no default
route, no DNS, RAM copy of `/userdata`) and logs what it does to GPIOs, PWMs
and the ADC.

## Hardware notes

| What | Where |
|---|---|
| SoC | Rockchip RV1103G, Cortex-A7, ~34 MB RAM for Linux |
| Sensor | JX-F37P 1080p, IQ file `/etc/iqfiles/jx_f37p_*.json` |
| Wi-Fi | RTL8188FU (USB). GPIO 34 and 53 power it: don't touch |
| Speaker amp | GPIO 4 (`pa-ctl`), driven by the kernel during playback |
| Microphone | **none fitted**: codec inputs are unconnected (a loud tone from the speaker shows up at -67 dBFS, crosstalk level) |
| IR LEDs | GPIO 55 = enable, PWM 7 (`/sys/class/pwm/pwmchip7`, 10 us period) = brightness; stock uses 80% |
| IR-cut filter | coil on a TMI8150B SPI motor driver (its pan/tilt outputs are unused): `/dev/spidev0.0`, mode 0, ≤5 MHz (the device tree default of 50 MHz garbles it), EN = GPIO 54. Frame: `[0x80\|reg, data]` to write, `[reg, 0]` to read; chip id at reg 0x00 = 0x81. Reg 0x02 (GLOBAL CTRL) bit 3 GENABLE must be set (it powers up 0, outputs disabled); `camd` writes 0x89 like the stock app. Reg 0x11: bit 7 enable, bits 3..2 direction: `0x88` (reverse) = filter in, `0x84` (forward) = filter out; pulse ~200 ms then write 0x00 |
| Status LED | GPIO 56 (green) |
| Reset button | SARADC channel 0 via the kernel `adc-keys` driver: key events on `/dev/input/event0` (KEY_VOLUMEDOWN/UP). Nothing in the kernel acts on it; the stock app did. Currently unused |
| Light sensor | SARADC channel 1 (LDR): ~390 flashlight, ~950 lit room, ~1020 dark (saturates) |

## Status / TODO

- [x] RTSP H.264/H.265
- [x] Speaker sink (raw / WAV / HTTP, pre-emption)
- [x] Wi-Fi, NTP, SSH, telnet, LAN-only mode, log rotation
- [x] Night mode (IR-cut filter, IR LEDs, gray scene, auto switching on the light sensor)
- [x] Status LED (Wi-Fi status)
- [x] On-screen text / timestamp
- [ ] Reset button (e.g. long press = reboot)
- [ ] Sub-stream (lower resolution second RTSP path)
- [ ] Verify the go2rtc / Home Assistant talk-back config

## License

[MIT](LICENSE). The on-screen display uses the
[Spleen](https://github.com/fcambus/spleen) font (BSD-2-Clause), fetched at
build time; its licence ships as `buddy3d/LICENSE.spleen-font`.
