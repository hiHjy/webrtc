
#include "drm_display.hpp"
#include "rkmpp_dec.h"
#include "rkmpp_enc.h"
#include "srstowebrtc.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <thread>

namespace {

/*
 * 这份 main.cpp 是整个 demo 的“总装配层”。
 *
 * 它本身不做复杂算法，职责主要是把几条链路接起来：
 *
 * 1. SrsToWebRTC
 *    从 SRS 拉 WebRTC 流，回调里拿到音视频数据。
 *
 * 2. RkMppDecoder
 *    把收到的 H.264 Annex-B 码流送进 MPP 解码器。
 *
 * 3. 解码回调 OnDecodedFrame()
 *    一旦解出一帧 NV12，就做三件事：
 *    - 用 CPU 可见指针 data 把原始 NV12 写到 1.nv12
 *    - 把 dma-buf fd 交给 DRM 显示线程
 *    - 把同一个 dma-buf fd 交给编码器再编码一次
 *
 * 4. DRM 显示线程
 *    单独线程串行处理页面翻转，避免阻塞 WebRTC / MPP 主链路。
 *
 * 所以整条路径可以简单记成：
 *   WebRTC 拉流 -> 解码 -> 回调 -> 显示 + dump + 编码
 */

constexpr const char *kDefaultPlayApi = "http://192.168.101.68:1985/rtc/v1/play/";
constexpr const char *kDefaultStreamUrl = "webrtc://192.168.101.68/live/livestream";
constexpr const char *kDumpNv12Path = "build-aarch64/1.nv12";
constexpr int kDumpNv12FrameCount = 400;

/*
 * 打印前几个字节，主要是为了快速判断：
 * - 码流是不是像 Annex-B / H.264
 * - 原始图像数据是不是拿到了
 *
 * 它只是调试辅助函数，不参与业务逻辑。
 */
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

struct AppContext {
    /*
     * 显示线程只需要知道“最新一帧该怎么显示”。
     *
     * 这里故意只保存 fd 和图像几何信息，不拷整帧数据。
     * 原因是：
     * - 显示走的是 dma-buf 直显
     * - fd 已经足够让 DRM 导入这帧
     * - 不再做一份 CPU 拷贝可以减少延迟和带宽浪费
     */
    struct PendingFrame {
        int fd = -1;
        RK_U32 width = 0;
        RK_U32 height = 0;
        RK_U32 h_stride = 0;
        RK_U32 v_stride = 0;
    };

    /* DRM 上下文和它对应的线程同步原语 */
    DRM_Ctx drm_ctx;
    std::mutex display_mutex;
    std::condition_variable display_cv;
    PendingFrame latest_frame;
    bool has_frame = false;
    bool stop = false;
    bool warned_non_nv12 = false;

    /*
     * 编码器和显示不同，它目前还是同步调用。
     * 所以这里用单独 mutex 保证：
     * - 初始化只做一次
     * - 后续送帧时不会和 deinit 交叉
     */
    std::mutex encoder_mutex;
    RkMppEncoder encoder;
    bool encoder_initialized = false;

