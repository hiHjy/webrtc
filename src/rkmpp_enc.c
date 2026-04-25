#include "rkmpp_enc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mpp_meta.h"

#define RKMPP_ALIGN(value, align) (((value) + (align) - 1) & ~((align) - 1))

/*
 * 这份 encoder 代码现在故意整理成和 decoder 类似的风格：
 *
 * 外部只需要关心 5 个动作：
 * 1. rk_mpp_encoder_init()
 * 2. rk_mpp_encoder_set_packet_callback()
 * 3. rk_mpp_encoder_write_header()
 * 4. rk_mpp_encoder_send_frame()
 * 5. rk_mpp_encoder_deinit()
 *
 * 但编码器和解码器在“数据流方向”上天然不一样，所以你会看到：
 *
 * decoder:
 *   压缩码流 -> 解码器 -> 原始图像帧
 *
 * encoder:
 *   原始图像帧 -> 编码器 -> 压缩码流
 *
 * 也正因为方向不同，MPP 的接口习惯也不完全一样：
 * - 解码时你不断喂 packet，再不断取 frame
 * - 编码时你不断喂 frame，再不断取 packet
 *
 * 你可以把它理解成一对镜像关系。
 */

/*
 * 这里根据 stride 计算一帧输入图像和一帧输出码流的大致 buffer 大小。
 *
 * 当前我们固定按 NV12 来理解输入，所以一帧图像大小约等于：
 *   h_stride * v_stride * 3 / 2
 *
 * packet_size 这里先按 frame_size 申请，是一个“偏保守但省心”的教学写法。
 * 真要极致优化时，可以再根据码率/场景做更细致的设计。
 */
static void rk_mpp_encoder_calc_buffer_size(RkMppEncoder *enc)
{
    enc->frame_size = (size_t)enc->h_stride * enc->v_stride * 3 / 2;
    enc->packet_size = enc->frame_size;
}

/*
 * 统一处理“编码器吐出来的一块码流”。
 *
 * 这里做三件事：
 * 1. 可选写文件，方便你本地 dump 验证；
 * 2. 更新运行时状态，例如 frame_count / pkt_eos；
 * 3. 通过回调把码流抛给业务层。
 *
 * 这样主流程里就不用到处重复“写文件 + 记状态 + 回调”的逻辑了。
 */
static int rk_mpp_encoder_emit_packet(RkMppEncoder *enc, MppPacket packet, int is_header)
{
    const uint8_t *data = NULL;
    size_t size = 0;
    int eos = 0;

    if (!enc || !packet)
        return -1;

    data = (const uint8_t *)mpp_packet_get_pos(packet);
    size = mpp_packet_get_length(packet);
    eos = mpp_packet_get_eos(packet);

    if (size > 0 && enc->f_out)
        fwrite(data, 1, size, enc->f_out);

    if (!is_header) {
        enc->pkt_eos = eos;
        enc->frame_count++;
        printf("encoded frame %d, packet size=%zu eos=%d\n",
               enc->frame_count, size, eos);
    } else {
        printf("encoded header size=%zu\n", size);
    }

    if (size > 0 && enc->packet_callback)
        enc->packet_callback(data, size, is_header, eos, enc->packet_callback_userdata);

    return 0;
}

/*
 * 这一段就是编码器真正的“准备阶段”。
 *
 * 你可以按下面这条主线去理解：
 * 1. 创建 MPP 编码器实例
 * 2. 取出默认配置
 * 3. 设置输入图像参数
 * 4. 设置码率控制参数
 * 5. 设置编码格式相关参数
 * 6. 申请输入/输出 buffer
 *
 * 其中最值得记住的是三类配置：
 *
 * prep:
 *   输入图像长什么样，宽高、stride、像素格式是什么
 *
 * rc:
 *   码率控制怎么做，例如 CBR、fps、gop、目标码率
 *
 * codec:
 *   最终要编码成什么格式，例如 H.264 / H.265
 */
