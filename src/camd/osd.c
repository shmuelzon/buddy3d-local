/* On-screen display: static text and/or a timestamp, burned into the video by
 * the encoder (Rockit OVERLAY regions attached to VENC channel 0).
 *
 * Each of osd_text / osd_time has its own position (tl, tr, bl, br). Items that
 * share a position are stacked in one region: text first, then time. Glyphs are
 * white with a dark outline so they read on any background. Font: Spleen
 * (BSD-2-Clause), converted at build time by tools/bdf2c.py.
 */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "camd.h"
#include "font_spleen.h"
#include "rk_mpi_rgn.h"

#define MARGIN   16
#define ALIGN    16
#define MAX_LINE 96

enum { ITEM_TEXT, ITEM_TIME };
struct slot {
	char pos[3];
	int items[2], n;
	int handle;
	int x, y, w, h;
	int has_time;
	uint8_t *buf;
	int attached;
};

static struct slot slots[2];
static int nslots;
static int fmt, bpp;                /* pixel format and bytes per pixel */
static int gw, gh, scale, outline;  /* glyph cell, scale factor, outline width */
static const uint8_t *font;         /* [95][rows][bpr] */
static int font_rows, font_bpr;
static pthread_t thread;
static int running;

static int align_up(int v) { return (v + ALIGN - 1) / ALIGN * ALIGN; }
static int align_down(int v) { return v / ALIGN * ALIGN; }

static void item_string(int item, char *out, size_t n)
{
	if (item == ITEM_TEXT) {
		snprintf(out, n, "%s", cfg.osd_text);
	} else {
		time_t t = time(NULL);
		struct tm tm;
		localtime_r(&t, &tm);
		if (!strftime(out, n, cfg.osd_time, &tm))
			out[0] = 0;
	}
}

static int glyph_bit(int c, int row, int col)
{
	if (c < 0x20 || c > 0x7e)
		c = '?';
	const uint8_t *g = font + ((c - 0x20) * font_rows + row) * font_bpr;
	return (g[col / 8] >> (7 - col % 8)) & 1;
}

static void put_px(struct slot *s, int x, int y, int white)
{
	if (x < 0 || y < 0 || x >= s->w || y >= s->h)
		return;
	uint32_t *p = (uint32_t *)s->buf + y * s->w + x;
	*p = white ? 0xffffffff : 0xff000000; /* opaque white / opaque black */
}

static void render(struct slot *s)
{
	int right = s->pos[1] == 'r';
	memset(s->buf, 0, s->w * s->h * bpp); /* fully transparent */
	for (int l = 0; l < s->n; l++) {
		char str[MAX_LINE];
		int len, x0, y0 = outline + l * gh;
		item_string(s->items[l], str, sizeof(str));
		len = strlen(str);
		while (len * gw + 2 * outline > s->w && len > 0)
			len--; /* truncate to the region */
		x0 = right ? s->w - outline - len * gw : outline;
		/* two passes: dark outline, then the white glyph on top */
		for (int pass = 0; pass < 2; pass++) {
			for (int i = 0; i < len; i++) {
				for (int r = 0; r < font_rows; r++) {
					for (int c = 0; c < font_bpr * 8 && c < gw / scale; c++) {
						if (!glyph_bit((unsigned char)str[i], r, c))
							continue;
						int px = x0 + i * gw + c * scale, py = y0 + r * scale;
						if (pass == 0) {
							for (int dy = -outline; dy < scale + outline; dy++)
								for (int dx = -outline; dx < scale + outline; dx++)
									put_px(s, px + dx, py + dy, 0);
						} else {
							for (int dy = 0; dy < scale; dy++)
								for (int dx = 0; dx < scale; dx++)
									put_px(s, px + dx, py + dy, 1);
						}
					}
				}
			}
		}
	}
}

static int push(struct slot *s)
{
	BITMAP_S bm = { (PIXEL_FORMAT_E)fmt, s->w, s->h, s->buf };
	int ret = RK_MPI_RGN_SetBitMap(s->handle, &bm);
	if (ret)
		ERR("osd: SetBitMap(%d) 0x%x", s->handle, ret);
	return ret;
}

static int create(struct slot *s, int pixfmt)
{
	RGN_ATTR_S a;
	RGN_CHN_ATTR_S ca;
	MPP_CHN_S chn = { RK_ID_VENC, 0, 0 };
	int ret;

	memset(&a, 0, sizeof(a));
	a.enType = OVERLAY_RGN;
	a.unAttr.stOverlay.enPixelFmt = (PIXEL_FORMAT_E)pixfmt;
	a.unAttr.stOverlay.stSize.u32Width = s->w;
	a.unAttr.stOverlay.stSize.u32Height = s->h;
	if ((ret = RK_MPI_RGN_Create(s->handle, &a)))
		return ret;

	memset(&ca, 0, sizeof(ca));
	ca.bShow = RK_TRUE;
	ca.enType = OVERLAY_RGN;
	ca.unChnAttr.stOverlayChn.stPoint.s32X = s->x;
	ca.unChnAttr.stOverlayChn.stPoint.s32Y = s->y;
	ca.unChnAttr.stOverlayChn.u32BgAlpha = 0;
	ca.unChnAttr.stOverlayChn.u32FgAlpha = 255;
	ca.unChnAttr.stOverlayChn.u32Layer = s->handle;
	if ((ret = RK_MPI_RGN_AttachToChn(s->handle, &chn, &ca))) {
		RK_MPI_RGN_Destroy(s->handle);
		return ret;
	}
	s->attached = 1;
	return 0;
}

