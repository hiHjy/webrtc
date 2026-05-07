#ifndef OPUS_PROCESS_H
#define OPUS_PROCESS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <opus/opus.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Opus 模块对外抛出的编码包回调。
 *
 * 这个层级只负责“编码”，不负责“发送”。
 * 所以一旦编码出一个包，就通过这个回调交给上层。
 */
typedef int (*opus_packet_callback_t)(const uint8_t *packet,
                                      size_t packet_len,
                                      void *user_data);

/*
 * Opus 编码器配置。
 *
 * frame_samples 是很关键的参数：
 * - 48kHz 下，960 表示 20ms
 * - Opus 要求按固定帧长编码
 * - 所以外面哪怕每次送进来的 PCM 块大小不一致，这一层也会自己攒够一帧再编码
 */
typedef struct opus_encoder_config {
    int sample_rate;
    int channels;
    int application;
    int bitrate;
    int frame_samples;
    int max_packet_size;
} opus_encoder_config_t;

/*
 * Opus 模块运行时上下文。
 *
 * encoder:
 *   libopus 的编码器实例。
 *
 * pcm_cache:
 *   用来攒 PCM 的缓存。因为采集层每次抛上来的块不一定正好等于一个 Opus 帧长，
 *   所以这里需要一块内部缓存把碎片拼成完整帧。
 *
 * packet_buffer:
 *   编码输出缓冲区，每次 opus_encode() 都会写到这里。
 */
typedef struct opus_encoder_ctx {
    OpusEncoder *encoder;
    opus_encoder_config_t config;

    /* 编码完成后的回调出口。 */
    opus_packet_callback_t packet_cb;
    void *packet_cb_user_data;

    /* PCM 攒帧缓存。 */
    opus_int16 *pcm_cache;

    /* 单个 Opus 包输出缓冲区。 */
    unsigned char *packet_buffer;

    /* 当前已经攒了多少帧 PCM。 */
    int cached_frames;
} opus_encoder_ctx_t;

/*
 * 给编码器配置一组默认值。
 * 这样上层可以只覆写自己真正关心的参数。
 */
void audio_opus_encoder_config_init(opus_encoder_config_t *config);

/*
 * 设置编码完成后的回调。
 * 这个回调通常由 manager 层注册。
 */
void audio_opus_encoder_set_packet_callback(opus_encoder_ctx_t *ctx,
                                            opus_packet_callback_t cb,
                                            void *user_data);

/*
 * 初始化 Opus 模块。
 *
 * 会创建 libopus 编码器，并分配：
 * - PCM 攒帧缓存
 * - Opus 包输出缓存
 */
int audio_opus_encoder_init(opus_encoder_ctx_t *ctx,
                            const opus_encoder_config_t *config);

/*
 * 把 PCM 数据喂给 Opus 模块。
 *
 * 注意这个接口并不保证“每调一次就编码一次”。
 * 它的真实行为是：
 * - 先把 PCM 复制进内部缓存
 * - 如果还没凑够一个完整 Opus 帧，就先攒着
 * - 一旦攒满 frame_samples，就调用 opus_encode()
 * - 编完后再通过 packet_cb 把 Opus 包抛给上层
 */
int audio_opus_encoder_push_pcm(opus_encoder_ctx_t *ctx,
                                const uint8_t *data,
                                size_t frames,
                                size_t frame_bytes);

/*
 * 把尾巴上不足一帧的 PCM 补零后编码出去。
 * 一般在 stop/close 时调用，避免最后一点音频丢掉。
 */
int audio_opus_encoder_flush(opus_encoder_ctx_t *ctx);

/*
 * 关闭 Opus 模块并释放所有资源。
 */
void audio_opus_encoder_close(opus_encoder_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif
