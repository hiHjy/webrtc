#include "opus_encoder.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/*
 * 把 Opus 编码上下文恢复到“空状态”。
 * 关闭时和初始化前都会用到它。
 */
static void opus_encoder_reset(opus_encoder_ctx_t *ctx) {
    if (ctx == NULL) {
        return;
    }

    memset(ctx, 0, sizeof(*ctx));
}

/*
 * 当内部已经攒满一整个 Opus 帧时，真正执行一次编码。
 *
 * 这个函数是模块内部用的“热路径”：
 * - 从 pcm_cache 读满帧 PCM
 * - 调用 opus_encode()
 * - 编码成功后，通过 packet_cb 往上抛出一个 Opus 包
 */
static int opus_encoder_encode_ready_frame(opus_encoder_ctx_t *ctx) {
    int packet_len = 0;

    if (ctx == NULL || ctx->encoder == NULL || ctx->pcm_cache == NULL ||
        ctx->packet_buffer == NULL) {
        return -EINVAL;
    }

    packet_len = opus_encode(ctx->encoder,
                             ctx->pcm_cache,
                             ctx->config.frame_samples,
                             ctx->packet_buffer,
                             ctx->config.max_packet_size);
    if (packet_len < 0) {
        return packet_len;
    }

    /*
     * 编码层不负责“发送”，只负责“把包交出去”。
     * 真正谁来消费这个包，由上层注册回调决定。
     */
    if (ctx->packet_cb != NULL) {
        return ctx->packet_cb(ctx->packet_buffer,
                              (size_t)packet_len,
                              ctx->packet_cb_user_data);
    }

    return 0;
}

/*
 * 默认 Opus 配置。
 *
 * 这里的默认值更偏语音链路：
 * - 48kHz
 * - 单声道
 * - VOIP 模式
 * - 20ms 帧长（960 samples）
 */
void audio_opus_encoder_config_init(opus_encoder_config_t *config) {
    if (config == NULL) {
        return;
    }

    memset(config, 0, sizeof(*config));
    config->sample_rate = 48000;
    config->channels = 1;
    config->application = OPUS_APPLICATION_VOIP;
    config->bitrate = 32000;
    config->frame_samples = 960;
    config->max_packet_size = 1500;
}

/*
 * 注册编码完成回调。
 *
 * 上层一般会把 manager 的回调挂到这里。
 */
void audio_opus_encoder_set_packet_callback(opus_encoder_ctx_t *ctx,
                                            opus_packet_callback_t cb,
                                            void *user_data) {
    if (ctx == NULL) {
        return;
    }

    ctx->packet_cb = cb;
    ctx->packet_cb_user_data = user_data;
}

/*
 * 初始化 Opus 模块。
 *
 * 主要做四件事：
 * 1. 关闭旧实例，避免重复 init 泄漏资源
 * 2. 创建 libopus 编码器
 * 3. 分配 PCM 攒帧缓存
 * 4. 分配 Opus 包输出缓存
 */
int audio_opus_encoder_init(opus_encoder_ctx_t *ctx,
                            const opus_encoder_config_t *config) {
    OpusEncoder *encoder = NULL;
    opus_encoder_config_t effective_config;
    int err = 0;

    if (ctx == NULL) {
        return -EINVAL;
    }

    audio_opus_encoder_close(ctx);
    audio_opus_encoder_config_init(&effective_config);
    if (config != NULL) {
        effective_config = *config;
    }

    if (effective_config.sample_rate <= 0 || effective_config.channels <= 0 ||
        effective_config.frame_samples <= 0 || effective_config.max_packet_size <= 0) {
        return -EINVAL;
    }

    encoder = opus_encoder_create(effective_config.sample_rate,
                                  effective_config.channels,
                                  effective_config.application,
                                  &err);
    if (err != OPUS_OK) {
        return -EINVAL;
    }

    if (effective_config.bitrate > 0) {
        err = opus_encoder_ctl(encoder, OPUS_SET_BITRATE(effective_config.bitrate));
        if (err != OPUS_OK) {
            opus_encoder_destroy(encoder);
            return -EINVAL;
        }
    }

    /*
     * pcm_cache 大小 = 一帧 Opus PCM 的长度。
     * 单位是 opus_int16，不是字节。
     */
    ctx->pcm_cache = (opus_int16 *)calloc((size_t)effective_config.frame_samples *
                                              (size_t)effective_config.channels,
                                          sizeof(opus_int16));
    if (ctx->pcm_cache == NULL) {
        opus_encoder_destroy(encoder);
        return -ENOMEM;
    }

    /* 单个 Opus 包输出缓冲区。 */
    ctx->packet_buffer =
        (unsigned char *)calloc((size_t)effective_config.max_packet_size, sizeof(unsigned char));
    if (ctx->packet_buffer == NULL) {
        free(ctx->pcm_cache);
        ctx->pcm_cache = NULL;
        opus_encoder_destroy(encoder);
        return -ENOMEM;
    }

    ctx->encoder = encoder;
    ctx->config = effective_config;
    ctx->cached_frames = 0;
    return 0;
}

