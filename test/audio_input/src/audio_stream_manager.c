#include "audio_stream_manager.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/*
 * Opus 模块编码出一个包后，最终会先回调到 manager 这一层。
 *
 * manager 的职责不是自己“消费”这个包，
 * 而是把它继续转交给更上层注册的 packet_cb。
 *
 * 你后面如果要接发送逻辑，通常就会通过
 * audio_stream_manager_set_packet_callback() 把发送函数挂到这里。
 */
static int audio_stream_manager_on_opus_packet(const uint8_t *packet,
                                               size_t packet_len,
                                               void *user_data) {
    audio_stream_manager_t *stream = (audio_stream_manager_t *)user_data;

    if (stream == NULL) {
        return -EINVAL;
    }

    /* 如果业务层注册了“最终 Opus 包出口”，这里直接转发。 */
    if (stream->packet_cb != NULL) {
        return stream->packet_cb(packet, packet_len, stream->packet_cb_user_data);
    }
    printf("got an Opus packet, but no packet_cb registered to consume it\n");
    printf("opus packet length: %zu bytes\n", packet_len);
    // 打印前100字节数据，方便调试和验证。
    size_t print_bytes = packet_len;
    if (print_bytes > 100) {
        print_bytes = 100;
    }
    printf("前100字节Opus包数据: ");
    for (size_t i = 0; i < print_bytes; i++) {
        printf("%02x ", packet[i]);
    }
    printf("\n");
    /*
     * 没注册也不算错误。
     * 这允许你先把整条链路跑通，后面再慢慢把发送逻辑挂上来。
     */
    return 0;
}

/*
 * 采集层抛 PCM 时，会先打到 manager 的这个回调。
 *
 * manager 在这里做的事情非常简单：
 * - 不做复杂音频处理
 * - 不自己编码
 * - 只是把 PCM 继续推给 Opus 模块
 *
 * 这样 manager 仍然只是“调度层”，而不是把所有音频细节都揉在一个函数里。
 */
static void audio_stream_manager_on_pcm_data(const uint8_t *data,
                                             snd_pcm_sframes_t frames,
                                             size_t frame_bytes,
                                             void *user_data) {
    audio_stream_manager_t *stream = (audio_stream_manager_t *)user_data;
    int ret = 0;

    if (stream == NULL || frames <= 0) {
        return;
    }

    /*
     * 把 PCM 喂给 Opus 模块。
     * Opus 模块内部会自己攒够一个完整帧长再真正编码。
     */
    ret = audio_opus_encoder_push_pcm(&stream->encoder,
                                      data,
                                      (size_t)frames,
                                      frame_bytes);
    if (ret < 0) {
        fprintf(stderr, "audio_opus_encoder_push_pcm failed: %d\n", ret);
    }

    printf("audio_stream_manager_on_pcm_data: got %ld frames, pushed to opus with ret=%d\n",
           frames, ret);
    //打印前100字节数据，方便调试和验证。
    size_t print_bytes = frame_bytes * (size_t)frames;
    if (print_bytes > 100) {
        print_bytes = 100; 
    }
    printf("前100字节PCM数据: ");
    // 打印前100字节数据
    for (size_t i = 0; i < print_bytes; i++) {
        printf("%02x ", data[i]);
    }
    printf("\n");


}

/*
 * 给 manager 配一组默认参数。
 *
 * 这些默认值的目标是：
 * - 直接可用
 * - 对语音流友好
 * - 和你当前 ALSA/Opus 使用场景基本对齐
 */
void audio_stream_manager_config_init(audio_stream_manager_config_t *config) {
    if (config == NULL) {
        return;
    }

    memset(config, 0, sizeof(*config));

    /* 采集默认值。 */
    snprintf(config->device, sizeof(config->device), "%s", "plughw:2,0");
    config->sample_rate = 48000;
    config->channels = 1;
    config->format = SND_PCM_FORMAT_S16_LE;
    config->period_size = 960;

    /* Opus 默认值。48k / 20ms / 语音应用。 */
    config->opus_application = OPUS_APPLICATION_VOIP;
    config->opus_bitrate = 32000;
    config->opus_frame_samples = 960;
    config->opus_max_packet_size = 1500;
}

/*
 * 注册“最终 Opus 包出口”。
 *
 * 你可以把这里理解成 manager 对业务层暴露的发送挂点。
 */
void audio_stream_manager_set_packet_callback(audio_stream_manager_t *stream,
                                              audio_stream_packet_callback_t cb,
                                              void *user_data) {
    if (stream == NULL) {
        return;
    }

    stream->packet_cb = cb;
    stream->packet_cb_user_data = user_data;
}

/*
 * 初始化 manager。
 *
 * 当前只做两件事：
 * 1. 把整个 manager 清零
 * 2. 准备一份配置
 *
 * 真正昂贵的动作，比如：
 * - 打开 ALSA 设备
 * - 创建 OpusEncoder
 *
 * 都留到 start() 再做，这样 init 更轻量，也更容易复用。
 */
int audio_stream_manager_init(audio_stream_manager_t *stream,
                              const audio_stream_manager_config_t *config) {
    if (stream == NULL) {
        return -EINVAL;
    }

    memset(stream, 0, sizeof(*stream));
    audio_stream_manager_config_init(&stream->config);
    printf("------device: %s\n", stream->config.device);
    if (config != NULL) {
        stream->config = *config;
    }

    printf("------device: %s\n", stream->config.device);
    /*
     * 防御式处理：
     * 如果外部传进来的 device 为空字符串，就回退到默认设备。
     */
    if (stream->config.device[0] == '\0') {
        snprintf(stream->config.device, sizeof(stream->config.device), "%s", "hw:1,0");
    }

    stream->is_initialized = 1;
    return 0;
}

