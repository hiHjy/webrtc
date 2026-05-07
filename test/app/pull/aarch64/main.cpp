#include "config_reader.hpp"
#include "drm_test.h"
#include "rkmpp_dec.h"
#include "srstowebrtc.hpp"
#include <atomic>
#include <audio_playback_manager.h>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <stdio.h>
#include <thread>

typedef struct {
	RkMppDecoder decoder;
	DRM_Ctx drm_ctx;
	std::condition_variable cv;
	std::mutex mtx;
	int request_display;
	int stop_display;
	DRM_Buf buf;
	audio_playback_manager_t audio_ctx;

	uint64_t audio_pts_ms;
	uint64_t video_pts_ms;
	uint64_t audio_first_ts;
	uint64_t video_first_ts;
	uint64_t audio_count;
	uint64_t video_count;
	uint64_t video_frame_droped;
	uint64_t video_current_decoded_frame_pts_us;
} App_Ctx;
// void videoCb(rtc::binary frame, rtc::FrameInfo info)
// {
//     std::cout << "获取到一帧视频数据:size:" << frame.size()
//               << " timestamp:" << info.timestamp
//               << std::endl;

// }

void audioCb(rtc::binary frame, rtc::FrameInfo info) {
	std::cout << "获取到一帧音频数据:size:" << frame.size() << " timestamp:" << info.timestamp
	          << std::endl;
}

static uint32_t mpp_fmt_to_drm_fmt(RK_U32 fmt) {
	switch (fmt & MPP_FRAME_FMT_MASK) {
	case MPP_FMT_YUV420SP:
		return DRM_FORMAT_NV12;

		// case MPP_FMT_YUV420SP_10BIT:
		//     return DRM_FORMAT_NV12_10; // 注意：不是所有 DRM plane 都支持

	case MPP_FMT_YUV422SP:
		return DRM_FORMAT_NV16;

	case MPP_FMT_YUV444SP:
		return DRM_FORMAT_NV24;

	case MPP_FMT_YUV420P:
		return DRM_FORMAT_YUV420;

	case MPP_FMT_YUV422P:
		return DRM_FORMAT_YUV422;

	case MPP_FMT_YUV444P:
		return DRM_FORMAT_YUV444;

	case MPP_FMT_RGB888:
		return DRM_FORMAT_RGB888;

	case MPP_FMT_BGR888:
		return DRM_FORMAT_BGR888;

	case MPP_FMT_ARGB8888:
		return DRM_FORMAT_ARGB8888;

	case MPP_FMT_ABGR8888:
		return DRM_FORMAT_ABGR8888;

	default:
		return 0;
	}
}
void mppDecodedFrameCb(const uint8_t *data, size_t size, int fd, RK_U32 width, RK_U32 height,
                       RK_U32 h_stride, RK_U32 v_stride, RK_U32 fmt, RK_S64 pts_us,
                       void *userdata) {
	(void)data;
	

	App_Ctx *ctx = static_cast<App_Ctx *>(userdata);


	std::cout << "MPP解码输出一帧: fd=" << fd << " " << width << "x" << height
	          << " stride=" << h_stride << "x" << v_stride << " fmt=" << fmt << " pts_us=" << pts_us
	          << " pts_ms=" << (pts_us / 1000) << " buf_size=" << size << " droped:"<< ctx->video_frame_droped <<std::endl;
	ctx->video_current_decoded_frame_pts_us = pts_us;
	{

		std::lock_guard<std::mutex> lock(ctx->mtx);
		DRM_Buf *buf = &ctx->buf;
		memset(buf, 0, sizeof(*buf));
		// 在这里构造DRM_Buf, 然后通知显示线程
		buf->dma_fd = fd;
		buf->fmt = mpp_fmt_to_drm_fmt(fmt);
		buf->w = width;
		buf->h = height;
		buf->size = size;
		buf->pitches[0] = h_stride;
		buf->pitches[1] = h_stride;
		buf->offsets[0] = 0;
		buf->offsets[1] = h_stride * v_stride;
		ctx->request_display = 1;
	}
	ctx->cv.notify_one();
}

void drmDiplayThread(App_Ctx *ctx) {
	while (1) {
		DRM_Buf buf;
		int64_t video_pts_us = 0;

		{
			std::unique_lock<std::mutex> lock(ctx->mtx);
			ctx->cv.wait(lock, [&ctx] { return ctx->request_display || ctx->stop_display; });
			if (ctx->stop_display) {
				break;
			}

			buf = ctx->buf;
			video_pts_us = ctx->video_current_decoded_frame_pts_us;
			ctx->request_display = 0;
		}

		int64_t audio_clock_us = 0;
		if (audio_playback_manager_get_clock_us(&ctx->audio_ctx, &audio_clock_us) == 0) {
			const int64_t diff_ms = (video_pts_us - audio_clock_us) / 1000;
			std::cout << "av_sync video_ms=" << (video_pts_us / 1000)
			          << " audio_clock_ms=" << (audio_clock_us / 1000)
			          << " diff_ms=" << diff_ms << std::endl;

			if (diff_ms < -120) {
				ctx->video_frame_droped++;
				continue;
			}

			if (diff_ms > 30) {
				std::this_thread::sleep_for(std::chrono::milliseconds(diff_ms - 10));
			}
		} else {
			std::cout << "av_sync audio clock not ready" << std::endl;
		}

		drmDisplaySubmit(&ctx->drm_ctx, &buf);
		drmHandleEvents(&ctx->drm_ctx, 10);
	}
}