/*
 * 喂一段 PCM 给 Opus 模块。
 *
 * 这是这个模块最核心的入口。
 *
 * 它的工作方式是：
 * 1. 先检查输入格式是否符合当前编码器配置
 * 2. 把输入 PCM 复制到内部缓存
 * 3. 一旦攒够一整帧，就立即编码
 *
 * 所以这个接口适合直接接采集层抛上来的“任意大小 PCM 块”。
 */
int audio_opus_encoder_push_pcm(opus_encoder_ctx_t *ctx,
                                const uint8_t *data,
                                size_t frames,
                                size_t frame_bytes) {
    const opus_int16 *samples = (const opus_int16 *)data;
    size_t channels = 0;

    if (ctx == NULL || ctx->encoder == NULL || data == NULL) {
        return -EINVAL;
    }

    channels = (size_t)ctx->config.channels;

    /*
     * 当前实现默认输入就是 S16_LE PCM。
     * 所以一帧字节数必须等于 channels * sizeof(opus_int16)。
     */
    if (frame_bytes != channels * sizeof(opus_int16)) {
        return -EINVAL;
    }
    int ret = 0;
    while (frames > 0) {
        size_t free_frames = (size_t)(ctx->config.frame_samples - ctx->cached_frames);
        size_t copy_frames = frames < free_frames ? frames : free_frames;
       

        /*
         * 把当前这段 PCM 复制进内部缓存。
         * 偏移位置取决于前面已经攒了多少帧。
         */
        if (copy_frames > 0) {
            memcpy(ctx->pcm_cache + ((size_t)ctx->cached_frames * channels),
                samples,
                copy_frames * channels * sizeof(opus_int16));

            ctx->cached_frames += (int)copy_frames;
            samples += copy_frames * channels;
            frames -= copy_frames;

            /* 还没攒满一个完整 Opus 帧，就继续等后续 PCM。 */
            if (ctx->cached_frames < ctx->config.frame_samples) {
                continue;
            }
        }
        /* 攒满一帧后，立刻编码并上抛一个 Opus 包。 */
        ret = opus_encoder_encode_ready_frame(ctx);
        if (ret < 0) {
            return ret;
        }

        /* 一帧编码完成后，缓存重新从 0 开始攒。 */
        ctx->cached_frames = 0;
    }

    return 0;
}

/*
 * 把尾部不足一帧的 PCM 补零后再编码一次。
 *
 * 这个接口通常在 close 前调，
 * 作用是尽量把最后那一点残余语音也编码出去，别直接丢掉。
 */
int audio_opus_encoder_flush(opus_encoder_ctx_t *ctx) {
    size_t tail_frames = 0;

    if (ctx == NULL || ctx->encoder == NULL) {
        return 0;
    }

    if (ctx->cached_frames == 0) {
        return 0;
    }

    /*
     * 把剩余没凑满的那一段补 0，拼成一帧完整 PCM。
     * 这在语音流尾部是很常见的做法。
     */
    tail_frames = (size_t)(ctx->config.frame_samples - ctx->cached_frames);
    memset(ctx->pcm_cache + ((size_t)ctx->cached_frames * (size_t)ctx->config.channels),
           0,
           tail_frames * (size_t)ctx->config.channels * sizeof(opus_int16));

    ctx->cached_frames = ctx->config.frame_samples;
    if (opus_encoder_encode_ready_frame(ctx) < 0) {
        return -EINVAL;
    }

    ctx->cached_frames = 0;
    return 0;
}

/*
 * 关闭编码模块并释放资源。
 */
void audio_opus_encoder_close(opus_encoder_ctx_t *ctx) {
    if (ctx == NULL) {
        return;
    }

    if (ctx->encoder != NULL) {
        opus_encoder_destroy(ctx->encoder);
    }

    free(ctx->pcm_cache);
    free(ctx->packet_buffer);
    opus_encoder_reset(ctx);
}