    /*
     * 原始 NV12 dump 状态。
     *
     * 这里写的是“解码后的原始图像”，不是编码后的码流。
     * 路径固定在 build-aarch64/1.nv12，最多写 400 帧。
     */
    FILE *nv12_dump_file = nullptr;
    int nv12_dumped_frames = 0;
    bool nv12_dump_done = false;
};

/*
 * 编码成功后的回调。
 *
 * 目前这里只打印码流基本信息，方便你确认：
 * - 编码器确实有输出
 * - 输出大小是否合理
 * - 前几个字节是不是像正常 H.264 Annex-B
 *
 * 如果后面你要推流/封装，这里就是最自然的出口。
 */
void OnEncodedPacket(const uint8_t *data,
                     size_t size,
                     int is_header,
                     int eos,
                     void *) {
    std::cout << "[enc-callback] type=" << (is_header ? "header" : "frame")
              << " size=" << size
              << " eos=" << eos;
    PrintHexPreview(data, size, 16);
    std::cout << std::endl;
}

/*
 * 编码器按“懒初始化”方式创建。
 *
 * 原因是解码前，我们还不确定真正输出的宽高、stride、fmt。
 * 只有当解码器真的吐出第一帧后，这些参数才是可信的。
 *
 * 这个函数只做一次初始化，后面再次调用会直接返回 true。
 */
bool EnsureEncoderReady(AppContext *app_ctx,
                        RK_U32 width,
                        RK_U32 height,
                        RK_U32 h_stride,
                        RK_U32 v_stride,
                        RK_U32 fmt) {
    if (!app_ctx) {
        return false;
    }

    std::lock_guard<std::mutex> lock(app_ctx->encoder_mutex);
    if (app_ctx->encoder_initialized) {
        return true;
    }

    if (rk_mpp_encoder_init(&app_ctx->encoder,
                            MPP_VIDEO_CodingAVC,
                            width,
                            height,
                            h_stride,
                            v_stride,
                            static_cast<MppFrameFormat>(fmt),
                            30,
                            0,
                            0,
                            nullptr) != 0) {
        std::cerr << "[main] rk_mpp_encoder_init failed" << std::endl;
        return false;
    }

    rk_mpp_encoder_set_packet_callback(&app_ctx->encoder, OnEncodedPacket, app_ctx);
    if (rk_mpp_encoder_write_header(&app_ctx->encoder) != 0) {
        std::cerr << "[main] rk_mpp_encoder_write_header failed" << std::endl;
        rk_mpp_encoder_deinit(&app_ctx->encoder);
        std::memset(&app_ctx->encoder, 0, sizeof(app_ctx->encoder));
        return false;
    }

    app_ctx->encoder_initialized = true;
    std::cout << "[main] encoder ready width=" << width
              << " height=" << height
              << " hs=" << h_stride
              << " vs=" << v_stride
              << " fmt=" << fmt << std::endl;
    return true;
}

/*
 * DRM 显示线程主函数。
 *
 * 回调线程只负责把“最新一帧”的 fd 塞进共享槽位并唤醒这里；
 * 真正的原子提交和 page flip 等待都放在这个线程做。
 *
 * 这样设计的目的很直接：
 * - DRM 页面翻转可能会等待事件
 * - 如果在解码/拉流回调里直接做显示，会把主链路卡住
 *
 * 所以这里本质上是一个“最新帧显示器”：
 * 旧帧来不及显示时允许被覆盖，只显示最新的一帧。
 */
void DisplayThreadMain(AppContext *app_ctx) {
    if (!app_ctx) {
        return;
    }

    if (drmInit(&app_ctx->drm_ctx) != 0) {
        std::cerr << "[display-thread] drmInit failed" << std::endl;
        if (SrsToWebRTC::getInstance()) {
            SrsToWebRTC::getInstance()->setStopRequested(true);
        }
        return;
    }

    while (true) {
        AppContext::PendingFrame frame;

        {
            std::unique_lock<std::mutex> lock(app_ctx->display_mutex);
            app_ctx->display_cv.wait(lock, [app_ctx]() {
                return app_ctx->stop || app_ctx->has_frame;
            });

            if (app_ctx->stop && !app_ctx->has_frame) {
                break;
            }

            frame = std::move(app_ctx->latest_frame);
            app_ctx->latest_frame = {};
            app_ctx->has_frame = false;
        }

        DRM_Buf buf;
        std::memset(&buf, 0, sizeof(buf));
        buf.dma_fd = frame.fd;
        buf.size = static_cast<uint64_t>(frame.h_stride) * frame.v_stride * 3 / 2;
        buf.w = static_cast<int>(frame.width);
        buf.h = static_cast<int>(frame.height);
        buf.fmt = DRM_FORMAT_NV12;
        buf.pitches[0] = frame.h_stride;
        buf.pitches[1] = frame.h_stride;
        buf.offsets[0] = 0;
        buf.offsets[1] = frame.h_stride * frame.v_stride;
        buf.modifier = 0;

        while (true) {
            if (drmDisplaySubmit(&app_ctx->drm_ctx, &buf) == 0) {
                break;
            }

            if (errno == EAGAIN) {
                const int event_ret = drmHandleEvents(&app_ctx->drm_ctx, 3000);
                if (event_ret >= 0) {
                    continue;
                }
                std::cerr << "[display-thread] drmHandleEvents failed ret="
                          << event_ret << std::endl;
            } else {
                std::cerr << "[display-thread] drmDisplaySubmit failed errno="
                          << errno << std::endl;
            }

            if (SrsToWebRTC::getInstance()) {
                SrsToWebRTC::getInstance()->setStopRequested(true);
            }
            drmDeinit(&app_ctx->drm_ctx);
            std::cout << "[display-thread] exit" << std::endl;
            return;
        }
    }

    /*
     * 如果还有 pending page flip，尽量在退出前消费一次事件，
     * 这样三缓冲状态能正常落稳。
     */
    drmHandleEvents(&app_ctx->drm_ctx, 0);
    drmDeinit(&app_ctx->drm_ctx);
    std::cout << "[display-thread] exit" << std::endl;
}

/*
 * 把解码出来的 NV12 写成标准 rawvideo 文件。
 *
 * 注意这里为什么用 data 写文件，而不是用 fd：
 * - data 是一块当前帧可直接访问的 CPU 指针
 * - 写 rawvideo 文件本质就是把像素字节顺序写出去
 * - 这件事不需要 DRM，也不需要再导入 dma-buf
 *
 * 同时这里按 width/height 逐行写，而不是把整块 buffer 原样 fwrite。
 * 这么做是为了去掉 stride 带来的填充字节，让 1.nv12 可以直接被 ffplay 播放。
 */
void DumpNv12Frame(AppContext *app_ctx,
                   const uint8_t *data,
                   size_t size,
                   RK_U32 width,
                   RK_U32 height,
                   RK_U32 h_stride,
                   RK_U32 v_stride) {
    if (!app_ctx || !data || size == 0 || app_ctx->nv12_dump_done) {
        return;
    }

    if (!app_ctx->nv12_dump_file) {
        app_ctx->nv12_dump_file = std::fopen(kDumpNv12Path, "wb");
        if (!app_ctx->nv12_dump_file) {
            std::perror("fopen build-aarch64/1.nv12");
            app_ctx->nv12_dump_done = true;
            return;
        }

        std::cout << "[main] start dumping NV12 to " << kDumpNv12Path
                  << " frames=" << kDumpNv12FrameCount
                  << " size=" << width << "x" << height << std::endl;
    }

    if (h_stride < width || v_stride < height) {
        std::cerr << "[main] invalid NV12 stride for dump width=" << width
                  << " height=" << height
                  << " hs=" << h_stride
                  << " vs=" << v_stride << std::endl;
        app_ctx->nv12_dump_done = true;
        std::fclose(app_ctx->nv12_dump_file);
        app_ctx->nv12_dump_file = nullptr;
        return;
    }

    const size_t y_plane_size = static_cast<size_t>(h_stride) * v_stride;
    const size_t required_size = y_plane_size + static_cast<size_t>(h_stride) * (height / 2);
    if (size < required_size) {
        std::cerr << "[main] NV12 dump buffer too small size=" << size
                  << " required=" << required_size << std::endl;
        app_ctx->nv12_dump_done = true;
        std::fclose(app_ctx->nv12_dump_file);
        app_ctx->nv12_dump_file = nullptr;
        return;
    }

    const uint8_t *y_plane = data;
    const uint8_t *uv_plane = data + y_plane_size;

    for (RK_U32 y = 0; y < height; ++y) {
        std::fwrite(y_plane + static_cast<size_t>(y) * h_stride, 1, width, app_ctx->nv12_dump_file);
    }

    for (RK_U32 y = 0; y < height / 2; ++y) {
        std::fwrite(uv_plane + static_cast<size_t>(y) * h_stride, 1, width, app_ctx->nv12_dump_file);
    }

    std::fflush(app_ctx->nv12_dump_file);
    app_ctx->nv12_dumped_frames++;

    if (app_ctx->nv12_dumped_frames >= kDumpNv12FrameCount) {
        std::fclose(app_ctx->nv12_dump_file);
        app_ctx->nv12_dump_file = nullptr;
        app_ctx->nv12_dump_done = true;
        std::cout << "[main] NV12 dump finished path=" << kDumpNv12Path
                  << " dumped_frames=" << app_ctx->nv12_dumped_frames << std::endl;
    }
}

/*
 * 解码器吐出一帧后的统一回调。
 *
 * 这是整份 main.cpp 最关键的入口。
 *
 * 一旦进到这里，说明：
 * - 当前这帧已经从压缩码流变成了原始图像
 * - data 指向 CPU 可读的 NV12 内容
 * - fd 是这帧底层 buffer 对应的 dma-buf
 *
 * 然后这里按顺序做三件事：
 * 1. 可选把 NV12 写到文件
 * 2. 通知显示线程去直显
 * 3. 确保编码器初始化完成，并把这帧再送去编码
 *
 * 这也是为什么同一个回调里同时用到了 data 和 fd：
 * - dump 文件要用 data
 * - DRM 显示和 MPP 编码更适合直接复用 fd
 */
void OnDecodedFrame(const uint8_t *data,
                    size_t size,
                    int fd,
                    RK_U32 width,
                    RK_U32 height,
                    RK_U32 h_stride,
                    RK_U32 v_stride,
                    RK_U32 fmt,
                    void *userdata) {
    auto *app_ctx = static_cast<AppContext *>(userdata);
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

    if (!app_ctx) {
        return;
    }

    /*
     * 这里只接受 NV12。
     *
     * 当前显示路径、dump 路径、以及 encoder 初始化时默认使用的 fmt，
     * 都是围绕 NV12 配的。
     */
    if (fmt != MPP_FMT_YUV420SP) {
        if (!app_ctx->warned_non_nv12) {
            std::cerr << "[mpp-callback] current frame fmt is not NV12, skip dump. fmt="
                      << fmt << std::endl;
            app_ctx->warned_non_nv12 = true;
        }
        return;
    }

    if (fd < 0) {
        std::cerr << "[mpp-callback] frame has invalid dma fd, skip display" << std::endl;
        return;
    }

    /*
     * 先用 data 做原始帧 dump。
     *
     * 这一段和后面的显示/编码没有互斥关系，因为写文件用的是 data，
     * 显示/编码用的是 fd，职责是分开的。
     */
    DumpNv12Frame(app_ctx, data, size, width, height, h_stride, v_stride);

    {
        /*
         * 只保留最新一帧给显示线程。
         *
         * 如果显示线程跟不上，旧帧会被新帧覆盖，这是有意为之。
         * 对实时预览来说，“最新画面”比“每一帧都不能丢”更重要。
         */
        std::lock_guard<std::mutex> lock(app_ctx->display_mutex);
        app_ctx->latest_frame.fd = fd;
        app_ctx->latest_frame.width = width;
        app_ctx->latest_frame.height = height;
        app_ctx->latest_frame.h_stride = h_stride;
        app_ctx->latest_frame.v_stride = v_stride;
        app_ctx->has_frame = true;
    }
    app_ctx->display_cv.notify_one();

    /*
     * 编码器第一次真正需要工作时再初始化。
     * 这样它拿到的是解码器输出的真实几何参数。
     */
    if (!EnsureEncoderReady(app_ctx, width, height, h_stride, v_stride, fmt)) {
        if (SrsToWebRTC::getInstance()) {
            SrsToWebRTC::getInstance()->setStopRequested(true);
        }
        return;
    }

    {
        /*
         * 再编码这里直接复用解码帧的 dma-buf fd。
         *
         * 也就是说，这里没有再做一份 NV12 内存拷贝，而是把外部 buffer
         * import 给编码器使用。
         */
        std::lock_guard<std::mutex> lock(app_ctx->encoder_mutex);
        if (rk_mpp_encoder_send_frame(&app_ctx->encoder, fd, 0) != 0) {
            std::cerr << "[mpp-callback] rk_mpp_encoder_send_frame failed" << std::endl;
            if (SrsToWebRTC::getInstance()) {
                SrsToWebRTC::getInstance()->setStopRequested(true);
            }
            return;
        }
    }
}

} // namespace

