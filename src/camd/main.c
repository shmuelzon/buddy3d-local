/* camd - minimal local-only camera daemon for the Prusa Buddy3D camera (RV1103)
 *
 * Video:   sensor -> ISP (rkaiq) -> VI -> VENC (H.264/H.265) -> RTSP
 * Mic:     AI -> G.711 A-law -> RTSP audio track
 * Speaker: TCP/HTTP audio sink -> AO
 */
#include <ctype.h>
#include <getopt.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "camd.h"
#include "rk_mpi_amix.h"
#include "rk_mpi_sys.h"

struct config cfg = {
	.video_codec = "h264",
	.video_width = 1920,
	.video_height = 1080,
	.video_fps = 0, /* 0 = whatever the sensor delivers */
	.video_bitrate = 2048,
	.video_gop = 0,
	.video_rc = "cbr",
	.video_flip = 0,
	.iq_dir = "/etc/iqfiles",
	.rtsp_port = 554,
	.rtsp_path = "/live",
	.mic_enabled = 0, /* the Buddy3D has no microphone fitted */
	.mic_volume = 80,
	.speaker_enabled = 1,
	.speaker_port = 7777,
	.speaker_volume = 80,
	.mic_gain_db = 12,
	.audio_mixer = "",
	.ir_mode = "auto",
	.ir_brightness = 80,
	.ir_night_threshold = 1005,
	.ir_day_threshold = 985,
	.ir_switch_delay = 3,
	.ir_sample_ms = 250,
	.ir_gray = 1,
	.ir_cut = 1,
	.ir_cut_pulse_ms = 200,
	.led_mode = "status",
	.osd_text = "",
	.osd_text_position = "tl",
	.osd_time = "",
	.osd_time_position = "tr",
	.osd_font_size = 32,
};

atomic_bool g_quit = false;

const char *log_time(void)
{
	static __thread char buf[16];
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
	return buf;
}

uint64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

#define STR(f) { #f, 's', cfg.f, sizeof(cfg.f) }
#define INT(f) { #f, 'i', &cfg.f, 0 }
static const struct { const char *key; char type; void *ptr; size_t size; } keys[] = {
	STR(video_codec), INT(video_width), INT(video_height), INT(video_fps),
	INT(video_bitrate), INT(video_gop), STR(video_rc), INT(video_flip), STR(iq_dir),
	INT(rtsp_port), STR(rtsp_path),
	INT(mic_enabled), INT(mic_volume), INT(mic_channel), INT(mic_gain_db),
	STR(ir_mode), INT(ir_brightness), INT(ir_night_threshold), INT(ir_day_threshold),
	INT(ir_switch_delay), INT(ir_sample_ms), INT(ir_gray), INT(ir_cut), INT(ir_cut_pulse_ms), STR(led_mode),
	STR(osd_text), STR(osd_text_position), STR(osd_time), STR(osd_time_position),
	INT(osd_font_size),
	INT(speaker_enabled), INT(speaker_port), INT(speaker_volume), STR(audio_mixer),
};

static char *trim(char *s)
{
	char *e;
	while (isspace((unsigned char)*s)) s++;
	e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
	return s;
}

static int set_key(const char *key, const char *val)
{
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		if (strcasecmp(key, keys[i].key))
			continue;
		if (keys[i].type == 'i')
			*(int *)keys[i].ptr = atoi(val);
		else
			snprintf(keys[i].ptr, keys[i].size, "%s", val);
		return 0;
	}
	return -1; /* not ours (e.g. wifi_ssid, used by the boot script) */
}

/* Shell-style KEY=VALUE file, shared with the boot script. Unknown keys are
 * ignored. Values may be single- or double-quoted. */
int config_load(const char *path)
{
	char line[512];
	FILE *f = fopen(path, "r");
	if (!f) {
		ERR("cannot open config %s", path);
		return -1;
	}
	while (fgets(line, sizeof(line), f)) {
		char *s = trim(line), *eq, *val;
		size_t n;
		if (!*s || *s == '#')
			continue;
		if (!strncmp(s, "export ", 7))
			s = trim(s + 7);
		if (!(eq = strchr(s, '=')))
			continue;
		*eq = 0;
		val = trim(eq + 1);
		n = strlen(val);
		if (n >= 2 && (val[0] == '"' || val[0] == '\'') && val[n - 1] == val[0]) {
			val[n - 1] = 0;
			val++;
		}
		set_key(trim(s), val);
	}
	fclose(f);
	return 0;
}

