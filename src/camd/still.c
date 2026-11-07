/* JPEG stills over HTTP:  GET http://<camera>:<http_port>/still
 *
 * On each request a frame is taken from VI channel 0 (which is also bound to
 * the H.264 encoder; the channel is created with depth 1 so a frame can be
 * held briefly), sent to a dedicated hardware JPEG encoder channel, and the
 * result is returned as image/jpeg. Nothing runs between requests.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "camd.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_venc.h"
#include "rk_mpi_vi.h"

#define VI_PIPE  0
#define VI_CHN   0
#define JPEG_CHN STILL_VENC_CHN

static int listen_fd = -1, venc_on;
static pthread_t thread;
static int running;

static int jpeg_start(void)
{
	VENC_CHN_ATTR_S a;
	VENC_RECV_PIC_PARAM_S recv;
	VENC_JPEG_PARAM_S jp;
	int ret;

	memset(&a, 0, sizeof(a));
	a.stVencAttr.enType = RK_VIDEO_ID_JPEG;
	a.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
	a.stVencAttr.u32PicWidth = cfg.video_width;
	a.stVencAttr.u32PicHeight = cfg.video_height;
	a.stVencAttr.u32VirWidth = cfg.video_width;
	a.stVencAttr.u32VirHeight = cfg.video_height;
	a.stVencAttr.u32StreamBufCnt = 1;
	/* a high-quality 1080p JPEG of a busy scene stays well under 1 MB */
	a.stVencAttr.u32BufSize = cfg.video_width * cfg.video_height / 2;
	a.stVencAttr.enMirror = MIRROR_NONE;
	a.stVencAttr.stAttrJpege.bSupportDCF = RK_FALSE;
	a.stVencAttr.stAttrJpege.enReceiveMode = VENC_PIC_RECEIVE_SINGLE;
	if ((ret = RK_MPI_VENC_CreateChn(JPEG_CHN, &a))) {
		ERR("still: JPEG channel 0x%x", ret);
		return -1;
	}
	venc_on = 1;
	memset(&jp, 0, sizeof(jp));
	jp.u32Qfactor = cfg.still_quality < 1 ? 1 : cfg.still_quality > 99 ? 99 : cfg.still_quality;
	RK_MPI_VENC_SetJpegParam(JPEG_CHN, &jp);
	memset(&recv, 0, sizeof(recv));
	recv.s32RecvPicNum = -1;
	if ((ret = RK_MPI_VENC_StartRecvFrame(JPEG_CHN, &recv))) {
		ERR("still: StartRecvFrame 0x%x", ret);
		return -1;
	}
	return 0;
}

static void send_str(int fd, const char *s)
{
	if (write(fd, s, strlen(s)) < 0) {}
}

static void reply_error(int fd, const char *status)
{
	char buf[160];
	snprintf(buf, sizeof(buf), "HTTP/1.0 %s\r\nContent-Type: text/plain\r\n"
		 "Connection: close\r\n\r\n%s\n", status, status);
	send_str(fd, buf);
}

static void serve_still(int fd)
{
	VIDEO_FRAME_INFO_S frame;
	VENC_STREAM_S s;
	VENC_PACK_S pack;
	char hdr[200];
	int ret;
	uint64_t t0 = now_us();

	memset(&frame, 0, sizeof(frame));
	if ((ret = RK_MPI_VI_GetChnFrame(VI_PIPE, VI_CHN, &frame, 1000))) {
		ERR("still: GetChnFrame 0x%x", ret);
		reply_error(fd, "503 Service Unavailable");
		return;
	}
	/* the encoder reads the frame asynchronously: release it only once the
	 * JPEG has come out */
	if ((ret = RK_MPI_VENC_SendFrame(JPEG_CHN, &frame, 1000))) {
		RK_MPI_VI_ReleaseChnFrame(VI_PIPE, VI_CHN, &frame);
		ERR("still: SendFrame 0x%x", ret);
		reply_error(fd, "503 Service Unavailable");
		return;
	}
	memset(&s, 0, sizeof(s));
	s.pstPack = &pack;
	ret = RK_MPI_VENC_GetStream(JPEG_CHN, &s, 2000);
	RK_MPI_VI_ReleaseChnFrame(VI_PIPE, VI_CHN, &frame);
	if (ret) {
		ERR("still: GetStream 0x%x", ret);
		reply_error(fd, "503 Service Unavailable");
		return;
	}
	void *data = RK_MPI_MB_Handle2VirAddr(s.pstPack->pMbBlk);
	snprintf(hdr, sizeof(hdr), "HTTP/1.0 200 OK\r\nContent-Type: image/jpeg\r\n"
		 "Content-Length: %u\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n",
		 s.pstPack->u32Len);
	send_str(fd, hdr);
	if (data && write(fd, data, s.pstPack->u32Len) < 0) {}
	LOG("still: %u bytes in %llu ms", s.pstPack->u32Len,
	    (unsigned long long)(now_us() - t0) / 1000);
	RK_MPI_VENC_ReleaseStream(JPEG_CHN, &s);
}

static void handle(int fd)
{
	char req[512], method[8] = "", path[128] = "";
	int n = 0;
	struct pollfd p = { fd, POLLIN, 0 };

	/* read the request line (and whatever headers came with it) */
	while (n < (int)sizeof(req) - 1 && poll(&p, 1, 2000) > 0) {
		int k = read(fd, req + n, sizeof(req) - 1 - n);
		if (k <= 0)
			break;
		n += k;
		req[n] = 0;
		if (strstr(req, "\r\n\r\n") || strstr(req, "\n\n"))
			break;
	}
	req[n] = 0;
	sscanf(req, "%7s %127s", method, path);
	char *q = strchr(path, '?');
	if (q)
		*q = 0;
	if (strcmp(method, "GET") && strcmp(method, "HEAD"))
		reply_error(fd, "405 Method Not Allowed");
	else if (!strcmp(path, "/still") || !strcmp(path, "/still.jpg"))
		serve_still(fd);
	else
		reply_error(fd, "404 Not Found");
}

static void *loop(void *arg)
{
	(void)arg;
	while (!g_quit) {
		struct pollfd p = { listen_fd, POLLIN, 0 };
		int fd;
		if (poll(&p, 1, 500) <= 0)
			continue;
		if ((fd = accept(listen_fd, NULL, NULL)) < 0)
			continue;
		handle(fd);
		close(fd);
	}
	return NULL;
}

int still_start(void)
{
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(cfg.http_port),
				 .sin_addr.s_addr = htonl(INADDR_ANY) };
	int one = 1;

	if (jpeg_start())
		return -1;
	listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (bind(listen_fd, (struct sockaddr *)&a, sizeof(a)) || listen(listen_fd, 4)) {
		ERR("still: cannot listen on port %d: %s", cfg.http_port, strerror(errno));
		close(listen_fd);
		listen_fd = -1;
		return -1;
	}
	running = !pthread_create(&thread, NULL, loop, NULL);
	LOG("still: http://<ip>:%d/still (JPEG quality %d)", cfg.http_port, cfg.still_quality);
	return running ? 0 : -1;
}

void still_stop(void)
{
	if (running)
		pthread_join(thread, NULL);
	running = 0;
	if (listen_fd >= 0)
		close(listen_fd);
	listen_fd = -1;
	if (venc_on) {
		RK_MPI_VENC_StopRecvFrame(JPEG_CHN);
		RK_MPI_VENC_DestroyChn(JPEG_CHN);
	}
	venc_on = 0;
}
