#ifndef AUDIO_CAPTURE_H
#define AUDIO_CAPTURE_H

/*
 * 让 C 头文件里的接口在 C++ 编译器下也保持 C ABI。
 * 后面如果你的上层是 C++，直接 include 这个头文件也不会有符号名改编问题。
 */
#ifdef __cplusplus
extern "C" {
#endif

/*
 * 某些系统头会根据 _POSIX_C_SOURCE 决定是否暴露 POSIX 接口。
 * 这里统一定义，避免不同源文件里可见的接口集合不一致。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

/*
 * 采集层对外暴露的 PCM 回调。
 *
 * data:
 *   指向本次采集到的原始 PCM 数据，数据就放在 ctx->buffer 里。
 *
 * frames:
 *   本次读到的“帧数”，一帧表示同一个采样时刻下所有声道的数据。
 *
 * frame_bytes:
 *   一帧的字节数。对 S16_LE 单声道来说是 2，对 S16_LE 双声道来说是 4。
 *
 * user_data:
 *   上层注册回调时传入的私有指针，通常会传自己的 manager / stream 上下文。
 *
 * 设计意图：
 *   audio_capture 只负责采集，不负责后续怎么处理 PCM。
 *   后续要做 Opus 编码、VAD、AEC、发送，全部交给回调的实现者。
 */
typedef void (*audio_pcm_callback_t)(const uint8_t *data,
                                     snd_pcm_sframes_t frames,
                                     size_t frame_bytes,
                                     void *user_data);

/*
 * 纯采集模块的运行时上下文。
 *
 * 这个结构体只关心 ALSA 采集本身：
 * - 设备句柄
 * - 实际生效的采集参数
 * - 线程状态
 * - 一块可复用的 PCM 缓冲区
 * - 一组上抛 PCM 的回调信息
 *
 * 它不关心：
 * - Opus 编码
 * - 网络发送
 * - WebRTC
 *
 * 这样做的好处是，采集层和后处理层可以解耦，后面要换编码器或加前处理都不用改 ALSA 层。
 */
typedef struct audio_capture_ctx {
    /* ALSA PCM capture 句柄。 */
    snd_pcm_t *pcm_handle;

    /* 最终生效的采集格式参数。 */
    snd_pcm_format_t format;
    unsigned int sample_rate;
    unsigned int channels;
    snd_pcm_uframes_t period_size;

    /*
     * frame_bytes:
     *   一帧的字节数 = 每个采样点字节数 * 声道数。
     *
     * buffer_bytes:
     *   当前 buffer 总字节数 = period_size * frame_bytes。
     */
    size_t frame_bytes;
    size_t buffer_bytes;

    /*
     * 采集线程每次把一包数据读到这块缓冲区里。
     * 回调拿到的 data 指针，本质上就是指向这块内存。
     */
    uint8_t *buffer;

    /*
     * requestStop:
     *   外部请求停止时置 1，采集线程会尽快退出。
     *
     * isRunning:
     *   只是一个轻量状态位，用于表达后台线程当前是否处于采集中。
     */
    uint8_t requestStop;
    uint8_t isRunning;

    /* 后台采集线程句柄。 */
    pthread_t capture_thread;

    /*
     * PCM 上抛回调。
     * 如果你上层有 audio_stream_manager，这里一般会挂 manager 的回调函数。
     */
    audio_pcm_callback_t pcm_cb;
    void *pcm_cb_user_data;
} audio_capture_ctx_t;

/*
 * 注册 PCM 回调。
 *
 * 这个接口只保存函数指针和 user_data，不会立刻触发任何采集行为。
 * 真正开始采集要调用 audio_start_capture()。
 */
void audio_capture_set_callback(audio_capture_ctx_t *ctx,
                                audio_pcm_callback_t cb,
                                void *user_data);

/*
 * 初始化 ALSA 采集模块。
 *
 * device/sample_rate/channels/format/period_size 都是“期望值”。
 * 因为用了 near 类接口，驱动最终接受的实际值可能会和期望值略有不同，
 * 所以初始化完成后，应当以 ctx 里保存的实际值为准。
 */
int audio_capture_init(audio_capture_ctx_t *ctx,
                       const char *device,
                       unsigned int sample_rate,
                       unsigned int channels,
                       snd_pcm_format_t format,
                       snd_pcm_uframes_t period_size);

/*
 * 同步读取一包 PCM 数据。
 *
 * 这个接口主要给内部线程用，也可以给你以后做调试或同步采集模式时用。
 * 返回值是“本次成功读取到的帧数”，负值表示错误。
 */
snd_pcm_sframes_t audio_capture_read(audio_capture_ctx_t *ctx,
                                     snd_pcm_uframes_t frames);

/*
 * 启动后台采集线程。
 * 启动后线程会持续调用 audio_capture_read()，并把 PCM 通过回调抛出去。
 */
int audio_start_capture(audio_capture_ctx_t *ctx);

/*
 * 请求停止采集。
 * 这里只负责“发停止信号 + 打断阻塞 read”，并不释放资源。
 */
void audio_stop_capture(audio_capture_ctx_t *ctx);

/*
 * 关闭采集模块。
 *
 * 它会做完整清理：
 * - stop
 * - join 线程
 * - 关闭 ALSA 句柄
 * - 释放 buffer
 * - 清空上下文
 */
void audio_capture_close(audio_capture_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif
