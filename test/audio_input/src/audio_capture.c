#include "audio_capture.h"

#include <alloca.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 把上下文恢复到“全空”状态。
 *
 * 这个函数是采集层清理逻辑的基础：
 * - init 前先清零，避免带入旧状态
 * - close 结束后也会把结构体打回一个可复用的空状态
 */
static void audio_capture_reset(audio_capture_ctx_t *ctx) {
    if (ctx == NULL) {
        return;
    }

    memset(ctx, 0, sizeof(*ctx));
}

/*
 * 采集线程启动参数。
 *
 * 现在这里只传一个 ctx，但单独包一层结构有两个好处：
 * 1. 避免直接把临时栈变量地址传给 pthread
 * 2. 以后如果还要给线程传别的启动参数，扩展比较自然
 */
typedef struct capture_thread_arg {
    audio_capture_ctx_t *ctx;
} capture_thread_arg_t;

/*
 * 后台采集线程主循环。
 *
 * 线程职责非常单纯：
 * 1. 按 period_size 持续读 ALSA
 * 2. 拿到 PCM 后通过回调抛给上层
 *
 * 它故意不做：
 * - Opus 编码
 * - 网络发送
 * - 文件落盘
 *
 * 这样 audio_capture 仍然是一个“纯采集模块”。
 */
static void *audio_capture_thread_main(void *arg) {
    capture_thread_arg_t *thread_arg = (capture_thread_arg_t *)arg;
    audio_capture_ctx_t *ctx = NULL;

    if (thread_arg == NULL) {
        return NULL;
    }

    ctx = thread_arg->ctx;
    free(thread_arg);

    /*
     * 按当前 period_size 持续读取音频。
     * 只要没有被外部 stop，且 read 没遇到不可恢复错误，就一直跑。
     */
    while (ctx != NULL && !ctx->requestStop) {
        snd_pcm_sframes_t got = audio_capture_read(ctx, ctx->period_size);

        /*
         * XRUN 在 audio_capture_read() 里已经做过 prepare 恢复。
         * 这里直接进入下一轮读即可。
         */
        if (got == -EPIPE) {
            continue;
        }

        /*
         * 其他负值表示不可继续的错误。
         * 当前策略是直接退出线程，让上层决定后续如何处理。
         */
        if (got < 0) {
            break;
        }

        /*
         * got > 0 时表示这次确实采到了有效 PCM。
         * 如果上层注册了回调，就把本次这包 PCM 交出去。
         *
         * 注意：
         * 这里传出去的是 ctx->buffer 的地址，不会再额外复制一份。
         * 所以上层如果要异步持有这包数据，需要自己复制。
         */
        if (ctx->pcm_cb != NULL) {
            ctx->pcm_cb(ctx->buffer, got, ctx->frame_bytes, ctx->pcm_cb_user_data);
        }
    }

    /*
     * 线程退出前清掉运行状态。
     * 这能让上层知道当前采集循环已经结束。
     */
    if (ctx != NULL) {
        ctx->isRunning = 0;
    }

    return NULL;
}

/*
 * 注册 PCM 回调。
 *
 * 采集层只保存：
 * - 回调函数地址
 * - 对应的 user_data
 *
 * 真正触发回调要等到后台线程读到音频数据以后。
 */
void audio_capture_set_callback(audio_capture_ctx_t *ctx,
                                audio_pcm_callback_t cb,
                                void *user_data) {
    if (ctx == NULL) {
        return;
    }

    ctx->pcm_cb = cb;
    ctx->pcm_cb_user_data = user_data;
}

/*
 * 初始化 ALSA 采集设备。
 *
 * 这个函数会完成整个设备准备流程：
 * - 打开 PCM 设备
 * - 配 access / format / rate / channels / period
 * - prepare 设备
 * - 分配一块 period 对应大小的采集缓冲区
 *
 * 初始化成功后，ctx 里保存的是“驱动最终接受的实际参数”。
 */