static int rk_mpp_encoder_prepare(RkMppEncoder *enc)
{
    MPP_RET ret = MPP_OK;

    ret = mpp_create(&enc->ctx, &enc->mpi);
    if (ret) {
        printf("mpp_create failed ret=%d\n", ret);
        return -1;
    }

    ret = mpp_init(enc->ctx, MPP_CTX_ENC, enc->type);
    if (ret) {
        printf("mpp_init failed ret=%d\n", ret);
        return -1;
    }

    ret = mpp_enc_cfg_init(&enc->enc_cfg);
    if (ret) {
        printf("mpp_enc_cfg_init failed ret=%d\n", ret);
        return -1;
    }

    ret = enc->mpi->control(enc->ctx, MPP_ENC_GET_CFG, enc->enc_cfg);
    if (ret) {
        printf("MPP_ENC_GET_CFG failed ret=%d\n", ret);
        return -1;
    }

    /* 告诉编码器：你接下来要吃到的原始图像长什么样 */
    mpp_enc_cfg_set_s32(enc->enc_cfg, "prep:width", enc->width);
    mpp_enc_cfg_set_s32(enc->enc_cfg, "prep:height", enc->height);
    mpp_enc_cfg_set_s32(enc->enc_cfg, "prep:hor_stride", enc->h_stride);
    mpp_enc_cfg_set_s32(enc->enc_cfg, "prep:ver_stride", enc->v_stride);
    mpp_enc_cfg_set_s32(enc->enc_cfg, "prep:format", enc->fmt);

    /* 这里先用最常见也最容易理解的 CBR 固定码率方式 */
    mpp_enc_cfg_set_s32(enc->enc_cfg, "rc:mode", MPP_ENC_RC_MODE_CBR);
    mpp_enc_cfg_set_s32(enc->enc_cfg, "rc:fps_in_flex", 0);
    mpp_enc_cfg_set_s32(enc->enc_cfg, "rc:fps_in_num", enc->fps);
    mpp_enc_cfg_set_s32(enc->enc_cfg, "rc:fps_in_denorm", 1);
    mpp_enc_cfg_set_s32(enc->enc_cfg, "rc:fps_out_flex", 0);
    mpp_enc_cfg_set_s32(enc->enc_cfg, "rc:fps_out_num", enc->fps);
    mpp_enc_cfg_set_s32(enc->enc_cfg, "rc:fps_out_denorm", 1);
    mpp_enc_cfg_set_s32(enc->enc_cfg, "rc:gop", enc->gop);
    mpp_enc_cfg_set_s32(enc->enc_cfg, "rc:bps_target", enc->bps);
    mpp_enc_cfg_set_s32(enc->enc_cfg, "rc:bps_max", enc->bps * 17 / 16);
    mpp_enc_cfg_set_s32(enc->enc_cfg, "rc:bps_min", enc->bps * 15 / 16);
    mpp_enc_cfg_set_s32(enc->enc_cfg, "rc:qp_init", -1);
    mpp_enc_cfg_set_s32(enc->enc_cfg, "rc:qp_max", 48);
    mpp_enc_cfg_set_s32(enc->enc_cfg, "rc:qp_min", 10);
    mpp_enc_cfg_set_s32(enc->enc_cfg, "rc:qp_max_i", 48);
    mpp_enc_cfg_set_s32(enc->enc_cfg, "rc:qp_min_i", 10);
    mpp_enc_cfg_set_s32(enc->enc_cfg, "rc:qp_ip", 2);

    /* codec:type 决定最终输出码流类型，例如 AVC 就是 H.264 */
    mpp_enc_cfg_set_s32(enc->enc_cfg, "codec:type", enc->type);
    if (enc->type == MPP_VIDEO_CodingAVC) {
        mpp_enc_cfg_set_s32(enc->enc_cfg, "h264:profile", 100);
        mpp_enc_cfg_set_s32(enc->enc_cfg, "h264:level", 40);
        mpp_enc_cfg_set_s32(enc->enc_cfg, "h264:cabac_en", 1);
        mpp_enc_cfg_set_s32(enc->enc_cfg, "h264:trans8x8", 1);
    }

    ret = enc->mpi->control(enc->ctx, MPP_ENC_SET_CFG, enc->enc_cfg);
    if (ret) {
        printf("MPP_ENC_SET_CFG failed ret=%d\n", ret);
        return -1;
    }

    {
        MppEncHeaderMode header_mode = MPP_ENC_HEADER_MODE_EACH_IDR;
        ret = enc->mpi->control(enc->ctx, MPP_ENC_SET_HEADER_MODE, &header_mode);
        if (ret) {
            printf("MPP_ENC_SET_HEADER_MODE failed ret=%d\n", ret);
            return -1;
        }
    }

    ret = mpp_buffer_group_get_internal(&enc->buf_grp, MPP_BUFFER_TYPE_DRM);
    if (ret) {
        printf("mpp_buffer_group_get_internal failed ret=%d\n", ret);
        return -1;
    }

    ret = mpp_buffer_get(enc->buf_grp, &enc->frm_buf, enc->frame_size);
    if (ret) {
        printf("mpp_buffer_get frm_buf failed ret=%d\n", ret);
        return -1;
    }

    ret = mpp_buffer_get(enc->buf_grp, &enc->pkt_buf, enc->packet_size);
    if (ret) {
        printf("mpp_buffer_get pkt_buf failed ret=%d\n", ret);
        return -1;
    }

    return 0;
}

