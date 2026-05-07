#include "audio_playback_manager.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 把环形队列逻辑状态清空。
 *
 * 注意：
 * 这里不释放底层缓冲区，只是把“当前队列为空”这个状态重置好。
 */
static void audio_playback_manager_free_queue(audio_playback_manager_t *manager) {
    if (manager == NULL) {
        return;
    }

    if (manager->queue_packet_sizes != NULL && manager->config.max_queue_packets > 0) {
        memset(manager->queue_packet_sizes, 0, manager->config.max_queue_packets * sizeof(size_t));
    }
    if (manager->queue_packet_pts_us != NULL && manager->config.max_queue_packets > 0) {
        memset(manager->queue_packet_pts_us, 0,
               manager->config.max_queue_packets * sizeof(int64_t));
    }

    manager->queue_read_index = 0;
    manager->queue_write_index = 0;
    manager->queued_packets = 0;
    manager->dequeue_pts_us = 0;
    manager->audio_clock_us = 0;
    manager->audio_clock_valid = 0;
}

/*
 * 释放 manager 持有的队列相关缓冲区。
 *
 * 当前队列采用固定容量环形数组，所以这里会统一释放：
 * - queue_buffer
 * - queue_packet_sizes
 * - dequeue_buffer
 */
static void audio_playback_manager_release_buffers(audio_playback_manager_t *manager) {
    if (manager == NULL) {
        return;
    }

    free(manager->queue_buffer);
    free(manager->queue_packet_sizes);
    free(manager->queue_packet_pts_us);
    free(manager->dequeue_buffer);

    manager->queue_buffer = NULL;
    manager->queue_packet_sizes = NULL;
    manager->queue_packet_pts_us = NULL;
    manager->dequeue_buffer = NULL;
}

/*
 * 把 manager 恢复到“空状态”。
 */
static void audio_playback_manager_reset(audio_playback_manager_t *manager) {
    if (manager == NULL) {
        return;
    }

    memset(manager, 0, sizeof(*manager));
}

static void audio_playback_manager_update_clock(audio_playback_manager_t *manager,
                                                size_t frames) {
    snd_pcm_sframes_t delay_frames = 0;
    int64_t delay_us = 0;
    int64_t block_end_pts_us = 0;
    int64_t clock_us = 0;

    if (manager == NULL || manager->playback.pcm_handle == NULL ||
        manager->playback.sample_rate == 0 || frames == 0) {
        return;
    }

    if (snd_pcm_delay(manager->playback.pcm_handle, &delay_frames) < 0) {
        delay_frames = 0;
    }
    if (delay_frames < 0) {
        delay_frames = 0;
    }

    block_end_pts_us =
        manager->dequeue_pts_us +
        (int64_t)frames * 1000000LL / (int64_t)manager->playback.sample_rate;
    delay_us = (int64_t)delay_frames * 1000000LL /
               (int64_t)manager->playback.sample_rate;
    clock_us = block_end_pts_us - delay_us;
    if (clock_us < 0) {
        clock_us = 0;
    }

    pthread_mutex_lock(&manager->queue_mutex);
    manager->audio_clock_us = clock_us;
    manager->audio_clock_valid = 1;
    pthread_mutex_unlock(&manager->queue_mutex);
}

/*
 * Opus 解码出 PCM 后，最终会先回调到 manager 这一层。
 *
 * manager 在这里做的事情很简单：
 * - 检查 PCM 格式是否和 playback 层一致
 * - 直接把 PCM 交给 audio_playback_write()
 *
 * 也就是说：
 * - Opus 的“入口”在 push_packet
 * - PCM 的“出口”在 playback_write
 */
