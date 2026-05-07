#ifndef __RKMPP_ENC_H__
#define __RKMPP_ENC_H__

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "mpp_buffer.h"
#include "mpp_frame.h"
#include "mpp_packet.h"
#include "rk_mpi.h"
#include "rk_venc_cfg.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*RkMppPacketCallback)(const uint8_t *data,
                                    size_t size,
                                    /* 1 表示编码头，例如 H.264 的 SPS/PPS */
                                    int is_header,
                                    /* 1 表示编码器认为这路码流已经到达 eos */
                                    int eos,
                                    void *userdata);

typedef struct RkMppEncoder_t {
    /* MPP 编码器本体 */
    MppCtx ctx;
    MppApi *mpi;
    MppEncCfg enc_cfg;
    MppFrame frame;
    MppPacket packet;

    /* 编码输入/输出使用的 buffer */
    MppBufferGroup buf_grp;
    MppBuffer frm_buf;
    MppBuffer pkt_buf;

    /* 可选文件输出，保留是为了方便本地 dump 验证 */
    FILE *f_out;

    /* 运行时状态 */
    int frame_count;
    int eos_sent;
    int pkt_eos;

    /* 编码完成回调 */
    RkMppPacketCallback packet_callback;
    void *packet_callback_userdata;

    /* 图像和码率参数 */
    RK_U32 width;
    RK_U32 height;
    RK_U32 h_stride;
    RK_U32 v_stride;
    MppFrameFormat fmt;
    MppCodingType type;
    RK_S32 fps;
    RK_S32 bps;
    RK_S32 gop;
    size_t frame_size;
    size_t packet_size;
} RkMppEncoder;

/*
 * 初始化一套编码器实例。
 *
 * 说明：
 * 1. h_stride / v_stride 传 0 时会自动按 16 对齐。
 * 2. bps / gop 传 0 时会自动给默认值。
 * 3. f_out 可以为 NULL；如果不需要落文件，只走回调就行。
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
                        FILE *f_out);

/* 注册编码输出回调，编码头和普通码流都会从这里抛出来。 */
void rk_mpp_encoder_set_packet_callback(RkMppEncoder *enc,
                                        RkMppPacketCallback callback,
                                        void *userdata);

/* 主动取一次编码头，例如 H.264 的 SPS/PPS。通常初始化后先调一次。 */
int rk_mpp_encoder_write_header(RkMppEncoder *enc);

/*
 * 送一帧 dma-buf fd 对应的原始图像给编码器。
 *
 * 当前版本按初始化时设置的 fmt / stride 解释这块 dma-buf，
 * 所以调用方需要保证 fd 对应的图像布局和 enc 的配置一致。
 */
int rk_mpp_encoder_send_frame(RkMppEncoder *enc,
                              int fd,
                              int eos);
void rk_mpp_encoder_deinit(RkMppEncoder *enc);

#ifdef __cplusplus
}
#endif

#endif  // __RKMPP_ENC_H__