int main(int argc, char const *argv[])
{
    /*
     * main 做的事情其实非常线性：
     * 1. 解析默认拉流地址
     * 2. 初始化 decoder
     * 3. 启动 DRM 显示线程
     * 4. 注册解码回调
     * 5. 配置 WebRTC 音视频回调
     * 6. 启动拉流
     * 7. 主线程 while(1) 等待 stop
     * 8. 按顺序停 WebRTC / 停显示线程 / 释放 encoder / decoder
     */
    const char *playApi = argc > 1 ? argv[1] : kDefaultPlayApi;
    const char *streamUrl = argc > 2 ? argv[2] : kDefaultStreamUrl;

    std::cout << "[main] playApi=" << playApi << std::endl;
    std::cout << "[main] streamUrl=" << streamUrl << std::endl;
    std::cout << "[main] board can run this binary directly from the project root NFS mount"
              << std::endl;
    std::cout << "[main] DRM device can be overridden with env DRM_DEVICE" << std::endl;

    RkMppDecoder decoder;
    std::memset(&decoder, 0, sizeof(decoder));
    /* decoder 初始化后，还不会马上解出画面，真正工作要等收到视频码流 */
    if (rk_mpp_decoder_init(&decoder, MPP_VIDEO_CodingAVC, NULL) != 0) {
        std::cerr << "[main] rk_mpp_decoder_init failed" << std::endl;
        rk_mpp_decoder_deinit(&decoder);
        return 1;
    }

    AppContext app_ctx;
    std::memset(&app_ctx.encoder, 0, sizeof(app_ctx.encoder));
    /* DRM 单独线程，避免显示阻塞拉流/解码主链路 */
    std::thread display_thread(DisplayThreadMain, &app_ctx);

    /* 解码器一旦吐出一帧，就会进入 OnDecodedFrame() */
    rk_mpp_decoder_set_frame_callback(&decoder, OnDecodedFrame, &app_ctx);

    SrsToWebRTC app;
    /* 当前 demo 不处理音频，只打印一下观察收流是否正常 */
    app.setAudioFrameCallback([](rtc::binary frame, rtc::FrameInfo info) {
        std::cout << "[pull-audio] packet size=" << frame.size()
                  << " timestamp=" << info.timestamp << std::endl;
    });

    /*
     * 视频回调拿到的是压缩码流，不是原始图像。
     *
     * 这里的 frameData 仍然是 H.264 Annex-B NAL 数据，
     * 所以这里做的事情非常单纯：直接送进解码器。
     *
     * 真正的“拿到原始 NV12 帧”是在 decoder 的回调里，而不是这里。
     */
    app.setVideoFrameCallback([&decoder, &app_ctx](rtc::binary frame, rtc::FrameInfo info) {
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

    /* 真正向 SRS 发起 WebRTC play 请求 */
    if (app.initRTC(playApi, streamUrl) != 0) {
        std::cerr << "[main] Failed to initialize pull RTC" << std::endl;
        {
            std::lock_guard<std::mutex> lock(app_ctx.display_mutex);
            app_ctx.stop = true;
        }
        app_ctx.display_cv.notify_one();
        if (display_thread.joinable()) {
            display_thread.join();
        }
        if (app_ctx.encoder_initialized) {
            rk_mpp_encoder_deinit(&app_ctx.encoder);
        }
        if (app_ctx.nv12_dump_file) {
            std::fclose(app_ctx.nv12_dump_file);
            app_ctx.nv12_dump_file = nullptr;
        }
        rk_mpp_decoder_deinit(&decoder);
        return 1;
    }

    /*
     * 主线程本身不处理视频，它只是作为“生命周期管理器”存在。
     *
     * 真正干活的是：
     * - SrsToWebRTC 内部线程/回调
     * - MPP 解码回调
     * - DRM 显示线程
     *
     * 所以这里保留一个简单 while(1) 轮询 stopRequested 就够了。
     */
    while (1) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (app.getStopRequested()) {
            break;
        }
    }

    /*
     * 退出顺序很重要：
     * 1. 先给 decoder 送 eos，尽量把内部残留帧吐干净
     * 2. 停止 WebRTC
     * 3. 通知显示线程退出并 join
     * 4. 最后再释放 encoder / dump 文件 / decoder
     *
     * 这样可以尽量避免“线程还在用 fd，但底层资源已经被释放”的问题。
     */
    std::cout << "[main] stop requested, flushing decoder" << std::endl;
    rk_mpp_decoder_send_data(&decoder, NULL, 0, 1);
    app.Stop();

    {
        std::lock_guard<std::mutex> lock(app_ctx.display_mutex);
        app_ctx.stop = true;
    }
    app_ctx.display_cv.notify_one();
    if (display_thread.joinable()) {
        display_thread.join();
    }
    if (app_ctx.encoder_initialized) {
        rk_mpp_encoder_deinit(&app_ctx.encoder);
    }
    if (app_ctx.nv12_dump_file) {
        std::fclose(app_ctx.nv12_dump_file);
        app_ctx.nv12_dump_file = nullptr;
    }
    rk_mpp_decoder_deinit(&decoder);
    return 0;
}