static void audio_playback_manager_on_pcm(const uint8_t *pcm,
                                          size_t frames,
                                          size_t frame_bytes,
                                          void *user_data) {
    audio_playback_manager_t *manager = (audio_playback_manager_t *)user_data;
    size_t expected_frame_bytes = 0;

    if (manager == NULL || pcm == NULL || frames == 0) {
        return;
    }

    expected_frame_bytes = manager->playback.frame_bytes;
    if (frame_bytes != expected_frame_bytes) {
        fprintf(stderr,
                "audio_playback_manager_on_pcm: frame_bytes mismatch, expected=%zu got=%zu\n",
                expected_frame_bytes,
                frame_bytes);
        return;
    }

    /* 把解码后的 PCM 直接写到声卡。 */
    if (audio_playback_write(&manager->playback, pcm, (snd_pcm_uframes_t)frames) < 0) {
        fprintf(stderr, "audio_playback_write failed\n");
        return;
    }

    audio_playback_manager_update_clock(manager, frames);
}

/*
 * 后台播放线程主循环。
 *
 * 线程职责：
 * 1. 等待队列里出现新的 Opus 包
 * 2. 从环形队列取出一个包
 * 3. 把包交给 Opus 解码器
 * 4. 解码器再通过回调把 PCM 交给 playback 层
 *
 * 数据链路长这样：
 *
 *   libdatachannel onFrame
 *        |
 *        v
 *   audio_playback_manager_push_packet()
 *        |
 *        v
 *   ring queue
 *        |
 *        v
 *   audio_playback_manager_thread_main()
 *        |
 *        v
 *   audio_opus_decoder_push_packet()
 *        |
 *        v
 *   audio_playback_manager_on_pcm()
 *        |
 *        v
 *   audio_playback_write()
 */
