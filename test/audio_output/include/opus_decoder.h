#ifndef OPUS_DECODER_H
#define OPUS_DECODER_H

/*
 * 让 C 头文件在 C++ 编译器下仍然保持 C ABI。
 */
#ifdef __cplusplus
extern "C" {
#endif

#include <opus/opus.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Opus 解码完成后对外抛出的 PCM 回调。
 *
 * 这个层级只负责“解码”，不负责“播放”。
 * 所以一旦把 Opus 解成 PCM，就通过这个回调继续交给上层。
 */
typedef void (*opus_pcm_callback_t)(const uint8_t *pcm,
                                    size_t frames,
                                    size_t frame_bytes,
                                    void *user_data);

/*
 * Opus 解码器配置。
 *
 * max_frame_samples 是一个上限值：
 * - Opus 一次可能解出不同长度的 PCM
 * - 所以需要预留一个足够大的输出缓冲区
 */
typedef struct opus_decoder_config {
    int sample_rate;
    int channels;
    int max_frame_samples;
} opus_decoder_config_t;

/*
 * Opus 解码模块运行时上下文。
 *
 * decoder:
 *   libopus 的解码器实例。
 *
 * pcm_buffer:
 *   解码输出缓冲区。每次 opus_decode() 的结果都会先落在这里。
 *
 * pcm_cb:
 *   PCM 回调出口。当前通常由 playback_manager 注册。
 */
typedef struct opus_decoder_ctx {
    OpusDecoder *decoder;
    opus_decoder_config_t config;

    /* 解码完成后的 PCM 回调出口。 */
    opus_pcm_callback_t pcm_cb;
    void *pcm_cb_user_data;

    /* 单次解码输出缓冲区。 */
    opus_int16 *pcm_buffer;
} opus_decoder_ctx_t;

/*
 * 给解码器配置一组默认值。
 */
void audio_opus_decoder_config_init(opus_decoder_config_t *config);

/*
 * 设置解码完成后的 PCM 回调。
 */
void audio_opus_decoder_set_pcm_callback(opus_decoder_ctx_t *ctx,
                                         opus_pcm_callback_t cb,
                                         void *user_data);

/*
 * 初始化 Opus 解码器。
 *
 * 会创建 libopus 解码器，并分配 PCM 输出缓冲区。
 */
int audio_opus_decoder_init(opus_decoder_ctx_t *ctx,
                            const opus_decoder_config_t *config);

/*
 * 把一个 Opus 包送入解码器。
 *
 * 成功时：
 * - 调用 opus_decode()
 * - 得到 PCM
 * - 再通过 pcm_cb 把 PCM 抛给上层
 */
int audio_opus_decoder_push_packet(opus_decoder_ctx_t *ctx,
                                   const uint8_t *packet,
                                   size_t packet_len);

/*
 * 关闭解码模块并释放资源。
 */
void audio_opus_decoder_close(opus_decoder_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif
