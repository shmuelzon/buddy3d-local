/* camd - minimal local-only camera daemon for the Prusa Buddy3D camera (RV1103) */
#ifndef CAMD_H
#define CAMD_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

const char *log_time(void);
#define LOG(fmt, ...)  fprintf(stderr, "%s camd: " fmt "\n", log_time(), ##__VA_ARGS__)
#define ERR(fmt, ...)  fprintf(stderr, "%s camd: ERROR: " fmt "\n", log_time(), ##__VA_ARGS__)

struct config {
	/* video */
	char video_codec[8];     /* h264 | h265 */
	int  video_width;
	int  video_height;
	int  video_fps;          /* 0 = sensor rate; lower = drop frames in the encoder */
	int  video_bitrate;      /* kbps */
	int  video_gop;          /* frames; 0 = 2 * fps */
	char video_rc[4];        /* cbr | vbr */
	int  video_flip;         /* 0 none, 1 mirror, 2 flip, 3 both (rotate 180) */
	char iq_dir[128];

	/* rtsp */
	int  rtsp_port;
	char rtsp_path[64];

	/* microphone -> RTSP audio track (G.711 A-law 8 kHz) */
	int  mic_enabled;
	int  mic_volume;         /* 0..100 */
	int  mic_channel;        /* 0 = left, 1 = right slot of the I2S frame */
	int  mic_gain_db;        /* software gain applied before encoding */

	/* night mode / LED */
	char ir_mode[8];         /* auto | day | night */
	int  ir_brightness;      /* 0..100 % */
	int  ir_night_threshold; /* light sensor >= this -> night (higher = darker) */
	int  ir_day_threshold;   /* light sensor <= this -> day */
	int  ir_switch_delay;    /* seconds a new state must persist */
	int  ir_sample_ms;       /* light sensor sampling interval */
	int  ir_gray;            /* switch ISP to its "gray" (B/W) scene at night */
	int  ir_cut;             /* drive the IR-cut filter (TMI8150B) */
	int  ir_cut_pulse_ms;    /* coil pulse length */
	char led_mode[8];        /* status (blink until Wi-Fi up, then solid) | on | off */

	/* on-screen display */
	char osd_text[96];
	char osd_text_position[4];  /* tl | tr | bl | br */
	char osd_time[64];          /* strftime() format, empty = no timestamp */
	char osd_time_position[4];
	int  osd_font_size;         /* 16 | 32 | 64 (pixel height) */

	/* codec mixer controls, "Name=Value;Name=Value" (see camd -A) */
	char audio_mixer[512];

	/* network audio -> speaker */
	int  speaker_enabled;
	int  speaker_port;
	int  speaker_volume;     /* 0..100 */
};

extern struct config cfg;
extern atomic_bool g_quit;

int  config_load(const char *path);
void config_dump(void);

/* rtsp.c: thread-safe wrapper around Rockchip's librtsp */
int  rtsp_start(void);
void rtsp_stop(void);
void rtsp_send_video(const uint8_t *data, int len, uint64_t pts_us);
void rtsp_send_audio(const uint8_t *data, int len, uint64_t pts_us);
void rtsp_poll(void);

/* video.c */
int  video_start(void);
void video_stop(void);

/* nightmode.c */
int  nightmode_start(void);
void nightmode_stop(void);
void video_set_gray(int gray);

/* osd.c */
int  osd_start(void);
void osd_stop(void);

/* speaker.c */
int  speaker_start(void);
void speaker_stop(void);

/* mic.c */
int  mic_start(void);
int  mic_record_raw(const char *path, int seconds);
void mic_stop(void);

/* g711.c */
uint8_t linear_to_alaw(int16_t pcm);
int16_t alaw_to_linear(uint8_t a);
int16_t ulaw_to_linear(uint8_t u);

uint64_t now_us(void);

#endif