static void *audio_playback_manager_thread_main(void *arg) {
    /*
     * arg:
     *   pthread_create() 启线程时传进来的启动参数。
     *
     * 这里实际上传进来的是 audio_playback_manager_t *，
     * 所以先把 void * 转回真实类型。
     *
     * manager 可以理解成整条“下行播放链路”的总控对象，
     * 里面同时放着：
     * - 环形队列
     * - Opus 解码器
     * - ALSA 播放器
     * - 锁和条件变量
     */
    audio_playback_manager_t *manager = (audio_playback_manager_t *)arg;

    /*
     * 这是后台播放线程的主循环。
     *
     * 线程会反复做四件事：
     * 1. 没包时睡眠等待
     * 2. 有包时从环形队列取出一个 Opus 包
     * 3. 把这个包送进 Opus 解码器
     * 4. 解码出的 PCM 再通过回调写给 ALSA 播放
     *
     * 它本质上是“队列消费者线程”。
     */
    while (manager != NULL) {
        /*
         * packet_len:
         *   当前这一轮从队列里取出来的 Opus 包长度，单位是字节。
         *
         * 举例：
         *   如果当前包是 38 字节，
         *   那这一轮里 packet_len 最后就会等于 38。
         */
        size_t packet_len = 0;
        int64_t packet_pts_us = 0;

        /*
         * slot:
         *   指向“当前读槽位”的起始地址。
         *
         * 当前队列底层不是链表，而是一整块大数组 queue_buffer。
         * 每个槽位固定占 max_packet_size 字节。
         *
         * 举例：
         *   假设：
         *   - max_packet_size = 1500
         *   - queue_read_index = 3
         *
         *   那么当前要读的槽位起始地址就是：
         *   queue_buffer + 3 * 1500
         */
        uint8_t *slot = NULL;

        /*
         * queue_mutex:
         *   队列锁。因为生产者线程和当前消费者线程都会访问队列，
         *   所以操作队列状态前必须先加锁。
         *
         * 这里的生产者通常是：
         *   libdatachannel onFrame -> audio_playback_manager_push_packet()
         *
         * 当前线程就是消费者：
         *   audio_playback_manager_thread_main()
         */
        pthread_mutex_lock(&manager->queue_mutex);

        /*
         * queued_packets:
         *   当前队列里一共有多少个还没处理的 Opus 包。
         *
         * request_stop:
         *   外部是否请求当前线程退出。
         *
         * 这里这段 while 的意思是：
         * - 还没有收到 stop 请求
         * - 并且队列里一个包都没有
         *
         * 这种情况下，线程就没有工作可做，所以阻塞等待。
         *
         * 为什么这里要写 while，而不是 if？
         * 因为条件变量可能出现“假唤醒”，
         * 所以每次醒来后都要重新检查条件是否真的满足。
         */
        while (!manager->request_stop && manager->queued_packets == 0) {
            /*
             * pthread_cond_wait() 会做两件事：
             * 1. 让当前线程进入睡眠，等待 queue_cond 被 signal/broadcast
             * 2. 睡眠期间临时释放 queue_mutex
             *
             * 等线程被唤醒后，它会先重新拿回 queue_mutex，
             * 然后再从这里继续往下执行。
             *
             * 这样做的好处：
             * - 没包时不会空转占 CPU
             * - 同时也不会一直霸占队列锁
             */
            pthread_cond_wait(&manager->queue_cond, &manager->queue_mutex);
        }

        /*
         * 如果外部已经请求 stop，
         * 并且当前队列也空了，
         * 说明没有剩余包需要处理，线程就可以退出。
         *
         * 注意这里不是“一收到 stop 就立刻退出”，而是：
         * - stop 后，尽量把队列里还剩的包消费完
         * - 等队列空了再退出
         *
         * 这是比较平滑的退出方式。
         */
        if (manager->request_stop && manager->queued_packets == 0) {
            pthread_mutex_unlock(&manager->queue_mutex);
            break;
        }

        /*
         * queue_read_index:
         *   当前“下一个该读哪个槽位”的索引。
         *
         * queue_packet_sizes:
         *   数组里记录每个槽位当前包的真实长度。
         *   例如 queue_packet_sizes[5] = 40，
         *   表示第 5 号槽位当前装着一个 40 字节的 Opus 包。
         *
         * queue_buffer:
         *   真正存放 Opus 包内容的大数组。
         *
         * dequeue_buffer:
         *   播放线程自己私有的一块临时缓冲区。
         *
         * 下面这几步做的事情是：
         * 1. 读出当前槽位这个包的真实长度
         * 2. 算出这个槽位在大数组中的起始地址
         * 3. 把这个包复制到 dequeue_buffer
         *
         * 为什么要先复制到 dequeue_buffer？
         * 因为 dequeue_buffer 是当前播放线程独占的。
         * 复制完以后就可以马上解锁，
         * 后面的解码过程就不再依赖共享队列内存。
         *
         * 例子：
         *   假设：
         *   - queue_read_index = 5
         *   - max_packet_size = 1500
         *   - queue_packet_sizes[5] = 40
         *
         *   那就表示：
         *   - 当前要处理的是第 5 号槽位
         *   - 这个包长度是 40 字节
         *   - 它在 queue_buffer 中的起始地址是：
         *     queue_buffer + 5 * 1500
         */
        packet_len = manager->queue_packet_sizes[manager->queue_read_index];
        packet_pts_us = manager->queue_packet_pts_us[manager->queue_read_index];
        slot = manager->queue_buffer +
               (manager->queue_read_index * manager->config.max_packet_size);
        memcpy(manager->dequeue_buffer, slot, packet_len);
        manager->dequeue_pts_us = packet_pts_us;

        /*
         * 当前槽位的数据已经被复制到 dequeue_buffer，
         * 所以把这个槽位的长度清零，表示这个槽位现在空出来了。
         */
        manager->queue_packet_sizes[manager->queue_read_index] = 0;
        manager->queue_packet_pts_us[manager->queue_read_index] = 0;
        printf("当前队列长度:%lu\n", manager->queued_packets);
        /*
         * 读指针向后移动一格。
         *
         * 这里是典型的环形数组写法：
         *   (当前索引 + 1) % 队列容量
         *
         * 举例：
         *   如果 max_queue_packets = 128，
         *   当前 queue_read_index = 127，
         *   那下一次就会回到：
         *   (127 + 1) % 128 = 0
         *
         * 这就是“环形队列”的意思。
         */
        manager->queue_read_index =
            (manager->queue_read_index + 1) % manager->config.max_queue_packets;

        /*
         * 队列中待处理包数量减 1。
         * 因为这一轮已经成功取出了一个包。
         */
        manager->queued_packets--;

        /*
         * 到这里为止，和队列有关的动作已经全部做完了：
         * - 包取出来了
         * - 槽位标为空了
         * - 读指针前进了
         * - 包数量更新了
         *
         * 所以现在要尽快解锁，
         * 让生产者线程可以继续往队列里塞新包。
         */
        pthread_mutex_unlock(&manager->queue_mutex);

        /*
         * 把当前 Opus 包送入解码器。
         *
         * 注意这里传给解码器的是 dequeue_buffer，
         * 不是前面的 slot。
         *
         * 这样后面的解码过程就完全不依赖共享队列内存，
         * 也不会在解码期间继续占着队列锁。
         *
         * audio_opus_decoder_push_packet() 内部还会继续做：
         * - opus_decode()
         * - 得到 PCM
         * - 通过回调进入 audio_playback_manager_on_pcm()
         * - 再由 audio_playback_write() 写到声卡
         */
        if (audio_opus_decoder_push_packet(&manager->decoder,
                                           manager->dequeue_buffer,
                                           packet_len) < 0) {
            fprintf(stderr, "audio_opus_decoder_push_packet failed, packet_len=%zu\n", packet_len);
        }
    }

    /*
     * 线程退出前，把 is_running 清成 0，
     * 这样外部就知道当前后台播放线程已经结束了。
     */
    if (manager != NULL) {
        manager->is_running = 0;
    }

    return NULL;
}

