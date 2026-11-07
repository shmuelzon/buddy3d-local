/* Network audio -> speaker.
 *
 * One TCP port accepts any of:
 *   - raw PCM (default s16le, 16 kHz, mono), e.g.
 *       ffmpeg -i x.mp3 -f s16le -ar 16000 -ac 1 - | nc cam 7777
 *   - a WAV stream/file (PCM 8/16-bit, A-law or u-law; any rate/channels)
 *       nc cam 7777 < x.wav
 *   - HTTP POST/PUT with optional query: ?format=s16le|s16be|alaw|ulaw&rate=N&channels=N&volume=N
 *       curl --data-binary @x.wav http://cam:7777/
 *       ffmpeg -i x.mp3 -f alaw -ar 8000 -ac 1 - | curl -T - 'http://cam:7777/?format=alaw&rate=8000'
 *     (Content-Length or chunked; reply sent when playback finishes)
 *
 * A new connection pre-empts the one currently playing (latest talker wins).
 */
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "camd.h"
#include "rk_mpi_ao.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"

#define AO_DEV 0
#define AO_CHN 0
#define AO_HW_RATE 16000
#define AO_FRAME_BYTES 1024

atomic_bool g_speaker_active = false;

enum fmt { F_S16LE, F_S16BE, F_U8, F_ALAW, F_ULAW };
struct stream_fmt { enum fmt fmt; int rate, channels; };

struct reader {
	int fd, lfd;
	uint8_t buf[4096];
	int pos, len;
	long long remaining;   /* body bytes left (HTTP Content-Length), -1 = until EOF */
	int chunked;           /* HTTP chunked transfer encoding */
	long long chunk_left;
	int preempt_fd;        /* set when a new client arrived */
	uint8_t pb[4];         /* pushed-back body bytes (format sniffing) */
	int pb_len, pb_pos;
};

static int listen_fd = -1;
static pthread_t thread;
static int running;

/* ---------------------------------------------------------------- AO ---- */

static int ao_open(int rate)
{
	AIO_ATTR_S a;
	AO_CHN_PARAM_S p;
	int ret;

	memset(&a, 0, sizeof(a));
	snprintf((char *)a.u8CardName, sizeof(a.u8CardName), "hw:0,0");
	a.soundCard.channels = 2;
	a.soundCard.sampleRate = AO_HW_RATE;
	a.soundCard.bitWidth = AUDIO_BIT_WIDTH_16;
	a.enBitwidth = AUDIO_BIT_WIDTH_16;
	a.enSamplerate = (AUDIO_SAMPLE_RATE_E)rate;
	a.enSoundmode = AUDIO_SOUND_MODE_MONO;
	a.u32PtNumPerFrm = AO_FRAME_BYTES / 2;
	a.u32FrmNum = 4;
	a.u32ChnCnt = 2;
	if ((ret = RK_MPI_AO_SetPubAttr(AO_DEV, &a)) || (ret = RK_MPI_AO_Enable(AO_DEV))) {
		ERR("speaker: AO enable 0x%x", ret);
		return -1;
	}
	memset(&p, 0, sizeof(p));
	p.enLoopbackMode = AUDIO_LOOPBACK_NONE;
	RK_MPI_AO_SetChnParams(AO_DEV, AO_CHN, &p);
	RK_MPI_AO_SetTrackMode(AO_DEV, AUDIO_TRACK_OUT_STEREO);
	if ((ret = RK_MPI_AO_EnableChn(AO_DEV, AO_CHN))) {
		ERR("speaker: AO enable chn 0x%x", ret);
		RK_MPI_AO_Disable(AO_DEV);
		return -1;
	}
	if (rate != AO_HW_RATE && (ret = RK_MPI_AO_EnableReSmp(AO_DEV, AO_CHN,
							       (AUDIO_SAMPLE_RATE_E)rate)))
		ERR("speaker: resample %d->%d failed 0x%x", rate, AO_HW_RATE, ret);
	RK_MPI_AO_SetVolume(AO_DEV, cfg.speaker_volume);
	g_speaker_active = true;
	return 0;
}

static int ao_send(const int16_t *pcm, int samples);

static void ao_close(int drain, int rate)
{
	if (drain) {
		static int16_t eos;
		ao_send(&eos, 0);                         /* zero-length frame = EOS */
		RK_MPI_AO_WaitEos(AO_DEV, AO_CHN, 5000);
	} else
		RK_MPI_AO_ClearChnBuf(AO_DEV, AO_CHN);
	if (rate != AO_HW_RATE)
		RK_MPI_AO_DisableReSmp(AO_DEV, AO_CHN);
	RK_MPI_AO_DisableChn(AO_DEV, AO_CHN);
	RK_MPI_AO_Disable(AO_DEV);
	g_speaker_active = false;
}