int audio_capture_init(audio_capture_ctx_t *ctx,
                       const char *device,
                       unsigned int sample_rate,
                       unsigned int channels,
                       snd_pcm_format_t format,
                       snd_pcm_uframes_t period_size) {
    snd_pcm_hw_params_t *hw_params = NULL;
    int err = 0;
    int dir = 0;
    const char *pcm_device = device != NULL ? device : "hw:1,0";

    if (ctx == NULL) {
        return -EINVAL;
    }

    /* 每次初始化都从一个干净上下文开始。 */
    audio_capture_reset(ctx);

    /* 给上层没填或填 0 的参数一个可工作的默认值。 */
    if (sample_rate == 0) {
        sample_rate = 48000;
    }
    if (channels == 0) {
        channels = 2;
    }
    if (format == SND_PCM_FORMAT_UNKNOWN) {
        format = SND_PCM_FORMAT_S16_LE;
    }
    if (period_size == 0) {
        period_size = 960;
    }

    /* 打开 ALSA PCM capture 设备。 */
    err = snd_pcm_open(&ctx->pcm_handle, pcm_device, SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        fprintf(stderr, "snd_pcm_open(%s) failed: %s\n", pcm_device, snd_strerror(err));
        audio_capture_close(ctx);
        return err;
    }

    /* 在栈上分配硬件参数对象。 */
    snd_pcm_hw_params_alloca(&hw_params);

    /* 读取设备支持能力范围，后面所有 set 都基于这份对象修改。 */
    err = snd_pcm_hw_params_any(ctx->pcm_handle, hw_params);
    if (err < 0) {
        fprintf(stderr, "snd_pcm_hw_params_any failed: %s\n", snd_strerror(err));
        audio_capture_close(ctx);
        return err;
    }

    /*
     * 交错模式：
     * 多声道时，一个 frame 里按声道顺序排列。
     * 例如双声道 S16_LE 会是 L R L R ...
     */
    err = snd_pcm_hw_params_set_access(ctx->pcm_handle,
                                       hw_params,
                                       SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0) {
        fprintf(stderr, "snd_pcm_hw_params_set_access failed: %s\n", snd_strerror(err));
        audio_capture_close(ctx);
        return err;
    }

    /* 设置 PCM 采样格式，例如 S16_LE。 */
    err = snd_pcm_hw_params_set_format(ctx->pcm_handle, hw_params, format);
    if (err < 0) {
        fprintf(stderr, "snd_pcm_hw_params_set_format failed: %s\n", snd_strerror(err));
        audio_capture_close(ctx);
        return err;
    }

    /*
     * 用 near 接口设置采样率。
     * near 的意思是：设备如果不支持你要求的精确值，就选一个最接近的。
     */
    err = snd_pcm_hw_params_set_rate_near(ctx->pcm_handle, hw_params, &sample_rate, NULL);
    if (err < 0) {
        fprintf(stderr, "snd_pcm_hw_params_set_rate_near failed: %s\n", snd_strerror(err));
        audio_capture_close(ctx);
        return err;
    }

    /* 设置声道数。 */
    err = snd_pcm_hw_params_set_channels(ctx->pcm_handle, hw_params, channels);
    if (err < 0) {
        fprintf(stderr, "snd_pcm_hw_params_set_channels failed: %s\n", snd_strerror(err));
        audio_capture_close(ctx);
        return err;
    }

    /*
     * 设置 period 大小。
     * 这里的 period 可以理解成“驱动每次准备给应用读的一包音频帧数”。
     */
    err = snd_pcm_hw_params_set_period_size_near(ctx->pcm_handle,
                                                 hw_params,
                                                 &period_size,
                                                 &dir);
    if (err < 0) {
        fprintf(stderr, "snd_pcm_hw_params_set_period_size_near failed: %s\n",
                snd_strerror(err));
        audio_capture_close(ctx);
        return err;
    }

    /* 把上面这组硬件参数真正提交给驱动。 */
    err = snd_pcm_hw_params(ctx->pcm_handle, hw_params);
    if (err < 0) {
        fprintf(stderr, "snd_pcm_hw_params failed: %s\n", snd_strerror(err));
        audio_capture_close(ctx);
        return err;
    }

    /*
     * 把驱动实际接受的 period_size 再读回来。
     * 因为 near 接口有可能把你申请的值调成设备支持的最近值。
     */
    err = snd_pcm_hw_params_get_period_size(hw_params, &period_size, &dir);
    if (err < 0) {
        fprintf(stderr, "snd_pcm_hw_params_get_period_size failed: %s\n",
                snd_strerror(err));
        audio_capture_close(ctx);
        return err;
    }

    /* 让设备进入 prepared 状态，后续就可以 read 了。 */
    err = snd_pcm_prepare(ctx->pcm_handle);
    if (err < 0) {
        fprintf(stderr, "snd_pcm_prepare failed: %s\n", snd_strerror(err));
        audio_capture_close(ctx);
        return err;
    }

    /* 记录最终生效的实际参数。 */
    ctx->format = format;
    ctx->sample_rate = sample_rate;
    ctx->channels = channels;
    ctx->period_size = period_size;

    /*
     * 一帧的字节数 = 每声道样本字节数 * 声道数。
     * 例如：
     * - S16_LE mono  => 2
     * - S16_LE stereo => 4
     */
    ctx->frame_bytes =
        (size_t)((snd_pcm_format_physical_width(format) / 8) * channels);

    /* 一包缓冲区大小 = period_size * frame_bytes。 */
    ctx->buffer_bytes = (size_t)period_size * ctx->frame_bytes;
    ctx->buffer = (uint8_t *)calloc(1, ctx->buffer_bytes);
    if (ctx->buffer == NULL) {
        fprintf(stderr, "calloc buffer failed, size=%zu\n", ctx->buffer_bytes);
        audio_capture_close(ctx);
        return -ENOMEM;
    }

    /* 初始化线程控制状态。 */
    ctx->requestStop = 0;
    ctx->isRunning = 0;
    memset(&ctx->capture_thread, 0, sizeof(ctx->capture_thread));

    fprintf(stdout,
            "ALSA capture init ok: device=%s rate=%u channels=%u period=%lu frame_bytes=%zu\n",
            pcm_device,
            ctx->sample_rate,
            ctx->channels,
            (unsigned long)ctx->period_size,
            ctx->frame_bytes);

    return 0;
}