/*
 * 给 playback_manager 配一组默认参数。
 *
 * 默认目标：
 * - 48kHz
 * - 单声道
 * - S16_LE
 * - 20ms 粒度
 * - 队列最多缓存 128 个 Opus 包
 */
void audio_playback_manager_config_init(audio_playback_manager_config_t *config) {
    if (config == NULL) {
        return;
    }

    memset(config, 0, sizeof(*config));
    snprintf(config->device, sizeof(config->device), "%s", "hw:0,0");
    config->sample_rate = 48000;
    config->channels = 1;
    config->format = SND_PCM_FORMAT_S16_LE;
    config->period_size = 960;
    config->decoder_max_frame_samples = 5760;
    config->max_queue_packets = 128;
    config->max_packet_size = 1500;
}

/*
 * 初始化整条下行链路。
 *
 * 当前会按顺序做这些事情：
 * 1. 清理并保存配置
 * 2. 初始化队列锁和条件变量
 * 3. 分配固定容量环形队列
 * 4. 初始化 ALSA 播放模块
 * 5. 初始化 Opus 解码模块
 * 6. 把“解码完成后的 PCM 回调”挂回 manager
 *
 * 注意：
 * 这里只做“准备工作”，并不会立刻启动后台播放线程。
 * 真正开始消费队列，要调用 audio_playback_manager_start()。
 */