/*
 * 对外初始化入口。
 *
 * 这一层主要做“参数归一化”：
 * - 如果 stride 没传，就自动补默认对齐
 * - 如果 fps / bps / gop 没传，就给一套可跑的默认值
 *
 * 真正和 MPP 打交道的准备动作，还是交给 rk_mpp_encoder_prepare()。
 */
int rk_mpp_encoder_init(RkMppEncoder *enc,
                        MppCodingType type,
                        RK_U32 width,
                        RK_U32 height,
                        RK_U32 h_stride,
                        RK_U32 v_stride,
                        MppFrameFormat fmt,
                        RK_S32 fps,
                        RK_S32 bps,
                        RK_S32 gop,
                        FILE *f_out)
{
    if (!enc || !width || !height) {
        printf("rk_mpp_encoder_init invalid argument\n");
        return -1;
    }

    memset(enc, 0, sizeof(*enc));

    enc->type = type;
    enc->width = width;
    enc->height = height;
    enc->h_stride = h_stride ? h_stride : RKMPP_ALIGN(width, 16);
    enc->v_stride = v_stride ? v_stride : RKMPP_ALIGN(height, 16);
    enc->fmt = fmt;
    enc->fps = fps > 0 ? fps : 30;
    enc->bps = bps > 0 ? bps : (RK_S32)(width * height * enc->fps / 8);
    enc->gop = gop > 0 ? gop : enc->fps;
    enc->f_out = f_out;

    rk_mpp_encoder_calc_buffer_size(enc);
    return rk_mpp_encoder_prepare(enc);
}

/*
 * 注册业务层回调。
 *
 * 你后面如果想把编码后的 H.264/H.265 直接推流、发 RTP、写环形队列，
 * 一般就是从这个回调往外接。
 */
void rk_mpp_encoder_set_packet_callback(RkMppEncoder *enc,
                                        RkMppPacketCallback callback,
                                        void *userdata)
{
    if (!enc)
        return;

    enc->packet_callback = callback;
    enc->packet_callback_userdata = userdata;
}

/*
 * 主动取一份编码头。
 *
 * 对 H.264 来说，这里面通常会有 SPS/PPS。
 * 很多封装/推流场景在真正送图像前，都会先把这一份头拿出去。
 *
 * 你可以把它理解成：
 * “先把解码器/播放器认识这路码流所需要的自我介绍发出去”。
 */
int rk_mpp_encoder_write_header(RkMppEncoder *enc)
{
    MPP_RET ret = MPP_OK;

    if (!enc || !enc->ctx || !enc->mpi || !enc->pkt_buf) {
        printf("rk_mpp_encoder_write_header invalid encoder state\n");
        return -1;
    }

    ret = mpp_packet_init_with_buffer(&enc->packet, enc->pkt_buf);
    if (ret) {
        printf("mpp_packet_init_with_buffer failed ret=%d\n", ret);
        return -1;
    }

    mpp_packet_set_length(enc->packet, 0);

    ret = enc->mpi->control(enc->ctx, MPP_ENC_GET_HDR_SYNC, enc->packet);
    if (ret) {
        printf("MPP_ENC_GET_HDR_SYNC failed ret=%d\n", ret);
        mpp_packet_deinit(&enc->packet);
        return -1;
    }

    rk_mpp_encoder_emit_packet(enc, enc->packet, 1);
    mpp_packet_deinit(&enc->packet);
    return 0;
}

/*
 * 送一帧 dma-buf fd 对应的原始图像给编码器。
 *
 * 这条函数是编码主线最重要的一段，建议你按顺序记：
 *
 * 1. 把外部 dma-buf fd import 成 MppBuffer
 * 2. 创建一个 MppFrame 描述对象，并绑定这块外部 buffer
 * 3. 创建一个 MppPacket 作为输出容器
 * 4. 用 mpp_meta_set_packet() 告诉编码器：
 *    “这次输出请写到这个 packet 里”
 * 5. encode_put_frame() 送进编码器
 * 6. encode_get_packet() 取出压缩后的 H.264/H.265 码流
 * 7. 写文件 / 回调给外部
 *
 * 这就是 encoder 看起来和 decoder 不太一样的根本原因：
 * decoder 是送 packet 取 frame，
 * encoder 是送 frame 取 packet。
 */
