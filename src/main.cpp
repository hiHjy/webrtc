#include "drm_atomic_display.hpp"
#include "rkmpp_dec.h"
#include "srstowebrtc.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <thread>

namespace {

constexpr const char *kDefaultPlayApi = "http://192.168.101.68:1985/rtc/v1/play/";
constexpr const char *kDefaultStreamUrl = "webrtc://192.168.101.68/live/livestream";

void PrintHexPreview(const uint8_t *data, size_t size, size_t max_bytes) {
    if (!data || size == 0) {
        std::cout << " data_preview=<empty>";
        return;
    }

    const size_t preview = std::min(size, max_bytes);
    std::cout << " data_preview=";
    for (size_t i = 0; i < preview; ++i) {
        if (i != 0) {
            std::cout << ' ';
        }
        std::cout << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<unsigned int>(data[i]);
    }
    std::cout << std::dec;
}

struct DisplayContext {
    struct PendingFrame {
        int fd = -1;
        RK_U32 width = 0;
        RK_U32 height = 0;
        RK_U32 h_stride = 0;
        RK_U32 v_stride = 0;
    };

    DrmAtomicDisplay display;
    std::mutex mutex;
    std::condition_variable cv;
    PendingFrame latest_frame;
    bool has_frame = false;
    bool stop = false;
    bool warned_non_nv12 = false;
};

void DisplayThreadMain(DisplayContext *display_ctx) {
    if (!display_ctx) {
        return;
    }

    while (true) {
        DisplayContext::PendingFrame frame;

        {
            std::unique_lock<std::mutex> lock(display_ctx->mutex);
            display_ctx->cv.wait(lock, [display_ctx]() {
                return display_ctx->stop || display_ctx->has_frame;
            });

            if (display_ctx->stop && !display_ctx->has_frame) {
                break;
            }

            frame = std::move(display_ctx->latest_frame);
            display_ctx->latest_frame = {};
            display_ctx->has_frame = false;
        }

        if (!display_ctx->display.PresentDmabuf(frame.fd,
                                                frame.width,
                                                frame.height,
                                                frame.h_stride,
                                                frame.v_stride)) {
            std::cerr << "[display-thread] DRM present failed: "
                      << display_ctx->display.LastError() << std::endl;
            if (SrsToWebRTC::getInstance()) {
                SrsToWebRTC::getInstance()->setStopRequested(true);
            }
            break;
        }
    }

    std::cout << "[display-thread] exit" << std::endl;
}

void OnDecodedFrame(const uint8_t *data,
                    size_t size,
                    int fd,
                    RK_U32 width,
                    RK_U32 height,
                    RK_U32 h_stride,
                    RK_U32 v_stride,
                    RK_U32 fmt,
                    void *userdata) {
    auto *display_ctx = static_cast<DisplayContext *>(userdata);
    std::cout << "[mpp-callback]";

    std::cout << " data=" << static_cast<const void *>(data)
              << " size=" << size
              << " fd=" << fd
              << " w=" << width
              << " h=" << height
              << " hs=" << h_stride
              << " vs=" << v_stride
              << " fmt=" << fmt;
    PrintHexPreview(data, size, 16);
    std::cout << std::endl;

    if (!display_ctx) {
        return;
    }

    if (fmt != MPP_FMT_YUV420SP) {
        if (!display_ctx->warned_non_nv12) {
            std::cerr << "[mpp-callback] current frame fmt is not NV12, skip dump. fmt="
                      << fmt << std::endl;
            display_ctx->warned_non_nv12 = true;
        }
        return;
    }

    if (fd < 0) {
        std::cerr << "[mpp-callback] frame has invalid dma fd, skip display" << std::endl;
        return;
    }

    {
        std::lock_guard<std::mutex> lock(display_ctx->mutex);
        display_ctx->latest_frame.fd = fd;
        display_ctx->latest_frame.width = width;
        display_ctx->latest_frame.height = height;
        display_ctx->latest_frame.h_stride = h_stride;
        display_ctx->latest_frame.v_stride = v_stride;
        display_ctx->has_frame = true;
    }
    display_ctx->cv.notify_one();
}

} // namespace

int main(int argc, char const *argv[])
{
    const char *playApi = argc > 1 ? argv[1] : kDefaultPlayApi;
    const char *streamUrl = argc > 2 ? argv[2] : kDefaultStreamUrl;

    std::cout << "[main] playApi=" << playApi << std::endl;
    std::cout << "[main] streamUrl=" << streamUrl << std::endl;
    std::cout << "[main] board can run this binary directly from the project root NFS mount"
              << std::endl;
    std::cout << "[main] DRM device can be overridden with env DRM_DEVICE" << std::endl;

    RkMppDecoder decoder;
    std::memset(&decoder, 0, sizeof(decoder));
    if (rk_mpp_decoder_init(&decoder, MPP_VIDEO_CodingAVC, NULL) != 0) {
        std::cerr << "[main] rk_mpp_decoder_init failed" << std::endl;
        rk_mpp_decoder_deinit(&decoder);
        return 1;
    }

    DisplayContext display_ctx;
    std::thread display_thread(DisplayThreadMain, &display_ctx);
    rk_mpp_decoder_set_frame_callback(&decoder, OnDecodedFrame, &display_ctx);

    SrsToWebRTC app;
    app.setAudioFrameCallback([](rtc::binary frame, rtc::FrameInfo info) {
        std::cout << "[pull-audio] packet size=" << frame.size()
                  << " timestamp=" << info.timestamp << std::endl;
    });
    app.setVideoFrameCallback([&decoder](rtc::binary frame, rtc::FrameInfo info) {
        auto *frameData = reinterpret_cast<uint8_t *>(frame.data());

        std::cout << "[pull-video] annexb_size=" << frame.size()
                  << " timestamp=" << info.timestamp;
        if (info.timestampSeconds) {
            std::cout << " timestampSeconds=" << info.timestampSeconds->count();
        }
        if (!frame.empty()) {
            std::cout << " head="
                      << static_cast<unsigned int>(frameData[0]);
            for (size_t i = 1; i < std::min<size_t>(frame.size(), 4); ++i) {
                std::cout << "," << static_cast<unsigned int>(frameData[i]);
            }
        }
        std::cout << std::endl;

        if (rk_mpp_decoder_send_data(&decoder, frameData, frame.size(), 0) != 0) {
            std::cerr << "[pull-video] rk_mpp_decoder_send_data failed, request stop"
                      << std::endl;
            if (SrsToWebRTC::getInstance()) {
                SrsToWebRTC::getInstance()->setStopRequested(true);
            }
        }
    });

    if (app.initRTC(playApi, streamUrl) != 0) {
        std::cerr << "[main] Failed to initialize pull RTC" << std::endl;
        {
            std::lock_guard<std::mutex> lock(display_ctx.mutex);
            display_ctx.stop = true;
        }
        display_ctx.cv.notify_one();
        if (display_thread.joinable()) {
            display_thread.join();
        }
        rk_mpp_decoder_deinit(&decoder);
        return 1;
    }

    while (1) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (app.getStopRequested()) {
            break;
        }
    }

    std::cout << "[main] stop requested, flushing decoder" << std::endl;
    rk_mpp_decoder_send_data(&decoder, NULL, 0, 1);
    app.Stop();

    {
        std::lock_guard<std::mutex> lock(display_ctx.mutex);
        display_ctx.stop = true;
    }
    display_ctx.cv.notify_one();
    if (display_thread.joinable()) {
        display_thread.join();
    }
    rk_mpp_decoder_deinit(&decoder);
    return 0;
}