int audio_playback_manager_init(audio_playback_manager_t *manager,
                                const audio_playback_manager_config_t *config) {
    opus_decoder_config_t decoder_config;
    int ret = 0;

    if (manager == NULL) {
        return -EINVAL;
    }

    /* 先清空 manager，再灌一组默认配置。 */
    audio_playback_manager_reset(manager);
    audio_playback_manager_config_init(&manager->config);
    if (config != NULL) {
        manager->config = *config;
    }

    if (manager->config.device[0] == '\0') {
        snprintf(manager->config.device, sizeof(manager->config.device), "%s", "hw:0,0");
    }
    if (manager->config.max_queue_packets == 0) {
        manager->config.max_queue_packets = 128;
    }
    if (manager->config.max_packet_size == 0) {
        manager->config.max_packet_size = 1500;
    }

    /* 初始化队列同步原语。 */
    ret = pthread_mutex_init(&manager->queue_mutex, NULL);
    if (ret != 0) {
        return -ret;
    }

    ret = pthread_cond_init(&manager->queue_cond, NULL);
    if (ret != 0) {
        pthread_mutex_destroy(&manager->queue_mutex);
        return -ret;
    }

    /*
     * 分配固定容量环形队列。
     *
     * queue_buffer:
     *   一整块连续内存，按 max_packet_size 切成多个槽位。
     *
     * queue_packet_sizes:
     *   每个槽位当前包的真实长度。
     *
     * dequeue_buffer:
     *   播放线程出队时用的临时缓冲区。
     */
    manager->queue_buffer =
        (uint8_t *)calloc(manager->config.max_queue_packets * manager->config.max_packet_size,
                          sizeof(uint8_t));
    manager->queue_packet_sizes =
        (size_t *)calloc(manager->config.max_queue_packets, sizeof(size_t));
    manager->queue_packet_pts_us =
        (int64_t *)calloc(manager->config.max_queue_packets, sizeof(int64_t));
    manager->dequeue_buffer =
        (uint8_t *)calloc(manager->config.max_packet_size, sizeof(uint8_t));
    if (manager->queue_buffer == NULL ||
        manager->queue_packet_sizes == NULL ||
        manager->queue_packet_pts_us == NULL ||
        manager->dequeue_buffer == NULL) {
        audio_playback_manager_release_buffers(manager);
        pthread_cond_destroy(&manager->queue_cond);
        pthread_mutex_destroy(&manager->queue_mutex);
        return -ENOMEM;
    }

    /* 初始化 ALSA 播放模块。 */
    ret = audio_playback_init(&manager->playback,
                              manager->config.device,
                              manager->config.sample_rate,
                              manager->config.channels,
                              manager->config.format,
                              manager->config.period_size);
    if (ret < 0) {
        audio_playback_manager_release_buffers(manager);
        pthread_cond_destroy(&manager->queue_cond);
        pthread_mutex_destroy(&manager->queue_mutex);
        return ret;
    }

    /* 组装一份 Opus 解码器自己的配置。 */
    audio_opus_decoder_config_init(&decoder_config);
    decoder_config.sample_rate = (int)manager->config.sample_rate;
    decoder_config.channels = (int)manager->config.channels;
    decoder_config.max_frame_samples = manager->config.decoder_max_frame_samples;

    ret = audio_opus_decoder_init(&manager->decoder, &decoder_config);
    if (ret < 0) {
        audio_playback_close(&manager->playback);
        audio_playback_manager_release_buffers(manager);
        pthread_cond_destroy(&manager->queue_cond);
        pthread_mutex_destroy(&manager->queue_mutex);
        return ret;
    }

    /*
     * 把解码模块的“PCM 回调”挂回 manager。
     * 这样 manager 就能在解码完成后把 PCM 继续交给 playback 层。
     */
    audio_opus_decoder_set_pcm_callback(&manager->decoder,
                                        audio_playback_manager_on_pcm,
                                        manager);

    manager->is_initialized = 1;
    return 0;
}

/*
 * 启动后台播放线程。
 *
 * 启动后数据流向是：
 * push_packet -> queue -> playback_thread -> decoder -> playback
 */
int audio_playback_manager_start(audio_playback_manager_t *manager) {
    int ret = 0;

    if (manager == NULL) {
        return -EINVAL;
    }

    /* 允许调用方偷懒：如果还没 init，就先补一次默认 init。 */
    if (!manager->is_initialized) {
        ret = audio_playback_manager_init(manager, NULL);
        if (ret < 0) {
            return ret;
        }
    }

    if (manager->is_running) {
        return -EBUSY;
    }

    /* 清掉 stop 标记并拉起后台线程。 */
    manager->request_stop = 0;
    manager->is_running = 1;

    ret = pthread_create(&manager->playback_thread,
                         NULL,
                         audio_playback_manager_thread_main,
                         manager);
    if (ret != 0) {
        manager->is_running = 0;
        return -ret;
    }

    return 0;
}

/*
 * 往 manager 里推一个 Opus 包。
 *
 * 这个接口非常适合直接挂在 libdatachannel 的 onFrame 回调里。
 * 它只做轻量工作：
 * - 参数检查
 * - 判断队列是否已满
 * - 把包拷贝进环形数组的一个槽位
 *
 * 它故意不在这里直接做解码和播放，
 * 这样就能避免网络回调线程被 snd_pcm_writei() 这种阻塞操作拖住。
 */