static int ao_send(const int16_t *pcm, int samples)
{
	AUDIO_FRAME_S f;
	MB_EXT_CONFIG_S ext;
	int ret;

	memset(&f, 0, sizeof(f));
	memset(&ext, 0, sizeof(ext));
	ext.pOpaque = (void *)pcm;
	ext.pu8VirAddr = (RK_U8 *)pcm;
	ext.u64Size = samples * 2;
	if (RK_MPI_SYS_CreateMB(&f.pMbBlk, &ext))
		return -1;
	f.u32Len = samples * 2;
	f.enBitWidth = AUDIO_BIT_WIDTH_16;
	f.enSoundMode = AUDIO_SOUND_MODE_MONO;
	f.bBypassMbBlk = RK_FALSE;
	ret = RK_MPI_AO_SendFrame(AO_DEV, AO_CHN, &f, 1000);
	RK_MPI_MB_ReleaseMB(f.pMbBlk);
	return ret;
}

/* ------------------------------------------------------------ reader ---- */

/* Fill the buffer; returns bytes available or 0 on EOF/error/preempt. Watches
 * the listening socket so a new client can pre-empt the current one. */
static int fill(struct reader *r)
{
	struct pollfd p[2] = { { r->fd, POLLIN, 0 }, { r->lfd, POLLIN, 0 } };

	if (r->pos < r->len)
		return r->len - r->pos;
	r->pos = r->len = 0;
	while (!g_quit) {
		if (poll(p, 2, 500) < 0) {
			if (errno == EINTR)
				continue;
			return 0;
		}
		if (p[1].revents & POLLIN) {
			r->preempt_fd = accept(r->lfd, NULL, NULL);
			if (r->preempt_fd >= 0)
				return 0;
		}
		if (p[0].revents & (POLLIN | POLLHUP | POLLERR)) {
			int n = read(r->fd, r->buf, sizeof(r->buf));
			if (n <= 0)
				return 0;
			r->len = n;
			return n;
		}
	}
	return 0;
}

/* Raw byte read (headers, chunk framing). */
static int rd(struct reader *r, void *dst, int n)
{
	int got = 0;
	while (got < n) {
		int avail = fill(r);
		if (!avail)
			break;
		if (avail > n - got)
			avail = n - got;
		memcpy((uint8_t *)dst + got, r->buf + r->pos, avail);
		r->pos += avail;
		got += avail;
	}
	return got;
}

static int rd_line(struct reader *r, char *line, int max)
{
	int n = 0;
	char c;
	while (rd(r, &c, 1) == 1) {
		if (c == '\n') {
			if (n && line[n - 1] == '\r')
				n--;
			line[n] = 0;
			return n;
		}
		if (n < max - 1)
			line[n++] = c;
	}
	line[n] = 0;
	return n ? n : -1;
}

/* Body read: honours Content-Length and chunked encoding. */
static int body(struct reader *r, void *dst, int n)
{
	int got = 0;
	while (got < n && r->pb_pos < r->pb_len)
		((uint8_t *)dst)[got++] = r->pb[r->pb_pos++];
	while (got < n) {
		int want = n - got, k;
		if (r->chunked) {
			if (r->chunk_left == 0) {
				char line[32];
				if (rd_line(r, line, sizeof(line)) == 0) /* CRLF after chunk */
					rd_line(r, line, sizeof(line));
				r->chunk_left = strtoll(line, NULL, 16);
				if (r->chunk_left <= 0) {
					r->chunked = 0;
					r->remaining = 0;
					break;
				}
			}
			if (want > r->chunk_left)
				want = r->chunk_left;
		} else if (r->remaining >= 0) {
			if (r->remaining == 0)
				break;
			if (want > r->remaining)
				want = r->remaining;
		}
		k = rd(r, (uint8_t *)dst + got, want);
		if (k <= 0)
			break;
		got += k;
		if (r->chunked)
			r->chunk_left -= k;
		else if (r->remaining > 0)
			r->remaining -= k;
	}
	return got;
}

