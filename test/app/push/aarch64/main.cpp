#include "cam.h"
#include <audio_stream_manager.h>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <rkmpp_enc.h>
#include <srs_api.hpp>
#include <stdio.h>
#include <string>
#include <thread>
#include <webrtctosrs.hpp>

const std::string publish_api = "http://192.168.1.27:1985/rtc/v1/publish/";
const std::string stream_url = "webrtc://192.168.1.27/live/test";

/*
 * 整个推流 demo 的全局上下文。
 *
 * 现在所有回调的 userdata 都统一传 App_Ctx*，这样每个回调里都能拿到同一组状态：
 *
 * - encoder:
 *   Rockchip MPP H.264 编码器。摄像头回调拿到 dma-buf fd 后，会把 fd 送进这里编码。
 *
 * - rtc:
 *   WebRTC -> SRS 推流对象。编码器吐出 H.264 后，通过它发视频；
 *   音频模块吐出 Opus 后，也通过它发音频。
 *
 * - video_frame_index / video_fps:
 *   用来给视频帧生成递增时间戳。WebRTC/RTP 发送时需要一个媒体时间线，
 *   这里按 30fps 计算：第 N 帧 timestamp = N * 1000000 / fps，单位 us。
 *
 * - audio_manager:
 *   ALSA 采集 + Opus 编码的总控模块。
 *
 * - audio_frames_index:
 *   当前其实更像“Opus 包序号”。因为配置是 48kHz、960 samples，
 *   每个 Opus 包是 20ms，所以 timestamp = packet_index * 20000us。
 */
typedef struct {
	RkMppEncoder encoder;
	WebRTCToSrs rtc;
	uint64_t video_frame_index;
	uint32_t video_fps;
	audio_stream_manager audio_manager;
	uint64_t audio_frames_index;
} App_Ctx;

/*
 * 摄像头取到一帧后的回调。
 *
 * cam 模块现在内部使用 V4L2 DMABUF 模式：
 * - fd 是当前这帧图像所在的 dma-buf fd
 * - index 是 V4L2 buffer 槽位
 * - w/h/stride/size 是驱动返回的图像参数
 *
 * 这里不拷贝图像数据，直接把 dma-buf fd 交给 MPP 编码器。
 * 这条路径是零拷贝/少拷贝链路的关键：camera -> dma-buf -> MPP encoder。
 */
void cam_cb(int fd, int index, int w, int h, int stride, unsigned int size, void *userdata) {
	// printf("cam_cb: fd=%d, index=%d, w=%d, h=%d, stride=%d, size=%u\n",
	//     fd, index, w, h, stride, size);
	App_Ctx *ctx = static_cast<App_Ctx *>(userdata);
	if (!ctx) {
		return;
	}

	rk_mpp_encoder_send_frame(&ctx->encoder, fd, 0);
}

/*
 * MPP 编码器吐出 H.264 码流后的回调。
 *
 * data/size:
 *   Annex-B 格式的 H.264 数据，通常带 00 00 00 01 起始码。
 *
 * is_header:
 *   1 表示 SPS/PPS 之类的编码头；0 表示普通视频帧。
 *   这里 header 也会发给 WebRTC，但不计入 video_frame_index，
 *   因为它不是一帧真正的视频画面。
 *
 * eos:
 *   编码器结束标志，目前正常实时推流里一般是 0。
 *
 * timestamp_us:
 *   给 WebRTCToSrs 的媒体时间戳，单位是微秒。
 *   当前用固定 fps 推算，适合摄像头稳定 30fps 的 demo。
 *   如果后面要做严格音视频同步，可以改成采集时间/系统单调时钟。
 */
void enc_cb(const uint8_t *data, size_t size, int is_header, int eos, void *userdata) {
	printf("enc_cb: data=%p, size=%zu, is_header=%d, eos=%d, userdata=%p\n", data, size, is_header,
	       eos, userdata);

	App_Ctx *ctx = static_cast<App_Ctx *>(userdata);
	if (!ctx) {
		return;
	}

	const uint32_t fps = ctx->video_fps ? ctx->video_fps : 30;
	const uint64_t timestamp_us = ctx->video_frame_index * 1000000ULL / fps;

	ctx->rtc.SendEncodedAnnexBFrame(data, size, timestamp_us);

	if (!is_header) {
		ctx->video_frame_index++;
	}
};

/*
 * Opus 编码器吐出一包音频后的回调。
 *
 * audio_input 模块内部链路是：
 *
 *   ALSA capture -> PCM(S16_LE) -> Opus encoder -> opus_packet_cb
 *
 * 当前配置：
 * - sample_rate = 48000
 * - channels = 1
 * - period_size = 960
 * - opus_frame_samples = 960
 *
 * 也就是说每包 Opus 对应 960 / 48000 = 20ms 音频。
 * 所以这里用 audio_frames_index * 20ms 作为 timestamp_us。
 */
static int opus_packet_cb(const uint8_t *packet, size_t packet_len, void *user_data) {
	printf("opus_packet_cb: packet=%p, packet_len=%zu, user_data=%p\n", packet, packet_len,
	       user_data);
	App_Ctx *ctx = static_cast<App_Ctx *>(user_data);
	if (!ctx) {
		return -1;
	}
	const uint64_t timestamp_us = ctx->audio_frames_index * 20 * 1000ULL ;
	ctx->rtc.SendEncodedOpus(packet, packet_len, timestamp_us);
	ctx->audio_frames_index++;
	return 0;
}