void config_dump(void)
{
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		if (keys[i].type == 'i')
			LOG("  %s=%d", keys[i].key, *(int *)keys[i].ptr);
		else
			LOG("  %s=%s", keys[i].key, (char *)keys[i].ptr);
	}
}

static void apply_mixer(void)
{
	char buf[sizeof(cfg.audio_mixer)], *save = NULL, *item;
	snprintf(buf, sizeof(buf), "%s", cfg.audio_mixer);
	for (item = strtok_r(buf, ";", &save); item; item = strtok_r(NULL, ";", &save)) {
		char *eq = strchr(item, '=');
		if (!eq)
			continue;
		*eq = 0;
		item = trim(item);
		if (RK_MPI_AMIX_SetControl(0, item, trim(eq + 1)))
			ERR("mixer: cannot set '%s'", item);
		else
			LOG("mixer: %s = %s", item, trim(eq + 1));
	}
}

static void on_signal(int sig)
{
	(void)sig;
	g_quit = true;
	alarm(3); /* if an MPI call hangs during teardown, SIGALRM terminates us */
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [-c config] [-o key=value]...\n"
		"  -c FILE       load KEY=VALUE config file\n"
		"  -o KEY=VALUE  override a single setting (after -c)\n"
		"  -p            print effective config and exit\n"
		"  -A            list audio codec mixer controls and exit\n"
		"  -R FILE:SECS  record raw stereo s16le 16 kHz from the codec and exit\n", argv0);
}

int main(int argc, char **argv)
{
	int c, print_only = 0;

	setvbuf(stdout, NULL, _IOLBF, 0);
	setvbuf(stderr, NULL, _IOLBF, 0);

	char *record = NULL;
	while ((c = getopt(argc, argv, "c:o:pAR:h")) != -1) {
		switch (c) {
		case 'c':
			if (config_load(optarg))
				return 1;
			break;
		case 'o': {
			char buf[256], *eq;
			snprintf(buf, sizeof(buf), "%s", optarg);
			if (!(eq = strchr(buf, '=')) || (*eq = 0, set_key(buf, eq + 1))) {
				ERR("bad option '%s'", optarg);
				return 1;
			}
			break;
		}
		case 'p':
			print_only = 1;
			break;
		case 'R':
			record = optarg;
			break;
		case 'A':
			RK_MPI_SYS_Init();
			RK_MPI_AMIX_ListContents(0);
			RK_MPI_SYS_Exit();
			return 0;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	LOG("starting with config:");
	config_dump();
	if (print_only)
		return 0;

	if (record) {
		char *colon = strrchr(record, ':');
		int secs = colon ? atoi(colon + 1) : 5;
		if (colon)
			*colon = 0;
		RK_MPI_SYS_Init();
		apply_mixer();
		mic_record_raw(record, secs);
		RK_MPI_SYS_Exit();
		return 0;
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, SIG_IGN);

	if (RK_MPI_SYS_Init() != 0) {
		ERR("RK_MPI_SYS_Init failed");
		return 1;
	}

	apply_mixer();
	if (rtsp_start())
		goto out;
	if (video_start())
		goto out;
	osd_start();
	if (cfg.mic_enabled && mic_start())
		ERR("mic disabled (init failed)");
	if (cfg.speaker_enabled && speaker_start())
		ERR("speaker disabled (init failed)");
	if (nightmode_start())
		ERR("nightmode disabled (init failed)");

	LOG("running: rtsp://<ip>:%d%s", cfg.rtsp_port, cfg.rtsp_path);
	while (!g_quit)
		usleep(200 * 1000);

	LOG("shutting down");
out:
	g_quit = true;
	nightmode_stop();
	speaker_stop();
	mic_stop();
	osd_stop();
	video_stop();
	rtsp_stop();
	RK_MPI_SYS_Exit();
	return 0;
}
