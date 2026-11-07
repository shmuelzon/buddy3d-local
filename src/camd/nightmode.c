/* Day/night switching and the status LED.
 *
 * Hardware (found by observing the stock app):
 *   IR LEDs:      GPIO 55 = enable, PWM 7 (pwmchip7/pwm0, 10 us period) = brightness
 *   Light sensor: SARADC channel 1 (LDR). ~950 lit room, ~1020 dark (saturates), lower = brighter
 *   Status LED:   GPIO 56
 *   IR-cut:       coil on the TMI8150B motor driver (its pan/tilt channels are
 *                 unused here). SPI on /dev/spidev0.0, chip EN = GPIO 54.
 *                 Register 0x02 (GLOBAL CTRL) bit 3 GENABLE must be set: it
 *                 powers up 0 with every output disabled. Register 0x11: bit 7
 *                 enable, bits 3..2 direction (01 forward = filter out, 10
 *                 reverse = filter in). The coil is bistable: pulse ~200 ms,
 *                 then disable.
 */
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "camd.h"

#define GPIO_IR_EN   55
#define GPIO_LED     56
#define PWM_CHIP     "/sys/class/pwm/pwmchip7"
#define PWM_PERIOD   10000
#define LDR_PATH     "/sys/bus/iio/devices/iio:device0/in_voltage1_raw"
#define GPIO_TMI_EN  54
#define TMI_DEV      "/dev/spidev0.0"
#define TMI_SPEED    1000000   /* chip max 5 MHz; the DT default (50 MHz) garbles */
#define TMI_REG_ID   0x00
#define TMI_REG_GCTL 0x02      /* GLOBAL CTRL */
#define TMI_GRESET   0x80      /*   1 = out of reset */
#define TMI_STANDBY  0x40      /*   1 = low-power sleep */
#define TMI_GENABLE  0x08      /*   1 = channel outputs enabled (power-on default 0!) */
#define TMI_REG_IR   0x11      /* IRCUT CTRL: bit 7 enable, bits 3..2 direction */

static pthread_t thread;
static int running;
static int is_night = -1;

static int write_str(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY), ok;
	if (fd < 0)
		return -1;
	ok = write(fd, val, strlen(val)) == (ssize_t)strlen(val);
	close(fd);
	return ok ? 0 : -1;
}

static int write_int(const char *path, int v)
{
	char buf[16];
	snprintf(buf, sizeof(buf), "%d", v);
	return write_str(path, buf);
}

static int gpio_out(int gpio, int value)
{
	char path[64];
	struct stat st;
	snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d", gpio);
	if (stat(path, &st)) {
		write_int("/sys/class/gpio/export", gpio);
		snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", gpio);
		write_str(path, "out");
	}
	snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", gpio);
	return write_int(path, value);
}

static int read_ldr(void)
{
	char buf[16] = { 0 };
	int fd = open(LDR_PATH, O_RDONLY), n;
	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	return n > 0 ? atoi(buf) : -1;
}

/* ---- TMI8150B IR-cut ---- */
static int tmi_fd = -1;

static int tmi_xfer(uint8_t b0, uint8_t b1, uint8_t *rx1)
{
	uint8_t tx[2] = { b0, b1 }, rx[2] = { 0, 0 };
	struct spi_ioc_transfer t;
	memset(&t, 0, sizeof(t));
	t.tx_buf = (uintptr_t)tx;
	t.rx_buf = (uintptr_t)rx;
	t.len = 2;
	t.speed_hz = TMI_SPEED;
	t.bits_per_word = 8;
	if (ioctl(tmi_fd, SPI_IOC_MESSAGE(1), &t) < 0)
		return -1;
	if (rx1)
		*rx1 = rx[1];
	return 0;
}

static int tmi_write(int reg, uint8_t v) { return tmi_xfer(0x80 | (reg & 0x1f), v, NULL); }
static int tmi_read(int reg, uint8_t *v) { return tmi_xfer(reg & 0x1f, 0, v); }

