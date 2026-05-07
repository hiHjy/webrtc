#include "audio_playback.h"

#include <alloca.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/*
 * 把播放上下文恢复到“空状态”。
 *
 * 初始化前和关闭后都会用到它。
 */
static void audio_playback_reset(audio_playback_ctx_t *ctx) {
    if (ctx == NULL) {
        return;
    }

    memset(ctx, 0, sizeof(*ctx));
}

/*
 * 初始化 ALSA 播放模块。
 *
 * 这个函数会完成整个播放设备准备流程：
 * - 打开 PCM playback 设备
 * - 配 access / format / rate / channels / period
 * - prepare 设备
 *
 * 初始化完成后，ctx 里保存的是驱动最终接受的实际参数。
 */
int audio_playback_init(audio_playback_ctx_t *ctx,
                        const char *device,
                        unsigned int sample_rate,
                        unsigned int channels,
                        snd_pcm_format_t format,
                        snd_pcm_uframes_t period_size) {
    snd_pcm_hw_params_t *hw_params = NULL;
    int err = 0;
    int dir = 0;
    const char *pcm_device = device != NULL ? device : "hw:0,0";

    if (ctx == NULL) {
        return -EINVAL;
    }

    /* 每次初始化都从一个干净上下文开始。 */
    audio_playback_reset(ctx);

    /* 给上层没填或填 0 的参数补一组可工作的默认值。 */
    if (sample_rate == 0) {
        sample_rate = 48000;
    }
    if (channels == 0) {
        channels = 1;
    }
    if (format == SND_PCM_FORMAT_UNKNOWN) {
        format = SND_PCM_FORMAT_S16_LE;
    }
    if (period_size == 0) {
        period_size = 960;
    }

    /* 打开 ALSA PCM playback 设备。 */
    err = snd_pcm_open(&ctx->pcm_handle, pcm_device, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        fprintf(stderr, "snd_pcm_open(%s) failed: %s\n", pcm_device, snd_strerror(err));
        audio_playback_close(ctx);
        return err;
    }

    /* 在栈上分配硬件参数对象。 */
    snd_pcm_hw_params_alloca(&hw_params);

    /* 读取设备支持能力范围，后面所有 set 都基于这份对象修改。 */
    err = snd_pcm_hw_params_any(ctx->pcm_handle, hw_params);
    if (err < 0) {
        fprintf(stderr, "snd_pcm_hw_params_any failed: %s\n", snd_strerror(err));
        audio_playback_close(ctx);
        return err;
    }

    /*
     * 交错模式：
     * 多声道时，一个 frame 中按声道顺序排列。
     */
    err = snd_pcm_hw_params_set_access(ctx->pcm_handle,
                                       hw_params,
                                       SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0) {
        fprintf(stderr, "snd_pcm_hw_params_set_access failed: %s\n", snd_strerror(err));
        audio_playback_close(ctx);
        return err;
    }

    /* 设置播放 PCM 格式，例如 S16_LE。 */
    err = snd_pcm_hw_params_set_format(ctx->pcm_handle, hw_params, format);
    if (err < 0) {
        fprintf(stderr, "snd_pcm_hw_params_set_format failed: %s\n", snd_strerror(err));
        audio_playback_close(ctx);
        return err;
    }

    /*
     * 用 near 接口设置采样率。
     * 如果设备不支持精确值，就选一个最接近的。
     */
    err = snd_pcm_hw_params_set_rate_near(ctx->pcm_handle, hw_params, &sample_rate, NULL);
    if (err < 0) {
        fprintf(stderr, "snd_pcm_hw_params_set_rate_near failed: %s\n", snd_strerror(err));
        audio_playback_close(ctx);
        return err;
    }

    /* 设置声道数。 */
    err = snd_pcm_hw_params_set_channels(ctx->pcm_handle, hw_params, channels);
    if (err < 0) {
        fprintf(stderr, "snd_pcm_hw_params_set_channels failed: %s\n", snd_strerror(err));
        audio_playback_close(ctx);
        return err;
    }

    /*
     * 设置 period 大小。
     * 这里的 period 可以理解成“声卡每次较自然地吞下一包数据的帧数”。
     */
    err = snd_pcm_hw_params_set_period_size_near(ctx->pcm_handle,
                                                 hw_params,
                                                 &period_size,
                                                 &dir);
    if (err < 0) {
        fprintf(stderr, "snd_pcm_hw_params_set_period_size_near failed: %s\n",
                snd_strerror(err));
        audio_playback_close(ctx);
        return err;
    }

    /* 把这组参数真正提交给驱动。 */
    err = snd_pcm_hw_params(ctx->pcm_handle, hw_params);
    if (err < 0) {
        fprintf(stderr, "snd_pcm_hw_params failed: %s\n", snd_strerror(err));
        audio_playback_close(ctx);
        return err;
    }

    /*
     * 把驱动实际接受的 period_size 再读回来。
     * near 接口可能会把你申请的值调整成设备支持的最近值。
     */
    err = snd_pcm_hw_params_get_period_size(hw_params, &period_size, &dir);
    if (err < 0) {
        fprintf(stderr, "snd_pcm_hw_params_get_period_size failed: %s\n",
                snd_strerror(err));
        audio_playback_close(ctx);
        return err;
    }

    /* 让设备进入 prepared 状态，后续就可以 write 了。 */
    err = snd_pcm_prepare(ctx->pcm_handle);
    if (err < 0) {
        fprintf(stderr, "snd_pcm_prepare failed: %s\n", snd_strerror(err));
        audio_playback_close(ctx);
        return err;
    }

    /* 记录最终生效的实际参数。 */
    ctx->format = format;
    ctx->sample_rate = sample_rate;
    ctx->channels = channels;
    ctx->period_size = period_size;

    /*
     * 一帧 PCM 的字节数 = 每声道样本字节数 * 声道数。
     */
    ctx->frame_bytes = (size_t)((snd_pcm_format_physical_width(format) / 8) * channels);

    return 0;
}