int audio_playback_manager_push_packet(audio_playback_manager_t *manager,
                                       const uint8_t *packet,
                                       size_t packet_len) {
    return audio_playback_manager_push_packet_with_pts(manager, packet, packet_len, 0);
}

int audio_playback_manager_push_packet_with_pts(audio_playback_manager_t *manager,
                                                const uint8_t *packet,
                                                size_t packet_len,
                                                int64_t pts_us) {
    if (manager == NULL || packet == NULL || packet_len == 0) {
        return -EINVAL;
    }

    if (!manager->is_initialized) {
        return -EINVAL;
    }
    if (packet_len > manager->config.max_packet_size) {
        return -EMSGSIZE;
    }

    pthread_mutex_lock(&manager->queue_mutex);
    /* 队列满了就直接报错，让上层决定丢包策略。 */
    if (manager->queued_packets >= manager->config.max_queue_packets) {
        pthread_mutex_unlock(&manager->queue_mutex);
        return -ENOBUFS;
    }

    /* 把新包拷贝进当前写槽位。 */
    memcpy(manager->queue_buffer +
               (manager->queue_write_index * manager->config.max_packet_size),
           packet,
           packet_len);
    manager->queue_packet_sizes[manager->queue_write_index] = packet_len;
    manager->queue_packet_pts_us[manager->queue_write_index] = pts_us;

    /* 写指针前进一格。 */
    manager->queue_write_index =
        (manager->queue_write_index + 1) % manager->config.max_queue_packets;
    manager->queued_packets++;

    /* 唤醒后台播放线程去消费。 */
    pthread_cond_signal(&manager->queue_cond);
    pthread_mutex_unlock(&manager->queue_mutex);

    return 0;
}

int audio_playback_manager_get_clock_us(audio_playback_manager_t *manager,
                                        int64_t *clock_us) {
    if (manager == NULL || clock_us == NULL) {
        return -EINVAL;
    }

    pthread_mutex_lock(&manager->queue_mutex);
    if (!manager->audio_clock_valid) {
        pthread_mutex_unlock(&manager->queue_mutex);
        return -EAGAIN;
    }

    *clock_us = manager->audio_clock_us;
    pthread_mutex_unlock(&manager->queue_mutex);
    return 0;
}

/*
 * 停止后台播放线程。
 *
 * 当前 stop 的核心是：
 * - 置 request_stop
 * - 唤醒可能阻塞等待中的线程
 * - join 等待线程完全退出
 */
void audio_playback_manager_stop(audio_playback_manager_t *manager) {
    if (manager == NULL) {
        return;
    }

    pthread_mutex_lock(&manager->queue_mutex);
    manager->request_stop = 1;
    pthread_cond_broadcast(&manager->queue_cond);
    pthread_mutex_unlock(&manager->queue_mutex);

    if (manager->playback_thread) {
        pthread_join(manager->playback_thread, NULL);
        memset(&manager->playback_thread, 0, sizeof(manager->playback_thread));
    }
}

/*
 * 关闭整条下行链路并释放资源。
 *
 * 当前会：
 * - stop 播放线程
 * - 清空环形队列状态
 * - 关闭 Opus 解码模块
 * - 关闭 ALSA 播放模块
 * - 释放队列缓冲区和同步原语
 */
void audio_playback_manager_close(audio_playback_manager_t *manager) {
    if (manager == NULL) {
        return;
    }

    if (manager->is_initialized) {
        audio_playback_manager_stop(manager);
    }

    pthread_mutex_lock(&manager->queue_mutex);
    audio_playback_manager_free_queue(manager);
    pthread_mutex_unlock(&manager->queue_mutex);

    audio_opus_decoder_close(&manager->decoder);
    audio_playback_close(&manager->playback);
    audio_playback_manager_release_buffers(manager);

    pthread_cond_destroy(&manager->queue_cond);
    pthread_mutex_destroy(&manager->queue_mutex);
    audio_playback_manager_reset(manager);
}