static void ircut_init(void)
{
	uint8_t mode = SPI_MODE_0, bits = 8, id = 0;
	uint32_t speed = TMI_SPEED;

	if (!cfg.ir_cut)
		return;
	gpio_out(GPIO_TMI_EN, 1);
	usleep(10 * 1000);
	tmi_fd = open(TMI_DEV, O_RDWR);
	if (tmi_fd < 0 || ioctl(tmi_fd, SPI_IOC_WR_MODE, &mode) < 0 ||
	    ioctl(tmi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
	    ioctl(tmi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0 || tmi_read(TMI_REG_ID, &id)) {
		ERR("ircut: cannot open %s, IR-cut disabled", TMI_DEV);
		goto fail;
	}
	if (id != 0x81) {
		ERR("ircut: unexpected TMI8150B id 0x%02x, IR-cut disabled", id);
		goto fail;
	}
	tmi_write(TMI_REG_IR, 0x00); /* make sure the coil isn't left driven */

	/* out of reset, awake, outputs enabled; keep the other writable bits */
	uint8_t g = 0, g2 = 0;
	tmi_read(TMI_REG_GCTL, &g);
	tmi_write(TMI_REG_GCTL, (g & ~TMI_STANDBY) | TMI_GRESET | TMI_GENABLE);
	tmi_read(TMI_REG_GCTL, &g2);
	if (!(g2 & TMI_GENABLE)) {
		ERR("ircut: cannot enable TMI8150B outputs (GLOBAL CTRL 0x%02x), IR-cut disabled", g2);
		goto fail;
	}
	LOG("ircut: TMI8150B found, GLOBAL CTRL 0x%02x -> 0x%02x, pulse %d ms", g, g2,
	    cfg.ir_cut_pulse_ms);
	return;
fail:
	if (tmi_fd >= 0)
		close(tmi_fd);
	tmi_fd = -1;
}

/* in = filter in front of the sensor (day), out = removed (night) */
static void ircut_set(int in)
{
	uint8_t v = 0;
	if (tmi_fd < 0)
		return;
	if (tmi_write(TMI_REG_IR, 0x80 | (in ? 0x08 : 0x04)) || tmi_read(TMI_REG_IR, &v) ||
	    !(v & 0x80)) {
		ERR("ircut: write failed (reg 0x11 reads 0x%02x)", v);
	}
	usleep(cfg.ir_cut_pulse_ms * 1000);
	tmi_write(TMI_REG_IR, 0x00);
	LOG("ircut: filter %s (reg 0x11 read 0x%02x during the pulse)", in ? "in" : "out", v);
}

static void ircut_close(void)
{
	if (tmi_fd >= 0) {
		tmi_write(TMI_REG_IR, 0x00);
		close(tmi_fd);
	}
	tmi_fd = -1;
}

static void ir_leds(int on)
{
	struct stat st;
	int duty = PWM_PERIOD * cfg.ir_brightness / 100;

	if (stat(PWM_CHIP "/pwm0", &st))
		write_int(PWM_CHIP "/export", 0);
	write_int(PWM_CHIP "/pwm0/period", PWM_PERIOD);
	write_int(PWM_CHIP "/pwm0/duty_cycle", on ? duty : 0);
	write_int(PWM_CHIP "/pwm0/enable", 1);
	gpio_out(GPIO_IR_EN, on && duty > 0);
}

/* Wi-Fi associated and an IPv4 address assigned? */
static int network_up(void)
{
	char state[16] = { 0 };
	struct ifreq ifr;
	int fd, ok;

	fd = open("/sys/class/net/wlan0/operstate", O_RDONLY);
	if (fd < 0)
		return 0;
	ok = read(fd, state, sizeof(state) - 1) > 0 && !strncmp(state, "up", 2);
	close(fd);
	if (!ok)
		return 0;
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return 0;
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "wlan0");
	ok = !ioctl(fd, SIOCGIFADDR, &ifr) &&
	     ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr.s_addr != 0;
	close(fd);
	return ok;
}

/* status: blink 1 Hz while the network is down, solid once it's up */
static void update_led(void)
{
	static int last = -1, last_net = -1;
	int on, net;

	if (!strcmp(cfg.led_mode, "off")) {
		on = 0;
	} else if (!strcmp(cfg.led_mode, "on")) {
		on = 1;
	} else {
		net = network_up();
		if (net != last_net) {
			LOG("led: network %s", net ? "up" : "down");
			last_net = net;
		}
		on = net || (now_us() / 500000) % 2;
	}
	if (on != last)
		gpio_out(GPIO_LED, on);
	last = on;
}

static void set_night(int night, int ldr)
{
	if (night == is_night)
		return;
	is_night = night;
	LOG("nightmode: %s (light sensor %d)", night ? "NIGHT" : "DAY", ldr);
	ircut_set(!night);
	ir_leds(night);
	if (cfg.ir_gray)
		video_set_gray(night);
}

static void *loop(void *arg)
{
	int avg = -1, pending = 0;
	uint64_t since = 0;
	(void)arg;

	while (!g_quit) {
		int v = read_ldr();
		update_led();
		/* light EMA (alpha 1/4, x8 fixed point): ~1 s time constant at 250 ms;
		 * the sensor is only a few counts noisy */
		if (v >= 0)
			avg = avg < 0 ? v * 8 : avg + (v * 8 - avg) / 4;

		if (!strcmp(cfg.ir_mode, "night")) {
			set_night(1, avg / 8);
		} else if (!strcmp(cfg.ir_mode, "day")) {
			set_night(0, avg / 8);
		} else if (avg >= 0) {
			int want = is_night;
			if (avg / 8 >= cfg.ir_night_threshold)
				want = 1;
			else if (avg / 8 <= cfg.ir_day_threshold)
				want = 0;
			if (is_night < 0) {
				set_night(want < 0 ? 0 : want, avg / 8); /* first decision: immediate */
				pending = is_night;
			} else if (want != is_night) {
				/* must persist for ir_switch_delay seconds */
				if (pending != want) {
					pending = want;
					since = now_us();
				} else if (now_us() - since >= (uint64_t)cfg.ir_switch_delay * 1000000) {
					set_night(want, avg / 8);
				}
			} else {
				pending = is_night;
			}
		}
		usleep((cfg.ir_sample_ms > 50 ? cfg.ir_sample_ms : 50) * 1000);
	}
	return NULL;
}

int nightmode_start(void)
{
	ircut_init();
	running = !pthread_create(&thread, NULL, loop, NULL);
	LOG("nightmode: mode %s, brightness %d%%, night>=%d day<=%d, delay %ds, ir-cut %s",
	    cfg.ir_mode, cfg.ir_brightness, cfg.ir_night_threshold, cfg.ir_day_threshold,
	    cfg.ir_switch_delay, tmi_fd >= 0 ? "on" : "off");
	return running ? 0 : -1;
}

void nightmode_stop(void)
{
	if (running)
		pthread_join(thread, NULL);
	running = 0;
	ir_leds(0);
	ircut_close();
	gpio_out(GPIO_LED, 0);
	is_night = -1;
}