int main(int argc, char const *argv[]) {
	/*
	 * ctx 用零初始化，保证计数器从 0 开始，C 模块里的结构体成员也不会带脏值。
	 */
	App_Ctx ctx{};
	ctx.video_fps = 30;

	/*
	 * 1. 初始化摄像头。
	 *
	 * camera_init 内部会完成：
	 * - 打开 /dev/video*
	 * - 设置格式/分辨率/帧率
	 * - 申请 dma-buf
	 * - VIDIOC_QBUF
	 * - VIDIOC_STREAMON
	 *
	 * 必须先 camera_init，再读取 cam_get_width/height/bytesperline，
	 * 因为这些值是驱动真正接受后的实际参数。
	 */
	camera_init();

	/*
	 * 2. 初始化 MPP H.264 编码器。
	 *
	 * cam 当前输出格式按 MPP_FMT_YUV422_YUYV 送给编码器。
	 * h_stride 用 cam_get_bytesperline()，v_stride 这里用图像高度。
	 */
	if (rk_mpp_encoder_init(&ctx.encoder, MPP_VIDEO_CodingAVC, cam_get_width(), cam_get_height(),
	                        cam_get_bytesperline(), cam_get_height(), MPP_FMT_YUV422_YUYV,
	                        ctx.video_fps, 0, ctx.video_fps, NULL)) {
		std::cout << "rk_mpp_encoder_init error" << std::endl;
	}

	/*
	 * 3. 注册摄像头回调。
	 *
	 * 后续 camera_run 线程每取到一帧，就会调用 cam_cb，
	 * cam_cb 再把 dma-buf fd 送进 ctx.encoder。
	 */
	cam_register_frame_callback(cam_cb, &ctx);

	/*
	 * 4. 先清理 SRS 上同名旧推流。
	 *
	 * 如果上一次程序异常退出，SRS 里可能还保留 active publish client。
	 * 直接再次 publish 同一个 stream_url 时，SRS 可能返回 {"code":400}。
	 * PrepareSrsPublishSession 会查询 /api/v1/streams/，找到旧 publish cid 后删除。
	 */
	if (!PrepareSrsPublishSession(publish_api, stream_url)) {
		std::cerr << "prepare SRS publish session error: " << HttpLastError() << std::endl;
		camera_close();
		rk_mpp_encoder_deinit(&ctx.encoder);
		return -1;
	}

	/*
	 * 5. 初始化 WebRTC 推流。
	 *
	 * initRTC 会：
	 * - 创建 PeerConnection
	 * - 添加 video/audio sendonly track
	 * - 生成 offer SDP
	 * - 调 SRS publish API 换 answer
	 * - 等待 video/audio track open
	 *
	 * 只有 initRTC 成功后，SendEncodedAnnexBFrame/SendEncodedOpus 才有意义。
	 */
	if (ctx.rtc.initRTC(publish_api.c_str(), stream_url.c_str())) {
		std::cout << "initRTC error" << std::endl;
		camera_close();
		rk_mpp_encoder_deinit(&ctx.encoder);
		return -1;
	}

	/*
	 * 6. 注册编码器输出回调。
	 *
	 * 注意这个回调要在 encoder init 后设置，因为 rk_mpp_encoder_init
	 * 会清零 encoder 结构体。
	 */
	rk_mpp_encoder_set_packet_callback(&ctx.encoder, enc_cb, &ctx);

	/*
	 * 7. 配置并启动音频采集/Opus 编码。
	 *
	 * audio_stream_manager_config_init 要先调用，它会填默认值；
	 * 然后再覆盖板子上的实际采集设备、声道数、采样率等。
	 *
	 * plughw:2,0 是你当前板子确认可用的录音设备。
	 */
	audio_stream_manager_config audio_cfg;

	audio_stream_manager_config_init(&audio_cfg);
	audio_cfg.channels = 1;
	// audio_cfg.device = "plughw:1,0";
	strcpy(audio_cfg.device, "plughw:2,0");
	audio_cfg.format = SND_PCM_FORMAT_S16_LE;
	audio_cfg.period_size = 960;
	audio_cfg.sample_rate = 48000;
	audio_stream_manager_init(&ctx.audio_manager, &audio_cfg);
	audio_stream_manager_set_packet_callback(&ctx.audio_manager, opus_packet_cb, &ctx);
	audio_stream_manager_start(&ctx.audio_manager);
	// audio_capture_init(&ctx.audio_manager, "plughw:1,0", 48000, 1, SND_PCM_FORMAT_S16_LE, 960);

	/*
	 * 8. 摄像头采集是阻塞循环，所以放到单独线程。
	 *
	 * camera_run 会持续 poll/DQBUF/QBUF，并通过 cam_cb 推动视频编码。
	 */
	std::thread cam_thread(camera_run);
	
	/*
	 * 9. 主线程只负责等待退出信号。
	 *
	 * WebRTCToSrs 里注册了 SIGINT/SIGTERM，Ctrl+C 后会设置 stopRequested。
	 */
	while (1) {
		std::this_thread::sleep_for(std::chrono::microseconds(50));
		if (ctx.rtc.getStopRequested())
			break;
	}

	/*
	 * 10. 退出时按“停止发送 -> 停音频 -> 停摄像头 -> 释放编码器”的顺序清理。
	 */
	std::cout << "退出" << std::endl;
	ctx.rtc.Stop();
	audio_stream_manager_close(&ctx.audio_manager);
	camera_close();
	rk_mpp_encoder_deinit(&ctx.encoder);

	if (cam_thread.joinable()) {
		cam_thread.join();
	}

	return 0;
}
