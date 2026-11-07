/* sensor -> ISP (rkaiq) -> VI chn0 -> VENC chn0 -> RTSP */
#include <dirent.h>
#include <fcntl.h>
#include <linux/v4l2-subdev.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <rk_aiq_user_api2_imgproc.h>
#include <rk_aiq_user_api2_sysctl.h>

#include "camd.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_venc.h"
#include "rk_mpi_vi.h"

#define VI_DEV  0
#define VI_PIPE 0
#define VI_CHN  0
#define VENC_CHN 0

static rk_aiq_sys_ctx_t *aiq;
static int sensor_fps = 30;  /* frame rate the sensor mode delivers */
static int enc_fps = 30;     /* frame rate we encode (<= sensor_fps) */
static pthread_t stream_thread;
static int stream_running, vi_dev_on, vi_chn_on, venc_on, bound;
static MPP_CHN_S src_chn = { RK_ID_VI, VI_DEV, VI_CHN };
static MPP_CHN_S dst_chn = { RK_ID_VENC, 0, VENC_CHN };

static XCamReturn isp_err_cb(rk_aiq_err_msg_t *msg)
{
	ERR("isp: error %d", msg->err_code);
	return XCAM_RETURN_NO_ERROR;
}

/* Ask the sensor's V4L2 sub-device for its frame interval (the current mode's
 * maximum rate; auto exposure may stretch frames below it in low light). The
 * sub-device name equals rkaiq's sensor name, e.g. "m00_b_jx_f37p 4-0040". */
static int read_sensor_fps(const char *name)
{
	DIR *d = opendir("/sys/class/video4linux");
	struct dirent *e;
	int fps = 0;

	if (!d)
		return 0;
	while (!fps && (e = readdir(d))) {
		char path[300], buf[64] = { 0 };
		int fd, n;
		if (strncmp(e->d_name, "v4l-subdev", 10))
			continue;
		snprintf(path, sizeof(path), "/sys/class/video4linux/%s/name", e->d_name);
		if ((fd = open(path, O_RDONLY)) < 0)
			continue;
		n = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (n <= 0)
			continue;
		buf[strcspn(buf, "\n")] = 0;
		if (strcmp(buf, name))
			continue;
		snprintf(path, sizeof(path), "/dev/%s", e->d_name);
		if ((fd = open(path, O_RDWR)) < 0)
			continue;
		struct v4l2_subdev_frame_interval fi;
		memset(&fi, 0, sizeof(fi));
		if (!ioctl(fd, VIDIOC_SUBDEV_G_FRAME_INTERVAL, &fi) && fi.interval.numerator)
			fps = (fi.interval.denominator + fi.interval.numerator / 2) / fi.interval.numerator;
		close(fd);
	}
	closedir(d);
	return fps;
}

static int isp_start(void)
{
	rk_aiq_static_info_t info;

	setenv("HDR_MODE", "0", 1);
	if (rk_aiq_uapi2_sysctl_enumStaticMetas(0, &info)) {
		ERR("isp: no sensor found");
		return -1;
	}
	LOG("isp: sensor %s, iq dir %s", info.sensor_info.sensor_name, cfg.iq_dir);
	if ((sensor_fps = read_sensor_fps(info.sensor_info.sensor_name)) <= 0) {
		ERR("isp: cannot read sensor frame rate, assuming 30");
		sensor_fps = 30;
	}
	enc_fps = cfg.video_fps > 0 && cfg.video_fps < sensor_fps ? cfg.video_fps : sensor_fps;
	if (cfg.video_fps > sensor_fps)
		ERR("isp: video_fps=%d exceeds the sensor's %d fps", cfg.video_fps, sensor_fps);
	LOG("isp: sensor delivers %d fps, encoding %d fps", sensor_fps, enc_fps);
	aiq = rk_aiq_uapi2_sysctl_init(info.sensor_info.sensor_name, cfg.iq_dir,
				       isp_err_cb, NULL);
	if (!aiq) {
		ERR("isp: init failed");
		return -1;
	}
	if (rk_aiq_uapi2_sysctl_prepare(aiq, 0, 0, RK_AIQ_WORKING_MODE_NORMAL)) {
		ERR("isp: prepare failed");
		return -1;
	}
	if (rk_aiq_uapi2_sysctl_start(aiq)) {
		ERR("isp: start failed");
		return -1;
	}
	if (cfg.video_flip)
		rk_aiq_uapi2_setMirrorFlip(aiq, cfg.video_flip & 1, cfg.video_flip & 2, 3);
	return 0;
}

