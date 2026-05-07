#ifndef AUDIO_PLAYBACK_MANAGER_H
#define AUDIO_PLAYBACK_MANAGER_H

/*
 * 让 C 头文件在 C++ 编译器下仍然保持 C ABI。
 */
#ifdef __cplusplus
extern "C" {
#endif

#include "audio_playback.h"
#include "opus_decoder.h"

#include <pthread.h>
#include <stdint.h>

/*
 * playback_manager 的总配置。
 *
 * 它同时管理：
 * - ALSA 播放参数
 * - Opus 解码参数
 * - 队列容量参数
 *
 * 对业务层来说，希望一次配置整条“下行播放链路”，
 * 所以这些参数都收敛在这里。
 */
typedef struct audio_playback_manager_config {
    /* ALSA 播放参数。 */
    char device[64];
    unsigned int sample_rate;
    unsigned int channels;
    snd_pcm_format_t format;
    snd_pcm_uframes_t period_size;

    /* Opus 解码参数。 */
    int decoder_max_frame_samples;

    /* 队列参数。 */
    size_t max_queue_packets;
    size_t max_packet_size;
} audio_playback_manager_config_t;

/*
 * playback_manager 是整条下行链路的总控层。
 *
 * 它内部同时持有：
 * - 一个 Opus 解码模块
 * - 一个 ALSA 播放模块
 * - 一条后台播放线程
 * - 一个固定容量的环形包队列
 *
 * 对上层来说，最核心的接口就是：
 * - init/start
 * - push_packet
 * - stop/close
 *
 * 其中 push_packet() 很适合直接给 libdatachannel 的 onFrame 调用。
 */
typedef struct audio_playback_manager {
    /* 纯播放层上下文。 */
    audio_playback_ctx_t playback;

    /* 纯 Opus 解码层上下文。 */
    opus_decoder_ctx_t decoder;

    /* 当前 manager 的工作配置。 */
    audio_playback_manager_config_t config;

    /* 后台播放线程及同步原语。 */
    pthread_t playback_thread;
    pthread_mutex_t queue_mutex;
    pthread_cond_t queue_cond;

    /*
     * 固定容量环形队列。
     *
     * queue_buffer:
     *   真正存放 Opus 包数据的连续内存。
     *
     * queue_packet_sizes:
     *   记录每个槽位当前包的实际长度。
     *
     * queue_packet_pts_us:
     *   记录每个槽位对应 Opus 包的播放时间戳，单位微秒。
     *
     * dequeue_buffer:
     *   播放线程从队列取包后，先复制到这块独立缓冲区，
     *   再送去解码，避免边解码边持有队列锁。
     */
    uint8_t *queue_buffer;
    size_t *queue_packet_sizes;
    int64_t *queue_packet_pts_us;
    uint8_t *dequeue_buffer;
    int64_t dequeue_pts_us;

    /* 当前读写位置和队列内包数量。 */
    size_t queue_read_index;
    size_t queue_write_index;
    size_t queued_packets;

    /*
     * 基于 ALSA delay 修正后的真实音频播放时钟。
     *
     * audio_clock_us:
     *   估算“声卡已经播放到的音频 PTS”，单位微秒。
     *
     * audio_clock_valid:
     *   至少成功写过一次 PCM 后才为 1。
     */
    int64_t audio_clock_us;
    uint8_t audio_clock_valid;

    /* 生命周期状态位。 */
    uint8_t is_initialized;
    uint8_t is_running;
    uint8_t request_stop;
} audio_playback_manager_t;

/*
 * 给 playback_manager 配一组默认参数。
 */
void audio_playback_manager_config_init(audio_playback_manager_config_t *config);

/*
 * 初始化整条下行链路。
 *
 * 当前会做：
 * 1. 保存配置
 * 2. 初始化队列和同步原语
 * 3. 初始化 ALSA 播放模块
 * 4. 初始化 Opus 解码模块
 * 5. 把“解码后的 PCM 回调”挂到 manager
 */
int audio_playback_manager_init(audio_playback_manager_t *manager,
                                const audio_playback_manager_config_t *config);

/*
 * 启动后台播放线程。
 *
 * 启动后线程会持续：
 * - 等待队列里有 Opus 包
 * - 取包
 * - 交给解码器
 * - 再把 PCM 写到声卡
 */
int audio_playback_manager_start(audio_playback_manager_t *manager);

/*
 * 往 manager 里推一个 Opus 包。
 *
 * 这个接口本身不做解码和播放，
 * 只负责把包复制进环形队列。
 *
 * 所以它很适合在 libdatachannel 的 onFrame 回调里直接调用。
 */
int audio_playback_manager_push_packet(audio_playback_manager_t *manager,
                                       const uint8_t *packet,
                                       size_t packet_len);

int audio_playback_manager_push_packet_with_pts(audio_playback_manager_t *manager,
                                                const uint8_t *packet,
                                                size_t packet_len,
                                                int64_t pts_us);

/*
 * 读取当前音频真实播放时钟，单位微秒。
 *
 * 返回 0 表示成功；返回 -EAGAIN 表示还没有足够 PCM 写入声卡，
 * 当前 clock 暂时不可用。
 */
int audio_playback_manager_get_clock_us(audio_playback_manager_t *manager,
                                        int64_t *clock_us);

/*
 * 停止后台播放线程。
 */
void audio_playback_manager_stop(audio_playback_manager_t *manager);

/*
 * 关闭整条下行链路并释放资源。
 */
void audio_playback_manager_close(audio_playback_manager_t *manager);

#ifdef __cplusplus
}
#endif

#endif
