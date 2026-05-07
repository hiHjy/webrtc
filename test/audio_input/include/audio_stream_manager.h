#ifndef AUDIO_STREAM_MANAGER_H
#define AUDIO_STREAM_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "audio_capture.h"
#include "opus_encoder.h"

#include <alsa/asoundlib.h>
#include <stdint.h>

/*
 * manager 层对外抛出的 Opus 包回调。
 *
 * 这个回调是“交付点”：
 * - manager 负责把麦克风 PCM 采上来
 * - manager 再把 PCM 喂给 Opus 模块编码
 * - 当 Opus 产出编码包时，manager 最后通过这个回调把包交给上层
 *
 * 你以后如果要接发送逻辑，最自然的挂点就是这里。
 */
typedef int (*audio_stream_packet_callback_t)(const uint8_t *packet,
                                              size_t packet_len,
                                              void *user_data);

/*
 * manager 层的配置。
 *
 * 它把“采集参数”和“编码参数”都放在一起，
 * 因为对业务层来说，你通常是希望一次配置整条音频流。
 */
typedef struct audio_stream_manager_config {
    /* ALSA 采集参数。 */
    char device[64];
    unsigned int sample_rate;
    unsigned int channels;
    snd_pcm_format_t format;//s16_le、s24_le、s32_le
    snd_pcm_uframes_t period_size;

    /* Opus 编码参数。 */
    int opus_application;
    int opus_bitrate;
    int opus_frame_samples;
    int opus_max_packet_size;
} audio_stream_manager_config_t;

/*
 * manager 是整条链路的“总控层”。
 *
 * 它内部同时持有：
 * - 一个采集模块
 * - 一个 Opus 编码模块
 * - 一份配置
 * - 一个对外的 Opus 包回调
 *
 * 这样上层只需要跟 manager 打交道，不用分别管理 capture 和 opus。
 */
typedef struct audio_stream_manager {
    /* 纯采集层上下文。 */
    audio_capture_ctx_t capture;

    /* 纯 Opus 编码层上下文。 */
    opus_encoder_ctx_t encoder;

    /* 当前 manager 的工作配置。 */
    audio_stream_manager_config_t config;

    /*
     * 当 Opus 模块产出一个编码包后，manager 最终通过这里把它交给业务层。
     * 业务层可以在这个回调里做发送、缓存、调试存盘等动作。
     */
    audio_stream_packet_callback_t packet_cb;
    void *packet_cb_user_data;

    /* 表示 config 是否已经初始化完成。 */
    uint8_t is_initialized;
} audio_stream_manager_t;

/*
 * 给 manager 配一组默认参数。
 *
 * 默认值的目的是让你最少填字段也能起一条流，
 * 调用方只改自己关心的那几个参数即可。
 */
void audio_stream_manager_config_init(audio_stream_manager_config_t *config);

/*
 * 设置最终的 Opus 包输出回调。
 *
 * 这不是 PCM 回调，而是“已经编码好的 Opus 包”的回调。
 * 所以这个接口更靠近发送层。
 */
void audio_stream_manager_set_packet_callback(audio_stream_manager_t *stream,
                                              audio_stream_packet_callback_t cb,
                                              void *user_data);

/*
 * 初始化 manager。
 *
 * 这里只做“保存配置、准备上下文”，并不会立刻打开麦克风或创建编码器。
 * 真正开始整条链路要调用 audio_stream_manager_start()。
 */
int audio_stream_manager_init(audio_stream_manager_t *stream,
                              const audio_stream_manager_config_t *config);

/*
 * 启动整条音频流。
 *
 * 这个函数会按顺序做三件事：
 * 1. 初始化 Opus 编码模块
 * 2. 初始化 ALSA 采集模块
 * 3. 启动采集线程
 *
 * 启动后，数据流向是：
 * capture thread -> PCM 回调 -> audio_opus_encoder_push_pcm -> Opus 包回调
 */
int audio_stream_manager_start(audio_stream_manager_t *stream);

/*
 * 停止整条音频流。
 *
 * 当前实现里它主要转给 audio_capture_stop()，
 * 因为采集线程一停，整条数据流就会停止继续往下走。
 */
void audio_stream_manager_stop(audio_stream_manager_t *stream);

/*
 * 关闭整条音频流并释放资源。
 *
 * 这个接口适合在退出时统一调用。
 * 当前会：
 * - 关闭采集模块
 * - flush Opus 尾帧
 * - 关闭 Opus 模块
 */
void audio_stream_manager_close(audio_stream_manager_t *stream);

#ifdef __cplusplus
}
#endif

#endif