/*
 * 把一段 PCM 数据写到声卡。
 *
 * 这个函数会持续写，直到本次 frames 全部写完为止。
 * 如果中间遇到 XRUN 或其他可恢复错误，会尝试恢复后继续写。
 */
int audio_playback_write(audio_playback_ctx_t *ctx,
                         const uint8_t *data,
                         snd_pcm_uframes_t frames) {
    const uint8_t *cursor = data;

    if (ctx == NULL || ctx->pcm_handle == NULL || data == NULL || frames == 0) {
        return -EINVAL;
    }

    while (frames > 0) {
        /* 把当前这一段 PCM 写到 ALSA。 */
        snd_pcm_sframes_t written = snd_pcm_writei(ctx->pcm_handle, cursor, frames);
        if (written == -EPIPE) {
            /*
             * XRUN：通常表示应用供数不及时，或者设备状态被打断。
             * playback 场景里 prepare 后通常还能继续写。
             */
            snd_pcm_prepare(ctx->pcm_handle);
            continue;
        }

        if (written < 0) {
            /* 其他错误交给 ALSA 通用恢复接口。 */
            written = snd_pcm_recover(ctx->pcm_handle, (int)written, 0);
            if (written < 0) {
                fprintf(stderr, "snd_pcm_writei/snd_pcm_recover failed: %s\n",
                        snd_strerror((int)written));
                return (int)written;
            }
            continue;
        }

        /* 推进写指针，并减少剩余待写帧数。 */
        cursor += (size_t)written * ctx->frame_bytes;
        frames -= (snd_pcm_uframes_t)written;
    }

    return 0;
}

/*
 * 关闭播放模块并释放资源。
 */
void audio_playback_close(audio_playback_ctx_t *ctx) {
    if (ctx == NULL) {
        return;
    }

    if (ctx->pcm_handle != NULL) {
        /* drain 会尽量把设备里还没播完的数据播掉。 */
        snd_pcm_drain(ctx->pcm_handle);
        snd_pcm_close(ctx->pcm_handle);
        ctx->pcm_handle = NULL;
    }

    audio_playback_reset(ctx);
}