static void *loop(void *arg)
{
	(void)arg;
	while (!g_quit) {
		/* wake just after each second boundary */
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		usleep(1000000 - ts.tv_nsec / 1000 + 5000);
		for (int i = 0; i < nslots; i++) {
			if (slots[i].has_time) {
				render(&slots[i]);
				push(&slots[i]);
			}
		}
	}
	return NULL;
}

static int valid_pos(const char *p)
{
	return (p[0] == 't' || p[0] == 'b') && (p[1] == 'l' || p[1] == 'r') && !p[2];
}

static void add_item(int item, const char *pos)
{
	int i;
	if (!valid_pos(pos)) {
		ERR("osd: bad position '%s' (use tl, tr, bl or br)", pos);
		return;
	}
	for (i = 0; i < nslots; i++)
		if (!strcmp(slots[i].pos, pos))
			break;
	if (i == nslots)
		snprintf(slots[nslots++].pos, sizeof(slots[0].pos), "%s", pos);
	slots[i].items[slots[i].n++] = item;
	if (item == ITEM_TIME)
		slots[i].has_time = 1;
}

int osd_start(void)
{
	nslots = 0;
	memset(slots, 0, sizeof(slots));
	if (cfg.osd_text[0])
		add_item(ITEM_TEXT, cfg.osd_text_position);
	if (cfg.osd_time[0])
		add_item(ITEM_TIME, cfg.osd_time_position);
	if (!nslots)
		return 0;

	if (cfg.osd_font_size <= 16) {
		font = &FONT16[0][0][0]; font_rows = FONT16_H; font_bpr = (FONT16_W + 7) / 8;
		gw = FONT16_W; gh = FONT16_H; scale = 1;
	} else {
		font = &FONT32[0][0][0]; font_rows = FONT32_H; font_bpr = (FONT32_W + 7) / 8;
		scale = cfg.osd_font_size >= 64 ? 2 : 1;
		gw = FONT32_W * scale; gh = FONT32_H * scale;
	}
	outline = gh >= 32 ? 2 : 1;

	for (int i = 0; i < nslots; i++) {
		struct slot *s = &slots[i];
		int chars = 0;
		for (int l = 0; l < s->n; l++) {
			char str[MAX_LINE];
			item_string(s->items[l], str, sizeof(str));
			/* timestamps can vary in length (e.g. %A); leave some room */
			int c = strlen(str) + (s->items[l] == ITEM_TIME ? 3 : 0);
			if (c > chars)
				chars = c;
		}
		s->handle = i;
		s->w = align_up(chars * gw + 2 * outline);
		if (s->w > cfg.video_width - 2 * MARGIN)
			s->w = align_down(cfg.video_width - 2 * MARGIN);
		s->h = align_up(s->n * gh + 2 * outline);
		s->x = s->pos[1] == 'l' ? MARGIN : align_down(cfg.video_width - MARGIN - s->w);
		s->y = s->pos[0] == 't' ? MARGIN : align_down(cfg.video_height - MARGIN - s->h);

		/* ARGB8888: explicit per-pixel alpha. (The 16-bit mode is really
		 * BGRA5551 with a global fg/bg alpha, which made the outline vanish.) */
		fmt = RK_FMT_ARGB8888;
		bpp = 4;
		int ret = create(s, fmt);
		if (ret) {
			ERR("osd: region %s %dx%d at %d,%d failed 0x%x", s->pos, s->w, s->h, s->x, s->y, ret);
			nslots = i;
			break;
		}
		s->buf = calloc(1, s->w * s->h * bpp);
		render(s);
		push(s);
		LOG("osd: %s region %dx%d at %d,%d (%d line%s)", s->pos, s->w, s->h, s->x, s->y,
		    s->n, s->n > 1 ? "s" : "");
	}
	if (nslots)
		for (int i = 0; i < nslots; i++)
			if (slots[i].has_time) {
				running = !pthread_create(&thread, NULL, loop, NULL);
				break;
			}
	return 0;
}

void osd_stop(void)
{
	MPP_CHN_S chn = { RK_ID_VENC, 0, 0 };
	if (running)
		pthread_join(thread, NULL);
	running = 0;
	for (int i = 0; i < nslots; i++) {
		if (slots[i].attached) {
			RK_MPI_RGN_DetachFromChn(slots[i].handle, &chn);
			RK_MPI_RGN_Destroy(slots[i].handle);
		}
		free(slots[i].buf);
	}
	nslots = 0;
}