int rk_mpp_encoder_send_frame(RkMppEncoder *enc, int fd, int eos)
{
    MPP_RET ret = MPP_OK;
    MppMeta meta = NULL;
    MppBuffer frm_buf = NULL;

    if (!enc || !enc->ctx || !enc->mpi) {
        printf("invalid encoder state\n");
        return -1;
    }

    if (fd < 0) {
        printf("rk_mpp_encoder_send_frame invalid fd=%d\n", fd);
        return -1;
    }

    MppBufferInfo info = {0};
    info.type = MPP_BUFFER_TYPE_EXT_DMA;
    info.fd = fd;
    info.size = enc->frame_size;

    ret = mpp_buffer_import(&frm_buf, &info);
    if (ret || !frm_buf) {
        printf("mpp_buffer_import failed ret=%d\n", ret);
        return -1;
    }

    ret = mpp_frame_init(&enc->frame);
    if (ret) {
        printf("mpp_frame_init failed ret=%d\n", ret);
        mpp_buffer_put(frm_buf);
        return -1;
    }

    mpp_frame_set_width(enc->frame, enc->width);
    mpp_frame_set_height(enc->frame, enc->height);
    mpp_frame_set_hor_stride(enc->frame, enc->h_stride);
    mpp_frame_set_ver_stride(enc->frame, enc->v_stride);
    mpp_frame_set_fmt(enc->frame, enc->fmt);
    mpp_frame_set_eos(enc->frame, eos ? 1 : 0);

    /* 关键：这里绑定外部 dmafd import 出来的 buffer */
    mpp_frame_set_buffer(enc->frame, frm_buf);

    meta = mpp_frame_get_meta(enc->frame);

    ret = mpp_packet_init_with_buffer(&enc->packet, enc->pkt_buf);
    if (ret) {
        printf("mpp_packet_init_with_buffer failed ret=%d\n", ret);
        mpp_frame_deinit(&enc->frame);
        mpp_buffer_put(frm_buf);
        return -1;
    }

    mpp_packet_set_length(enc->packet, 0);
    mpp_meta_set_packet(meta, KEY_OUTPUT_PACKET, enc->packet);

    ret = enc->mpi->encode_put_frame(enc->ctx, enc->frame);
    if (ret) {
        printf("encode_put_frame failed ret=%d\n", ret);
        mpp_packet_deinit(&enc->packet);
        mpp_frame_deinit(&enc->frame);
        mpp_buffer_put(frm_buf);
        return -1;
    }

    mpp_frame_deinit(&enc->frame);

    ret = enc->mpi->encode_get_packet(enc->ctx, &enc->packet);
    if (ret) {
        printf("encode_get_packet failed ret=%d\n", ret);
        mpp_packet_deinit(&enc->packet);
        mpp_buffer_put(frm_buf);
        return -1;
    }

    if (enc->packet) {
        rk_mpp_encoder_emit_packet(enc, enc->packet, 0);
        mpp_packet_deinit(&enc->packet);
    }

    mpp_buffer_put(frm_buf);

    if (eos)
        enc->eos_sent = 1;

    return 0;
}

/*
 * 资源释放顺序和 init 大致相反：
 * packet/frame -> cfg -> buffer -> ctx
 *
 * 记这个顺序的意义不是“必须一字不差”，而是帮你建立一个习惯：
 * 谁后创建，通常就更适合先释放。
 */
void rk_mpp_encoder_deinit(RkMppEncoder *enc)
{
    if (!enc)
        return;

    if (enc->packet)
        mpp_packet_deinit(&enc->packet);
    if (enc->frame)
        mpp_frame_deinit(&enc->frame);
    if (enc->enc_cfg)
        mpp_enc_cfg_deinit(enc->enc_cfg);
    if (enc->frm_buf)
        mpp_buffer_put(enc->frm_buf);
    if (enc->pkt_buf)
        mpp_buffer_put(enc->pkt_buf);
    if (enc->buf_grp)
        mpp_buffer_group_put(enc->buf_grp);
    if (enc->ctx)
        mpp_destroy(enc->ctx);
}
