/* Thread-safe wrapper around Rockchip's prebuilt librtsp ("rtsp_demo").
 * One session: video (H.264/H.265) + optional G.711 A-law audio. */
#include <pthread.h>
#include <string.h>
#include <unistd.h>

#include "camd.h"
#include "rtsp_demo.h"

static rtsp_demo_handle demo;
static rtsp_session_handle session;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t poll_thread;
static int poll_running;

static void *poll_loop(void *arg)
{
	(void)arg;
	while (!g_quit) {
		rtsp_poll();
		usleep(5 * 1000);
	}
	return NULL;
}

int rtsp_start(void)
{
	int codec = !strcmp(cfg.video_codec, "h265") ? RTSP_CODEC_ID_VIDEO_H265
						      : RTSP_CODEC_ID_VIDEO_H264;

	demo = create_rtsp_demo(cfg.rtsp_port);
	if (!demo) {
		ERR("rtsp: cannot listen on port %d", cfg.rtsp_port);
		return -1;
	}
	session = rtsp_new_session(demo, cfg.rtsp_path);
	if (!session) {
		ERR("rtsp: cannot create session %s", cfg.rtsp_path);
		return -1;
	}
	rtsp_set_video(session, codec, NULL, 0);
	if (cfg.mic_enabled) {
		rtsp_set_audio(session, RTSP_CODEC_ID_AUDIO_G711A, NULL, 0);
		rtsp_set_audio_sample_rate(session, 8000);
		rtsp_set_audio_channels(session, 1);
	}
	rtsp_sync_video_ts(session, rtsp_get_reltime(), rtsp_get_ntptime());
	if (cfg.mic_enabled)
		rtsp_sync_audio_ts(session, rtsp_get_reltime(), rtsp_get_ntptime());

	poll_running = !pthread_create(&poll_thread, NULL, poll_loop, NULL);
	LOG("rtsp: listening on port %d path %s (%s%s)", cfg.rtsp_port, cfg.rtsp_path,
	    cfg.video_codec, cfg.mic_enabled ? " + pcma" : "");
	return 0;
}

void rtsp_stop(void)
{
	if (poll_running)
		pthread_join(poll_thread, NULL);
	poll_running = 0;
	pthread_mutex_lock(&lock);
	if (demo)
		rtsp_del_demo(demo);
	demo = NULL;
	session = NULL;
	pthread_mutex_unlock(&lock);
}

void rtsp_send_video(const uint8_t *data, int len, uint64_t pts_us)
{
	pthread_mutex_lock(&lock);
	if (session)
		rtsp_tx_video(session, data, len, pts_us);
	pthread_mutex_unlock(&lock);
}

void rtsp_send_audio(const uint8_t *data, int len, uint64_t pts_us)
{
	pthread_mutex_lock(&lock);
	if (session)
		rtsp_tx_audio(session, data, len, pts_us);
	pthread_mutex_unlock(&lock);
}

void rtsp_poll(void)
{
	pthread_mutex_lock(&lock);
	if (demo)
		rtsp_do_event(demo);
	pthread_mutex_unlock(&lock);
}
