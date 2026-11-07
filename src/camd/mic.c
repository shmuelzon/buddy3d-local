/* Microphone -> G.711 A-law 8 kHz -> RTSP audio track */
#include <pthread.h>
#include <string.h>

#include "camd.h"
#include "rk_mpi_ai.h"
#include "rk_mpi_mb.h"

#define AI_DEV 0
#define AI_CHN 0
#define AI_HW_RATE 16000
#define AI_OUT_RATE 8000      /* G.711; decimated 2:1 in software (Rockit's
                                 resampler yields no frames on this firmware) */
#define AI_FRAME_SAMPLES 640  /* 40 ms @ 16 kHz */

static int mic_open(void);
static pthread_t thread;
static int running, ai_on, ai_chn_on;

static void *mic_loop(void *arg)
{
	static uint8_t alaw[4096];
	uint64_t frames = 0, fails = 0, t0 = now_us();
	unsigned last_len = 0;
	int prev = 0;
	int first = 1;
	(void)arg;
	while (!g_quit) {
		AUDIO_FRAME_S f;
		int ret;
		memset(&f, 0, sizeof(f));
		if ((ret = RK_MPI_AI_GetFrame(AI_DEV, AI_CHN, &f, NULL, 500)) != RK_SUCCESS) {
			if (!fails++)
				ERR("mic: GetFrame 0x%x", ret);
			continue;
		}
		frames++;
		last_len = f.u32Len;
		if (first || now_us() - t0 >= 60 * 1000000ULL) {
			LOG("mic: %llu frames, %llu timeouts, frame %u bytes",
			    (unsigned long long)frames, (unsigned long long)fails, last_len);
			frames = fails = 0;
			first = 0;
			t0 = now_us();
		}
		const int16_t *pcm = RK_MPI_MB_Handle2VirAddr(f.pMbBlk);
		int n = f.u32Len / 4; /* stereo frames */
		if (pcm && n > 0) {
			int m = 0, ch = cfg.mic_channel & 1;
			/* gain in 1/256 steps: 10^(dB/20) */
			int g = 256;
			for (int k = 0; k < cfg.mic_gain_db; k += 2) g = g * 323 / 256; /* +2 dB */
			if (n > (int)sizeof(alaw) * 2)
				n = sizeof(alaw) * 2;
			/* take one channel, [1 2 1]/4 low-pass, keep every 2nd sample */
			for (int i = 0; i + 1 < n; i += 2) {
				int a = pcm[2 * i + ch], b = pcm[2 * (i + 1) + ch];
				int v = (prev + 2 * a + b) / 4 * g / 256;
				prev = b;
				if (v > 32767) v = 32767;
				if (v < -32768) v = -32768;
				alaw[m++] = linear_to_alaw(v);
			}
			rtsp_send_audio(alaw, m, now_us());
		}
		RK_MPI_AI_ReleaseFrame(AI_DEV, AI_CHN, &f, NULL);
	}
	return NULL;
}

/* Debug: capture raw interleaved stereo from the codec to a file. */
int mic_record_raw(const char *path, int seconds)
{
	FILE *out = fopen(path, "wb");
	uint64_t end;
	if (!out)
		return -1;
	if (mic_open()) {
		fclose(out);
		return -1;
	}
	end = now_us() + (uint64_t)seconds * 1000000;
	while (now_us() < end) {
		AUDIO_FRAME_S f;
		memset(&f, 0, sizeof(f));
		if (RK_MPI_AI_GetFrame(AI_DEV, AI_CHN, &f, NULL, 500) != RK_SUCCESS)
			continue;
		void *p = RK_MPI_MB_Handle2VirAddr(f.pMbBlk);
		if (p)
			fwrite(p, 1, f.u32Len, out);
		RK_MPI_AI_ReleaseFrame(AI_DEV, AI_CHN, &f, NULL);
	}
	fclose(out);
	mic_stop();
	return 0;
}

int mic_start(void)
{
	if (mic_open())
		return -1;
	running = !pthread_create(&thread, NULL, mic_loop, NULL);
	LOG("mic: capturing %d Hz -> pcma %d Hz", AI_HW_RATE, AI_OUT_RATE);
	return running ? 0 : -1;
}

static int mic_open(void)
{
	AIO_ATTR_S a;
	AI_CHN_PARAM_S p;
	int ret;

	memset(&a, 0, sizeof(a));
	snprintf((char *)a.u8CardName, sizeof(a.u8CardName), "hw:0,0");
	a.soundCard.channels = 2;
	a.soundCard.sampleRate = AI_HW_RATE;
	a.soundCard.bitWidth = AUDIO_BIT_WIDTH_16;
	a.enBitwidth = AUDIO_BIT_WIDTH_16;
	a.enSamplerate = (AUDIO_SAMPLE_RATE_E)AI_HW_RATE;
	/* The codec delivers interleaved L/R; only one side carries the mic (the
	 * other is the I2S loopback slot), so capture stereo and pick a channel. */
	a.enSoundmode = AUDIO_SOUND_MODE_STEREO;
	a.u32PtNumPerFrm = AI_FRAME_SAMPLES;
	a.u32FrmNum = 4;
	a.u32ChnCnt = 2;
	if ((ret = RK_MPI_AI_SetPubAttr(AI_DEV, &a)) || (ret = RK_MPI_AI_Enable(AI_DEV))) {
		ERR("mic: AI enable 0x%x", ret);
		return -1;
	}
	ai_on = 1;
	memset(&p, 0, sizeof(p));
	p.s32UsrFrmDepth = 4;
	if ((ret = RK_MPI_AI_SetChnParam(AI_DEV, AI_CHN, &p)) ||
	    (ret = RK_MPI_AI_EnableChn(AI_DEV, AI_CHN))) {
		ERR("mic: AI chn 0x%x", ret);
		mic_stop();
		return -1;
	}
	ai_chn_on = 1;
	RK_MPI_AI_SetVolume(AI_DEV, cfg.mic_volume);
	return 0;
}

void mic_stop(void)
{
	if (running)
		pthread_join(thread, NULL);
	running = 0;
	if (ai_chn_on)
		RK_MPI_AI_DisableChn(AI_DEV, AI_CHN);
	if (ai_on)
		RK_MPI_AI_Disable(AI_DEV);
	ai_chn_on = ai_on = 0;
}
