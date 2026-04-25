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
#define BUF_SIZE (1024 * 1024)

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
                                   void *userdata);

typedef struct RkMppDecoder_t {
    MppCtx ctx;
    MppApi *mpi;
    MppPacket packet;
    MppDecCfg dec_cfg;
    MppFrame frame;
    MppBufferGroup frm_grp;
    FILE *f_out;
    int frame_count;
    int timeout_count;
    int eos_wait_count;
    int eos_sent;
    RkMppFrameCallback frame_callback;
    void *frame_callback_userdata;
    int ext_dma_fds[EXT_BUF_SIZE];
    unsigned char internal_buf[BUF_SIZE];
} RkMppDecoder;

const char *get_mpp_frame_fmt_name(RK_U32 fmt);
void dump_frame_nv12(MppFrame frame, FILE *fp_out);
int rk_mpp_decoder_init(RkMppDecoder *dec, MppCodingType type, FILE *f_out);
void rk_mpp_decoder_set_frame_callback(RkMppDecoder *dec,
                                       RkMppFrameCallback callback,
                                       void *userdata);
int rk_mpp_decoder_handle_info_change(RkMppDecoder *dec, MppFrame frame);
int rk_mpp_decoder_handle_frame(RkMppDecoder *dec, MppFrame frame);
int rk_mpp_decoder_poll_frames(RkMppDecoder *dec);
int rk_mpp_decoder_send_data(RkMppDecoder *dec, uint8_t *data, size_t len, int eos);
int rk_mpp_decoder_run_file(RkMppDecoder *dec, FILE *f_in);
void rk_mpp_decoder_deinit(RkMppDecoder *dec);
void decode(const char *input, const char *output, MppCodingType type);

#ifdef __cplusplus
}
#endif

#endif  // __RKMPP_DEC_H__