int main(int argc, char const *argv[]) {
	ConfigReader cfgReader("./main.conf");
	if (!cfgReader.loaded()) {
		std::cout << "配置文件读取失败" << std::endl;
	} else {
		std::cout << "配置文件读取成功" << std::endl;
	}
	std::string publish_api;
	std::string play_api;
	std::string stream_url;
	publish_api = cfgReader.read("publish_api");
	play_api = cfgReader.read("play_api");
	stream_url = cfgReader.read("stream_url");

	std::cout << "publish_api: " << publish_api << "\n"
	          << "play_api: " << play_api << "\n"
	          << "stream_url: " << stream_url << std::endl;

	App_Ctx ctx{};

	rk_mpp_decoder_init(&ctx.decoder, MPP_VIDEO_CodingAVC, nullptr);
	rk_mpp_decoder_set_frame_callback(&ctx.decoder, mppDecodedFrameCb, &ctx);

	SrsToWebRTC rtc;
	rtc.setVideoFrameCallback([&ctx](rtc::binary frame, rtc::FrameInfo info) {
		// std::cout << "[Libdatachannel Callback]获取视频数据 --大小：" << frame.size()
		//  << "  时间戳:" << info.timestamp << std::endl;

		if (!ctx.video_first_ts) {
			ctx.video_first_ts = info.timestamp;
		}
		ctx.video_pts_ms = static_cast<uint64_t>(
		    (static_cast<uint64_t>(info.timestamp) - ctx.video_first_ts) * 1000LL / 90000);
		std::cout << "video_pts:" << ctx.video_pts_ms << "  seq:" << ++ctx.video_count << std::endl;
		const RK_S64 video_pts_us = static_cast<RK_S64>(
		    (static_cast<uint64_t>(info.timestamp) - ctx.video_first_ts) * 1000000ULL / 90000ULL);
		rk_mpp_decoder_send_data_with_pts(&ctx.decoder, reinterpret_cast<uint8_t *>(frame.data()),
		                                  static_cast<size_t>(frame.size()), 0, video_pts_us);
	});

	// audio_playback_ctx ap_ctx;
	// audio_playback_init(&ap_ctx, "plughw:1,0", 48000, 1, SND_PCM_FORMAT_S16, 960);

	audio_playback_manager_config_t audio_cfg;
	audio_playback_manager_config_init(&audio_cfg);
	audio_cfg.channels = 1;
	strcpy(audio_cfg.device, "plughw:0,0");
	audio_cfg.period_size = 960;
	audio_cfg.format = SND_PCM_FORMAT_S16_LE;
	audio_cfg.sample_rate = 48000;

	if (audio_playback_manager_init(&ctx.audio_ctx, &audio_cfg) != 0) {
		std::cerr << "audio_playback_manager_init failed" << std::endl;
		return 1;
	}

	if (audio_playback_manager_start(&ctx.audio_ctx) != 0) {
		std::cerr << "audio_playback_manager_start failed" << std::endl;
		audio_playback_manager_close(&ctx.audio_ctx);
		return 1;
	}

	rtc.setAudioFrameCallback([&](rtc::binary frame, rtc::FrameInfo info) {
		if (!ctx.audio_first_ts) {
			ctx.audio_first_ts = info.timestamp;
		}

		ctx.audio_pts_ms = static_cast<uint64_t>(
		    (static_cast<uint64_t>(info.timestamp) - ctx.audio_first_ts) * 1000LL / 48000);
		std::cout << "audio_pts:" << ctx.audio_pts_ms << "  seq:" << ++ctx.audio_count << std::endl;
		const int64_t audio_pts_us = static_cast<int64_t>(
		    (static_cast<uint64_t>(info.timestamp) - ctx.audio_first_ts) * 1000000ULL / 48000ULL);
		int ret = audio_playback_manager_push_packet_with_pts(
		    &ctx.audio_ctx, reinterpret_cast<uint8_t *>(frame.data()),
		    static_cast<size_t>(frame.size()), audio_pts_us);
		if (ret != 0) {
			std::cerr << "audio_playback_manager_push_packet_with_pts failed ret=" << ret
			          << " size=" << frame.size() << " timestamp=" << info.timestamp
			          << " pts_us=" << audio_pts_us << std::endl;
		}
		// int64_t audio_clock_us = 0;
		// if (audio_playback_manager_get_clock_us(&ctx.audio_ctx, &audio_clock_us) == 0) {
		// 	std::cout << "audio_clock_ms:" << (audio_clock_us / 1000)
		// 	          << " audio_queue_delay_ms:"
		// 	          << ((audio_pts_us - audio_clock_us) / 1000)
		// 	          << std::endl;
		// }
		// std::cout << "获取到一帧音频数据:size:" << frame.size()
		//       << " timestamp:" << info.timestamp
		//       << std::endl;
	});

	drmInit(&ctx.drm_ctx);
	drmDisplaySetup(&ctx.drm_ctx, DRM_FORMAT_NV12, 0, 0, 1920, 1080);
	std::thread display_thread(drmDiplayThread, &ctx);
	if (rtc.initRTC(play_api.c_str(), stream_url.c_str())) {
		std::cout << "init RTC" << std::endl;
		rtc.setStopRequested(true);
	}

	while (1) {
		std::this_thread::sleep_for(std::chrono::microseconds(50));
		if (rtc.getStopRequested())
			break;
	}
	std::cout << "退出" << std::endl;
	rtc.Stop();

	{
		std::lock_guard<std::mutex> lock(ctx.mtx);
		ctx.stop_display = 1;
	}
	ctx.cv.notify_one();
	if (display_thread.joinable()) {
		display_thread.join();
	}
	audio_playback_manager_close(&ctx.audio_ctx);
	drmDeinit(&ctx.drm_ctx);
	rk_mpp_decoder_deinit(&ctx.decoder);

	return 0;
}