/* The sensor's IQ file has main scene "normal" with sub-scenes "day" and
 * "gray" (the latter tuned for IR / black-and-white). */
void video_set_gray(int gray)
{
	int ret;
	if (!aiq)
		return;
	ret = rk_aiq_uapi2_sysctl_switch_scene(aiq, "normal", gray ? "gray" : "day");
	if (ret)
		ERR("isp: switch to scene %s failed (%d)", gray ? "gray" : "day", ret);
}

static void isp_stop(void)
{
	if (!aiq)
		return;
	rk_aiq_uapi2_sysctl_stop(aiq, false);
	rk_aiq_uapi2_sysctl_deinit(aiq);
	aiq = NULL;
}

static int vi_start(void)
{
	VI_DEV_ATTR_S dev_attr;
	VI_DEV_BIND_PIPE_S bind;
	VI_CHN_ATTR_S chn;
	int ret;

	memset(&dev_attr, 0, sizeof(dev_attr));
	memset(&bind, 0, sizeof(bind));
	if (RK_MPI_VI_GetDevAttr(VI_DEV, &dev_attr) == RK_ERR_VI_NOT_CONFIG &&
	    (ret = RK_MPI_VI_SetDevAttr(VI_DEV, &dev_attr))) {
		ERR("vi: SetDevAttr 0x%x", ret);
		return -1;
	}
	if (RK_MPI_VI_GetDevIsEnable(VI_DEV) != RK_SUCCESS) {
		if ((ret = RK_MPI_VI_EnableDev(VI_DEV))) {
			ERR("vi: EnableDev 0x%x", ret);
			return -1;
		}
		bind.u32Num = 1;
		bind.PipeId[0] = VI_PIPE;
		if ((ret = RK_MPI_VI_SetDevBindPipe(VI_DEV, &bind))) {
			ERR("vi: SetDevBindPipe 0x%x", ret);
			return -1;
		}
	}
	vi_dev_on = 1;

	memset(&chn, 0, sizeof(chn));
	chn.stIspOpt.u32BufCount = 2;
	chn.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
	chn.stSize.u32Width = cfg.video_width;
	chn.stSize.u32Height = cfg.video_height;
	chn.enPixelFormat = RK_FMT_YUV420SP;
	chn.enCompressMode = COMPRESS_MODE_NONE;
	chn.u32Depth = 0;
	if ((ret = RK_MPI_VI_SetChnAttr(VI_PIPE, VI_CHN, &chn)) ||
	    (ret = RK_MPI_VI_EnableChn(VI_PIPE, VI_CHN))) {
		ERR("vi: channel setup 0x%x", ret);
		return -1;
	}
	vi_chn_on = 1;
	return 0;
}

