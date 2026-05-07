#ifndef __RKMPP_DEC_H__
#define __RKMPP_DEC_H__

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "rk_mpi.h"
#include "mpp_buffer.h"
#include "mpp_err.h"
#include "mpp_frame.h"
#include "mpp_packet.h"
#include "rk_vdec_cfg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EXT_BUF_SIZE 32

typedef void (*RkMppFrameCallback)(const uint8_t *data,
                                   size_t size,
                                   int fd,
                                   RK_U32 width,
                                   RK_U32 height,
                                   RK_U32 h_stride,
                                   RK_U32 v_stride,
                                   RK_U32 fmt,
                                   RK_S64 pts_us,
                                   void *userdata);

typedef struct RkMppDecoder_t {
    MppCtx ctx;
    MppApi *mpi;
    MppPacket packet;
    MppDecCfg dec_cfg;
    MppBufferGroup frm_grp;
    FILE *f_out;
    int frame_count;
    int timeout_count;
    int eos_wait_count;
    int eos_sent;
    RkMppFrameCallback frame_callback;
    void *frame_callback_userdata;
    int ext_dma_fds[EXT_BUF_SIZE];
    unsigned char internal_buf[1024 * 1024];
} RkMppDecoder;

int rk_mpp_decoder_init(RkMppDecoder *dec, MppCodingType type, FILE *f_out);
void rk_mpp_decoder_set_frame_callback(RkMppDecoder *dec,
                                       RkMppFrameCallback callback,
                                       void *userdata);
int rk_mpp_decoder_send_data_with_pts(RkMppDecoder *dec,
                                      uint8_t *data,
                                      size_t len,
                                      int eos,
                                      RK_S64 pts_us);
int rk_mpp_decoder_send_data(RkMppDecoder *dec, uint8_t *data, size_t len, int eos);
void rk_mpp_decoder_deinit(RkMppDecoder *dec);

#ifdef __cplusplus
}
#endif

#endif  // __RKMPP_DEC_H__