/*
 * 请求停止采集。
 *
 * 分两步：
 * 1. 把 requestStop 置位，告诉线程“该退出了”
 * 2. 调 snd_pcm_drop() 打断可能阻塞中的 read
 *
 * 这样 stop 的响应会更快，不需要等下一次正常返回。
 */
void audio_stop_capture(audio_capture_ctx_t *ctx) {
    if (ctx == NULL) {
        return;
    }

    ctx->requestStop = 1;

    if (ctx->pcm_handle != NULL) {
        snd_pcm_drop(ctx->pcm_handle);
    }
}

/*
 * 同步读取一包 PCM。
 *
 * 成功时返回“实际读取到的帧数”。
 * 错误时返回负值。
 *
 * 特别说明：
 * - EPIPE 代表 XRUN，这里会先 prepare 恢复，再返回 -EPIPE 给上层
 * - 其他 recoverable 错误会尝试 snd_pcm_recover()
 */
snd_pcm_sframes_t audio_capture_read(audio_capture_ctx_t *ctx,
                                     snd_pcm_uframes_t frames) {
    snd_pcm_sframes_t got = 0;

    if (ctx == NULL || ctx->pcm_handle == NULL || ctx->buffer == NULL) {
        return -EINVAL;
    }

    /* 不允许要求超过当前 buffer 可承载的帧数。 */
    if (frames == 0 || frames > ctx->period_size) {
        frames = ctx->period_size;
    }

    /* 把一包交错 PCM 读到 ctx->buffer。 */
    got = snd_pcm_readi(ctx->pcm_handle, ctx->buffer, frames);
    if (got == -EPIPE) {
        /*
         * XRUN 通常表示应用读慢了，或者设备状态被外部打断。
         * capture 场景里 prepare 后大多还能继续跑。
         */
        fprintf(stderr, "XRUN occurred, recovering with snd_pcm_prepare\n");
        snd_pcm_prepare(ctx->pcm_handle);
        return -EPIPE;
    }

    if (got < 0) {
        /*
         * 其他错误交给 ALSA 通用恢复接口。
         * recover 成功后，本轮数据仍视为无效，所以这里返回 0。
         */
        got = snd_pcm_recover(ctx->pcm_handle, (int)got, 0);
        if (got < 0) {
            fprintf(stderr, "snd_pcm_readi/snd_pcm_recover failed: %s\n",
                    snd_strerror((int)got));
            return got;
        }

        return 0;
    }

    return got;
}

/*
 * 启动后台采集线程。
 *
 * 这个接口只负责启动“持续采集”循环，
 * 并不关心采到的数据后续怎么处理，那部分都交给回调。
 */
int audio_start_capture(audio_capture_ctx_t *ctx) {
    capture_thread_arg_t *thread_arg = NULL;
    int thread_err = 0;

    if (ctx == NULL) {
        fprintf(stderr, "audio_start_capture: ctx is NULL\n");
        return -EINVAL;
    }

    if (ctx->pcm_handle == NULL || ctx->buffer == NULL) {
        fprintf(stderr, "audio_start_capture: capture is not initialized\n");
        return -EINVAL;
    }

    if (ctx->isRunning) {
        fprintf(stderr, "audio capture is already running\n");
        return -EBUSY;
    }

    thread_arg = (capture_thread_arg_t *)calloc(1, sizeof(*thread_arg));
    if (thread_arg == NULL) {
        return -ENOMEM;
    }

    thread_arg->ctx = ctx;
    ctx->requestStop = 0;
    ctx->isRunning = 1;

    thread_err = pthread_create(&ctx->capture_thread,
                                NULL,
                                audio_capture_thread_main,
                                thread_arg);
    if (thread_err != 0) {
        free(thread_arg);
        ctx->isRunning = 0;
        ctx->requestStop = 0;
        return -thread_err;
    }

    return 0;
}

/*
 * 关闭采集模块并释放所有资源。
 *
 * 顺序很重要：
 * 1. 先 stop，通知线程退出并打断 read
 * 2. 再 join，确保线程彻底结束
 * 3. 最后释放 handle 和 buffer
 */
void audio_capture_close(audio_capture_ctx_t *ctx) {
    if (ctx == NULL) {
        return;
    }

    audio_stop_capture(ctx);

    if (ctx->capture_thread) {
        pthread_join(ctx->capture_thread, NULL);
        memset(&ctx->capture_thread, 0, sizeof(ctx->capture_thread));
    }

    if (ctx->pcm_handle != NULL) {
        snd_pcm_close(ctx->pcm_handle);
        ctx->pcm_handle = NULL;
    }

    if (ctx->buffer != NULL) {
        free(ctx->buffer);
        ctx->buffer = NULL;
    }

    audio_capture_reset(ctx);
}