static int peek(struct reader *r, void *dst, int n)
{
	/* make sure n bytes are contiguous in the buffer */
	if (r->len - r->pos < n) {
		memmove(r->buf, r->buf + r->pos, r->len - r->pos);
		r->len -= r->pos;
		r->pos = 0;
		while (r->len < n) {
			int k = read(r->fd, r->buf + r->len, sizeof(r->buf) - r->len);
			if (k <= 0)
				break;
			r->len += k;
		}
	}
	if (r->len - r->pos < n)
		return 0;
	memcpy(dst, r->buf + r->pos, n);
	return n;
}

/* ------------------------------------------------------------ parsing --- */

static void parse_query(const char *q, struct stream_fmt *f, int *volume)
{
	char key[32], val[32];
	while (q && *q) {
		int n = 0, m = 0;
		while (*q && *q != '=' && *q != '&' && n < 31) key[n++] = *q++;
		key[n] = 0;
		if (*q == '=') q++;
		while (*q && *q != '&' && m < 31) val[m++] = *q++;
		val[m] = 0;
		if (*q == '&') q++;
		if (!strcmp(key, "format")) {
			if (!strcmp(val, "s16le")) f->fmt = F_S16LE;
			else if (!strcmp(val, "s16be")) f->fmt = F_S16BE;
			else if (!strcmp(val, "u8")) f->fmt = F_U8;
			else if (!strcmp(val, "alaw") || !strcmp(val, "pcma")) f->fmt = F_ALAW;
			else if (!strcmp(val, "ulaw") || !strcmp(val, "pcmu")) f->fmt = F_ULAW;
		} else if (!strcmp(key, "rate")) {
			f->rate = atoi(val);
		} else if (!strcmp(key, "channels")) {
			f->channels = atoi(val);
		} else if (!strcmp(key, "volume")) {
			*volume = atoi(val);
		}
	}
}

static int parse_http(struct reader *r, struct stream_fmt *f, int *volume)
{
	char line[1024], *q;
	if (rd_line(r, line, sizeof(line)) < 0)
		return -1;
	if ((q = strchr(line, '?'))) {
		char *sp = strchr(q, ' ');
		if (sp) *sp = 0;
		parse_query(q + 1, f, volume);
	}
	r->remaining = -1;
	while (rd_line(r, line, sizeof(line)) > 0) {
		if (!strncasecmp(line, "Content-Length:", 15))
			r->remaining = atoll(line + 15);
		else if (!strncasecmp(line, "Transfer-Encoding:", 18) && strstr(line, "chunked"))
			r->chunked = 1;
		else if (!strncasecmp(line, "Expect:", 7) && strstr(line, "100-continue")) {
			const char *c = "HTTP/1.1 100 Continue\r\n\r\n";
			if (write(r->fd, c, strlen(c)) < 0) {}
		}
	}
	return 0;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }

static int parse_wav(struct reader *r, struct stream_fmt *f)
{
	uint8_t h[16];
	if (body(r, h, 8) != 8) /* rest of RIFF header; "RIFF" already consumed */
		return -1;
	for (;;) {
		uint32_t sz;
		if (body(r, h, 8) != 8)
			return -1;
		sz = le32(h + 4);
		if (!memcmp(h, "data", 4))
			return 0;
		if (!memcmp(h, "fmt ", 4) && sz >= 16) {
			uint8_t fm[16];
			int bits;
			if (body(r, fm, 16) != 16)
				return -1;
			f->channels = le16(fm + 2);
			f->rate = le32(fm + 4);
			bits = le16(fm + 14);
			switch (le16(fm)) {
			case 1: f->fmt = bits == 8 ? F_U8 : F_S16LE; break;
			case 6: f->fmt = F_ALAW; break;
			case 7: f->fmt = F_ULAW; break;
			case 0xfffe: f->fmt = bits == 8 ? F_U8 : F_S16LE; break; /* extensible PCM */
			default:
				ERR("speaker: unsupported WAV format %d", le16(fm));
				return -1;
			}
			sz -= 16;
		}
		/* skip rest of chunk (word aligned) */
		for (sz += sz & 1; sz > 0;) {
			uint8_t tmp[64];
			int k = body(r, tmp, sz > sizeof(tmp) ? sizeof(tmp) : sz);
			if (k <= 0)
				return -1;
			sz -= k;
		}
	}
}

/* ------------------------------------------------------------ client ---- */