/*
 * 启动整条音频流。
 *
 * 数据链路长这样：
 *
 *   audio_capture thread
 *        |
 *        v
 *   audio_stream_manager_on_pcm_data()
 *        |
 *        v
 *   audio_opus_encoder_push_pcm()
 *        |
 *        v
 *   audio_stream_manager_on_opus_packet()
 *        |
 *        v
 *   stream->packet_cb(...)
 *
 * 也就是说：
 * - PCM 的“入口”在 capture -> manager
 * - Opus 包的“出口”在 manager -> 业务层
 */
int audio_stream_manager_start(audio_stream_manager_t *stream) {
    opus_encoder_config_t encoder_config;
    int ret = 0;

    if (stream == NULL) {
        return -EINVAL;
    }

    /*
     * 允许调用方偷懒：
     * 如果还没 init，就按默认配置补一次 init。
     */
    if (!stream->is_initialized) {
        ret = audio_stream_manager_init(stream, NULL);
        if (ret < 0) {
            return ret;
        }
    }

    /*
     * 当前这条链路只支持把 S16_LE PCM 直接喂给 Opus 模块。
     * 如果后面你要支持 S24/S32，需要在 manager 或更下层做格式转换。
     */
    if (stream->config.format != SND_PCM_FORMAT_S16_LE) {
        fprintf(stderr, "audio_stream_manager only supports S16_LE pcm for opus\n");
        return -EINVAL;
    }

    //打印当前的配置，方便调试和验证。
    printf("audio_stream_manager configuration:\n");
    printf("  device: %s\n", stream->config.device);
    printf("  sample_rate: %u\n", stream->config.sample_rate);
    printf("  channels: %u\n", stream->config.channels);
    printf("  format: %d\n", stream->config.format);
    printf("  period_size: %lu\n", stream->config.period_size);
    printf("  opus_application: %d\n", stream->config.opus_application);
    printf("  opus_bitrate: %d\n", stream->config.opus_bitrate);
    printf("  opus_frame_samples: %d\n", stream->config.opus_frame_samples);
    printf("  opus_max_packet_size: %d\n", stream->config.opus_max_packet_size);
    /*
     * 用 manager 的总配置，拼一份 Opus 模块自己的配置。
     * 这样编码模块仍然独立，但 manager 统一掌握整条链路的参数。
     */
    audio_opus_encoder_config_init(&encoder_config);
    encoder_config.sample_rate = (int)stream->config.sample_rate;
    encoder_config.channels = (int)stream->config.channels;
    encoder_config.application = stream->config.opus_application;
    encoder_config.bitrate = stream->config.opus_bitrate;
    encoder_config.frame_samples = stream->config.opus_frame_samples; //- 每帧样本数（48kHz × 0.02s = 960）
    encoder_config.max_packet_size = stream->config.opus_max_packet_size;//避免IP分片（MTU限制）

    /* 先准备 Opus 模块。 */
    ret = audio_opus_encoder_init(&stream->encoder, &encoder_config);
    if (ret < 0) {
        return ret;
    }

    /*
     * 把 Opus 模块的“编码完成回调”挂回 manager。
     * 这样 manager 能接住编码后的包，再决定怎么往上交。
     */
    audio_opus_encoder_set_packet_callback(&stream->encoder,
                                           audio_stream_manager_on_opus_packet,
                                           stream);

    /* 再准备采集模块。 */
    ret = audio_capture_init(&stream->capture,
                             stream->config.device,
                             stream->config.sample_rate,
                             stream->config.channels,
                             stream->config.format,
                             stream->config.period_size);
    if (ret < 0) {
        audio_opus_encoder_close(&stream->encoder);
        return ret;
    }

    /*
     * 把采集层的 PCM 回调挂回 manager。
     * 这样采集线程一读到数据，就会先打到 manager。
     */
    audio_capture_set_callback(&stream->capture,
                               audio_stream_manager_on_pcm_data,
                               stream);

    /* 最后真正启动采集线程。 */
    ret = audio_start_capture(&stream->capture);
    if (ret < 0) {
        audio_capture_close(&stream->capture);
        audio_opus_encoder_close(&stream->encoder);
        return ret;
    }

    return 0;
}

/*
 * 停止整条流。
 *
 * 当前 stop 的核心是“先停采集”：
 * 因为只要麦克风不再继续产出 PCM，后面的编码链自然也就停了。
 */
void audio_stream_manager_stop(audio_stream_manager_t *stream) {
    if (stream == NULL) {
        return;
    }

    audio_stop_capture(&stream->capture);
}

/*
 * 关闭整条流。
 *
 * 这里比 stop 多做两件事：
 * 1. 释放 capture 资源
 * 2. 把 Opus 里尾巴上还没凑满一帧的 PCM 补零编码出去，然后释放编码器
 *
 * flush 放在 close 里，是为了尽量不丢最后那一点残留音频。
 */
void audio_stream_manager_close(audio_stream_manager_t *stream) {
    if (stream == NULL) {
        return;
    }

    audio_capture_close(&stream->capture);
    audio_opus_encoder_flush(&stream->encoder);
    audio_opus_encoder_close(&stream->encoder);
}