static int venc_start(void)
{
	VENC_CHN_ATTR_S a;
	VENC_RECV_PIC_PARAM_S recv;
	int h265 = !strcmp(cfg.video_codec, "h265");
	int vbr = !strcmp(cfg.video_rc, "vbr");
	int gop = cfg.video_gop > 0 ? cfg.video_gop : enc_fps * 2;
	int ret;

	memset(&a, 0, sizeof(a));
	if (!h265 && !vbr) {
		a.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
		a.stRcAttr.stH264Cbr = (VENC_H264_CBR_S){ .u32Gop = gop,
			.u32SrcFrameRateNum = sensor_fps, .u32SrcFrameRateDen = 1,
			.fr32DstFrameRateNum = enc_fps, .fr32DstFrameRateDen = 1,
			.u32BitRate = cfg.video_bitrate };
	} else if (!h265) {
		a.stRcAttr.enRcMode = VENC_RC_MODE_H264VBR;
		a.stRcAttr.stH264Vbr = (VENC_H264_VBR_S){ .u32Gop = gop,
			.u32SrcFrameRateNum = sensor_fps, .u32SrcFrameRateDen = 1,
			.fr32DstFrameRateNum = enc_fps, .fr32DstFrameRateDen = 1,
			.u32BitRate = cfg.video_bitrate, .u32MaxBitRate = cfg.video_bitrate * 3 / 2,
			.u32MinBitRate = cfg.video_bitrate / 4 };
	} else if (!vbr) {
		a.stRcAttr.enRcMode = VENC_RC_MODE_H265CBR;
		a.stRcAttr.stH265Cbr = (VENC_H265_CBR_S){ .u32Gop = gop,
			.u32SrcFrameRateNum = sensor_fps, .u32SrcFrameRateDen = 1,
			.fr32DstFrameRateNum = enc_fps, .fr32DstFrameRateDen = 1,
			.u32BitRate = cfg.video_bitrate };
	} else {
		a.stRcAttr.enRcMode = VENC_RC_MODE_H265VBR;
		a.stRcAttr.stH265Vbr = (VENC_H265_VBR_S){ .u32Gop = gop,
			.u32SrcFrameRateNum = sensor_fps, .u32SrcFrameRateDen = 1,
			.fr32DstFrameRateNum = enc_fps, .fr32DstFrameRateDen = 1,
			.u32BitRate = cfg.video_bitrate, .u32MaxBitRate = cfg.video_bitrate * 3 / 2,
			.u32MinBitRate = cfg.video_bitrate / 4 };
	}

	a.stVencAttr.enType = h265 ? RK_VIDEO_ID_HEVC : RK_VIDEO_ID_AVC;
	a.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
	a.stVencAttr.u32Profile = h265 ? 0 : H264E_PROFILE_HIGH;
	a.stVencAttr.u32PicWidth = cfg.video_width;
	a.stVencAttr.u32PicHeight = cfg.video_height;
	a.stVencAttr.u32VirWidth = cfg.video_width;
	a.stVencAttr.u32VirHeight = cfg.video_height;
	a.stVencAttr.u32StreamBufCnt = 2;
	a.stVencAttr.u32BufSize = cfg.video_width * cfg.video_height * 3 / 2;
	a.stVencAttr.enMirror = MIRROR_NONE;

	if ((ret = RK_MPI_VENC_CreateChn(VENC_CHN, &a))) {
		ERR("venc: CreateChn 0x%x", ret);
		return -1;
	}
	venc_on = 1;
	memset(&recv, 0, sizeof(recv));
	recv.s32RecvPicNum = -1;
	if ((ret = RK_MPI_VENC_StartRecvFrame(VENC_CHN, &recv))) {
		ERR("venc: StartRecvFrame 0x%x", ret);
		return -1;
	}
	LOG("venc: %s %dx%d@%d %s %d kbps gop %d", h265 ? "h265" : "h264", cfg.video_width,
	    cfg.video_height, enc_fps, vbr ? "vbr" : "cbr", cfg.video_bitrate, gop);
	return 0;
}

static void *stream_loop(void *arg)
{
	VENC_STREAM_S s;
	VENC_PACK_S pack;
	uint64_t frames = 0, bytes = 0, t0 = now_us();

	(void)arg;
	memset(&s, 0, sizeof(s));
	s.pstPack = &pack;
	while (!g_quit) {
		int ret = RK_MPI_VENC_GetStream(VENC_CHN, &s, 500);
		if (ret != RK_SUCCESS)
			continue;
		void *data = RK_MPI_MB_Handle2VirAddr(s.pstPack->pMbBlk);
		if (data)
			rtsp_send_video(data, s.pstPack->u32Len, s.pstPack->u64PTS);
		frames++;
		bytes += s.pstPack->u32Len;
		RK_MPI_VENC_ReleaseStream(VENC_CHN, &s);

		uint64_t dt = now_us() - t0;
		if (dt >= 60 * 1000000ULL) {
			LOG("video: %.1f fps, %llu kbps", frames * 1e6 / dt,
			    (unsigned long long)(bytes * 8 * 1000 / dt));
			frames = bytes = 0;
			t0 = now_us();
		}
	}
	return NULL;
}

int video_start(void)
{
	int ret;

	if (isp_start() || vi_start() || venc_start())
		return -1;
	if ((ret = RK_MPI_SYS_Bind(&src_chn, &dst_chn))) {
		ERR("video: bind vi->venc 0x%x", ret);
		return -1;
	}
	bound = 1;
	stream_running = !pthread_create(&stream_thread, NULL, stream_loop, NULL);
	return stream_running ? 0 : -1;
}

void video_stop(void)
{
	if (stream_running)
		pthread_join(stream_thread, NULL);
	stream_running = 0;
	if (bound)
		RK_MPI_SYS_UnBind(&src_chn, &dst_chn);
	if (venc_on) {
		RK_MPI_VENC_StopRecvFrame(VENC_CHN);
		RK_MPI_VENC_DestroyChn(VENC_CHN);
	}
	if (vi_chn_on)
		RK_MPI_VI_DisableChn(VI_PIPE, VI_CHN);
	if (vi_dev_on)
		RK_MPI_VI_DisableDev(VI_DEV);
	bound = venc_on = vi_chn_on = vi_dev_on = 0;
	isp_stop();
}