/* Plays one client; returns a pre-empting client's fd, or -1. */
static int play_client(int fd)
{
	struct reader *r = calloc(1, sizeof(*r));
	struct stream_fmt f = { F_S16LE, 16000, 1 };
	int volume = cfg.speaker_volume, http = 0, opened = 0, next = -1;
	int bps, frame_samples;
	uint8_t magic[4];
	static uint8_t in[AO_FRAME_BYTES / 2 * 8 * 2]; /* up to 8 ch x 16-bit */
	static int16_t out[AO_FRAME_BYTES / 2];
	long long total = 0;

	if (!r)
		return -1;
	r->fd = fd;
	r->lfd = listen_fd;
	r->remaining = -1;
	r->preempt_fd = -1;

	if (peek(r, magic, 4) == 4 && (!memcmp(magic, "POST", 4) || !memcmp(magic, "PUT ", 4))) {
		http = 1;
		if (parse_http(r, &f, &volume))
			goto done;
	}
	/* body may itself be a WAV file; otherwise push the sniffed bytes back */
	{
		int m = body(r, magic, 4);
		if (m == 4 && !memcmp(magic, "RIFF", 4)) {
			if (parse_wav(r, &f))
				goto done;
		} else {
			memcpy(r->pb, magic, m > 0 ? m : 0);
			r->pb_len = m > 0 ? m : 0;
		}
	}

	if (f.channels < 1 || f.channels > 8 || f.rate < 8000 || f.rate > 48000) {
		ERR("speaker: bad stream params rate=%d channels=%d", f.rate, f.channels);
		goto done;
	}
	bps = (f.fmt == F_S16LE || f.fmt == F_S16BE) ? 2 : 1;
	frame_samples = AO_FRAME_BYTES / 2;
	LOG("speaker: playing fmt=%d rate=%d ch=%d%s", f.fmt, f.rate, f.channels,
	    http ? " (http)" : "");

	if (ao_open(f.rate))
		goto done;
	opened = 1;
	if (volume != cfg.speaker_volume)
		RK_MPI_AO_SetVolume(AO_DEV, volume);

	for (;;) {
		int want = frame_samples * f.channels * bps, n, samples, i, c;
		n = body(r, in, want);
		n -= n % (f.channels * bps);
		if (n <= 0)
			break;
		samples = n / (f.channels * bps);
		for (i = 0; i < samples; i++) {
			int acc = 0;
			for (c = 0; c < f.channels; c++) {
				const uint8_t *p = in + (i * f.channels + c) * bps;
				switch (f.fmt) {
				case F_S16LE: acc += (int16_t)(p[0] | p[1] << 8); break;
				case F_S16BE: acc += (int16_t)(p[1] | p[0] << 8); break;
				case F_U8:    acc += ((int)p[0] - 128) << 8; break;
				case F_ALAW:  acc += alaw_to_linear(p[0]); break;
				case F_ULAW:  acc += ulaw_to_linear(p[0]); break;
				}
			}
			out[i] = acc / f.channels;
		}
		if (ao_send(out, samples))
			break;
		total += samples;
	}
	next = r->preempt_fd;
	LOG("speaker: %s after %.1fs", next >= 0 ? "pre-empted" : "done",
	    (double)total / f.rate);

done:
	if (opened)
		ao_close(next < 0, f.rate);
	if (http) {
		const char *resp = "HTTP/1.0 200 OK\r\nContent-Length: 3\r\n\r\nok\n";
		if (write(fd, resp, strlen(resp)) < 0) {}
	}
	close(fd);
	if (next < 0)
		next = r->preempt_fd;
	free(r);
	return next;
}

static void *speaker_loop(void *arg)
{
	(void)arg;
	while (!g_quit) {
		struct pollfd p = { listen_fd, POLLIN, 0 };
		int fd;
		if (poll(&p, 1, 500) <= 0)
			continue;
		fd = accept(listen_fd, NULL, NULL);
		while (fd >= 0 && !g_quit)
			fd = play_client(fd);
	}
	return NULL;
}

int speaker_start(void)
{
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(cfg.speaker_port),
				 .sin_addr.s_addr = htonl(INADDR_ANY) };
	int one = 1;

	listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (bind(listen_fd, (struct sockaddr *)&a, sizeof(a)) || listen(listen_fd, 2)) {
		ERR("speaker: cannot listen on port %d: %s", cfg.speaker_port, strerror(errno));
		close(listen_fd);
		listen_fd = -1;
		return -1;
	}
	running = !pthread_create(&thread, NULL, speaker_loop, NULL);
	LOG("speaker: listening on tcp port %d", cfg.speaker_port);
	return running ? 0 : -1;
}

void speaker_stop(void)
{
	if (running)
		pthread_join(thread, NULL);
	running = 0;
	if (listen_fd >= 0)
		close(listen_fd);
	listen_fd = -1;
}
