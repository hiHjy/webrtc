#include "opus_decoder.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
/*
 * 把 Opus 解码上下文恢复到“空状态”。
 */
static void audio_opus_decoder_reset(opus_decoder_ctx_t *ctx) {
    if (ctx == NULL) {
        return;
    }

    memset(ctx, 0, sizeof(*ctx));
}

/*
 * 默认解码配置。
 *
 * 这里默认对齐当前链路：
 * - 48kHz
 * - 单声道
 * - 最大解码帧长预留到 5760 samples
 */
void audio_opus_decoder_config_init(opus_decoder_config_t *config) {
    if (config == NULL) {
        return;
    }

    memset(config, 0, sizeof(*config));
    config->sample_rate = 48000;
    config->channels = 1;
    config->max_frame_samples = 5760;
}

/*
 * 注册“解码完成后的 PCM 回调”。
 *
 * 上层一般会把 playback_manager 的回调挂到这里。
 */
void audio_opus_decoder_set_pcm_callback(opus_decoder_ctx_t *ctx,
                                         opus_pcm_callback_t cb,
                                         void *user_data) {
    if (ctx == NULL) {
        return;
    }

    ctx->pcm_cb = cb;
    ctx->pcm_cb_user_data = user_data;
   
}

/*
 * 初始化 Opus 解码模块。
 *
 * 主要做三件事：
 * 1. 关闭旧实例，避免重复 init 泄漏资源
 * 2. 创建 libopus 解码器
 * 3. 分配 PCM 输出缓冲区
 */
int audio_opus_decoder_init(opus_decoder_ctx_t *ctx,
                            const opus_decoder_config_t *config) {
    opus_decoder_config_t effective_config;
    int err = 0;

    if (ctx == NULL) {
        return -EINVAL;
    }

    /* 先把旧实例清掉，再按默认值准备一份新配置。 */
    audio_opus_decoder_close(ctx);
    audio_opus_decoder_config_init(&effective_config);
    if (config != NULL) {
        effective_config = *config;
    }

    if (effective_config.sample_rate <= 0 ||
        effective_config.channels <= 0 ||
        effective_config.max_frame_samples <= 0) {
        return -EINVAL;
    }

    /* 创建 libopus 解码器实例。 */
    ctx->decoder = opus_decoder_create(effective_config.sample_rate,
                                       effective_config.channels,
                                       &err);
    if (err != OPUS_OK) {
        audio_opus_decoder_reset(ctx);
        return -EINVAL;
    }

    /*
     * 分配单次解码输出缓冲区。
     * 大小 = max_frame_samples * channels。
     */
    ctx->pcm_buffer = (opus_int16 *)calloc((size_t)effective_config.max_frame_samples *
                                               (size_t)effective_config.channels,
                                           sizeof(opus_int16));
    if (ctx->pcm_buffer == NULL) {
        audio_opus_decoder_close(ctx);
        return -ENOMEM;
    }

    ctx->config = effective_config;
    return 0;
}

/*
 * 把一个 Opus 包送入解码器。
 *
 * 成功时：
 * - 调用 opus_decode()
 * - 把结果写到 ctx->pcm_buffer
 * - 再通过 pcm_cb 把 PCM 抛给上层
 */
int audio_opus_decoder_push_packet(opus_decoder_ctx_t *ctx,
                                   const uint8_t *packet,
                                   size_t packet_len) {
    int decoded_samples = 0;

    if (ctx == NULL || ctx->decoder == NULL || packet == NULL || packet_len == 0) {
        return -EINVAL;
    }

    /* 真正执行一次 Opus -> PCM 解码。 */
    decoded_samples = opus_decode(ctx->decoder,
                                  packet,
                                  (opus_int32)packet_len,
                                  ctx->pcm_buffer,
                                  ctx->config.max_frame_samples,
                                  0);
    if (decoded_samples < 0) {
        return decoded_samples;
    }
    
    printf("音频解码成功\n");

    /*
     * 解码层不负责“播放”。
     * 真正怎么消费 PCM，由上层注册的 pcm_cb 决定。
     */
    if (ctx->pcm_cb != NULL) {
        ctx->pcm_cb((const uint8_t *)ctx->pcm_buffer,
                    (size_t)decoded_samples,
                    (size_t)ctx->config.channels * sizeof(opus_int16),
                    ctx->pcm_cb_user_data);
    }

    return decoded_samples;
}

/*
 * 关闭解码模块并释放资源。
 */
void audio_opus_decoder_close(opus_decoder_ctx_t *ctx) {
    if (ctx == NULL) {
        return;
    }

    if (ctx->decoder != NULL) {
        opus_decoder_destroy(ctx->decoder);
    }

    free(ctx->pcm_buffer);
    audio_opus_decoder_reset(ctx);
}
