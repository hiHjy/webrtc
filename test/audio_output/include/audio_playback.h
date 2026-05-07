#ifndef AUDIO_PLAYBACK_H
#define AUDIO_PLAYBACK_H

/*
 * 让 C 头文件在 C++ 编译器下仍然保持 C ABI。
 */
#ifdef __cplusplus
extern "C" {
#endif

/*
 * 某些系统头会根据 _POSIX_C_SOURCE 决定是否暴露 POSIX 接口。
 * 这里和 input 模块保持一致，避免不同源文件可见接口不一致。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <alsa/asoundlib.h>
#include <stddef.h>
#include <stdint.h>

/*
 * 纯播放模块的运行时上下文。
 *
 * 这一层只关心 ALSA playback 本身：
 * - PCM 设备句柄
 * - 最终生效的播放参数
 * - 一帧 PCM 的字节数
 *
 * 它不关心：
 * - Opus 解码
 * - 网络收包
 * - 队列和线程调度
 *
 * 这样 audio_playback 就能保持为一个“纯输出模块”。
 */
typedef struct audio_playback_ctx {
    /* ALSA PCM playback 句柄。 */
    snd_pcm_t *pcm_handle;

    /* 最终生效的播放参数。 */
    snd_pcm_format_t format;
    unsigned int sample_rate;
    unsigned int channels;
    snd_pcm_uframes_t period_size;

    /*
     * 一帧 PCM 的字节数。
     * 例如 S16_LE 单声道为 2，双声道为 4。
     */
    size_t frame_bytes;
} audio_playback_ctx_t;

/*
 * 初始化 ALSA 播放模块。
 *
 * device/sample_rate/channels/format/period_size 都是“期望值”。
 * 因为采用 near 类接口，驱动最终接受的值可能和期望值略有不同，
 * 所以初始化完成后，以 ctx 中记录的实际值为准。
 */
int audio_playback_init(audio_playback_ctx_t *ctx,
                        const char *device,
                        unsigned int sample_rate,
                        unsigned int channels,
                        snd_pcm_format_t format,
                        snd_pcm_uframes_t period_size);

/*
 * 把一段 PCM 数据写到声卡播放。
 *
 * data:
 *   指向交错排列的原始 PCM 数据。
 *
 * frames:
 *   本次要写入的帧数。
 *
 * 这个接口只负责“写播放设备”，并不关心这段 PCM 是怎么来的。
 */
int audio_playback_write(audio_playback_ctx_t *ctx,
                         const uint8_t *data,
                         snd_pcm_uframes_t frames);

/*
 * 关闭播放模块并释放资源。
 */
void audio_playback_close(audio_playback_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif
