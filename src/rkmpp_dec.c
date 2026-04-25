#include "rkmpp_dec.h"
#include <errno.h>
#include <fcntl.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "ffmpeg_pull_rtsp.h"

/*
 * 这是一个“尽量容易看懂”的单线程 MPP 解码示例。
 *
 * 这次重构的重点是把“输入源”和“解码器核心”拆开：
 *
 * 1. rk_mpp_decoder_init()
 *    只负责初始化解码器。
 *
 * 2. rk_mpp_decoder_send_data()
 *    外部传入一块压缩码流 data + len + eos。
 *    这意味着以后可以很自然地换成：
 *    - 文件 fread
 *    - 网络拉流回调
 *    - 环形队列
 *    - socket 接收缓存
 *
 * 3. rk_mpp_decoder_poll_frames()
 *    尽量把当前已经可取出的 frame 全部取出来。
 *
 * 4. rk_mpp_decoder_run_file()
 *    这里只是一个“文件输入包装层”，方便继续用文件做验证。
 */

#define BUF_SIZE (1024 * 1024)
#define EOS_WAIT_TIMEOUT_COUNT 2000

static int rk_drm_ioctl_retry(int fd, unsigned long request, void *arg)
{
    int ret;

    do {
        ret = ioctl(fd, request, arg);
    } while (ret == -1 && (errno == EINTR || errno == EAGAIN));

    return ret;
}

static int rk_drm_create_dmabuf_fd(size_t size)
{
    const char *drm_device = getenv("DRM_DEVICE");
    const char *device_path = (drm_device && drm_device[0]) ? drm_device : "/dev/dri/card0";
    struct drm_mode_create_dumb create_req;
    struct drm_prime_handle prime_req;
    int drm_fd = -1;
    int dma_fd = -1;

    if (size == 0) {
        printf("rk_drm_create_dmabuf_fd invalid size=%zu\n", size);
        return -1;
    }

    drm_fd = open(device_path, O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        perror("open drm device");
        return -1;
    }

    memset(&create_req, 0, sizeof(create_req));
    create_req.bpp = 8;
    create_req.width = 4096;
    create_req.height = (uint32_t)((size + create_req.width - 1) / create_req.width);

    if (rk_drm_ioctl_retry(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_req) < 0) {
        perror("DRM_IOCTL_MODE_CREATE_DUMB");
        close(drm_fd);
        return -1;
    }

    memset(&prime_req, 0, sizeof(prime_req));
    prime_req.handle = create_req.handle;
    prime_req.flags = DRM_CLOEXEC | DRM_RDWR;
    prime_req.fd = -1;

    if (rk_drm_ioctl_retry(drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime_req) < 0) {
        struct drm_mode_destroy_dumb destroy_req;

        perror("DRM_IOCTL_PRIME_HANDLE_TO_FD");
        memset(&destroy_req, 0, sizeof(destroy_req));
        destroy_req.handle = create_req.handle;
        rk_drm_ioctl_retry(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);
        close(drm_fd);
        return -1;
    }

    dma_fd = prime_req.fd;

    close(drm_fd);
    printf("rk_drm_create_dmabuf_fd size=%zu width=%u height=%u pitch=%u alloc_size=%llu fd=%d dev=%s\n",
           size,
           create_req.width,
           create_req.height,
           create_req.pitch,
           (unsigned long long)create_req.size,
           dma_fd,
           device_path);
    return dma_fd;
}

static void rk_mpp_decoder_release_ext_dma_fds(RkMppDecoder *dec)
{
    int i;

    if (!dec)
        return;

    for (i = 0; i < EXT_BUF_SIZE; ++i) {
        if (dec->ext_dma_fds[i] >= 0) {
            close(dec->ext_dma_fds[i]);
            dec->ext_dma_fds[i] = -1;
        }
    }
}

static void msleep(unsigned int ms)
{
    usleep(ms * 1000);
}

const char *get_mpp_frame_fmt_name(RK_U32 fmt)
{
    switch (fmt) {
    case MPP_FMT_YUV420SP:
        return "MPP_FMT_YUV420SP / NV12";
    case MPP_FMT_YUV420SP_VU:
        return "MPP_FMT_YUV420SP_VU / NV21";
    case MPP_FMT_YUV420P:
        return "MPP_FMT_YUV420P / I420";
    case MPP_FMT_YUV422SP:
        return "MPP_FMT_YUV422SP / NV16";
    case MPP_FMT_YUV422SP_VU:
        return "MPP_FMT_YUV422SP_VU / NV61";
    case MPP_FMT_YUV422P:
        return "MPP_FMT_YUV422P";
    case MPP_FMT_YUV422_YUYV:
        return "MPP_FMT_YUV422_YUYV / YUY2";
    case MPP_FMT_YUV422_YVYU:
        return "MPP_FMT_YUV422_YVYU";
    case MPP_FMT_YUV422_UYVY:
        return "MPP_FMT_YUV422_UYVY";
    case MPP_FMT_YUV422_VYUY:
        return "MPP_FMT_YUV422_VYUY";
    case MPP_FMT_YUV400:
        return "MPP_FMT_YUV400";
    case MPP_FMT_YUV440SP:
        return "MPP_FMT_YUV440SP";
    case MPP_FMT_YUV411SP:
        return "MPP_FMT_YUV411SP";
    case MPP_FMT_YUV444SP:
        return "MPP_FMT_YUV444SP";
    case MPP_FMT_YUV444P:
        return "MPP_FMT_YUV444P";
    case MPP_FMT_YUV420SP_10BIT:
        return "MPP_FMT_YUV420SP_10BIT";
    case MPP_FMT_YUV422SP_10BIT:
        return "MPP_FMT_YUV422SP_10BIT";
    case MPP_FMT_RGB565:
        return "MPP_FMT_RGB565";
    case MPP_FMT_BGR565:
        return "MPP_FMT_BGR565";
    case MPP_FMT_RGB555:
        return "MPP_FMT_RGB555";
    case MPP_FMT_BGR555:
        return "MPP_FMT_BGR555";
    case MPP_FMT_RGB444:
        return "MPP_FMT_RGB444";
    case MPP_FMT_BGR444:
        return "MPP_FMT_BGR444";
    case MPP_FMT_RGB888:
        return "MPP_FMT_RGB888";
    case MPP_FMT_BGR888:
        return "MPP_FMT_BGR888";
    case MPP_FMT_RGB101010:
        return "MPP_FMT_RGB101010";
    case MPP_FMT_BGR101010:
        return "MPP_FMT_BGR101010";
    case MPP_FMT_ARGB8888:
        return "MPP_FMT_ARGB8888";
    case MPP_FMT_ABGR8888:
        return "MPP_FMT_ABGR8888";
    case MPP_FMT_BGRA8888:
        return "MPP_FMT_BGRA8888";
    case MPP_FMT_RGBA8888:
        return "MPP_FMT_RGBA8888";
    default:
        return "UNKNOWN_MPP_FMT";
    }
}

/*
 * 把解码出的 frame 按 NV12 文件格式写到磁盘。
 */
void dump_frame_nv12(MppFrame frame, FILE *fp_out)
{
    MppBuffer buffer = NULL;
    MppFrameFormat fmt;
    RK_U32 width;
    RK_U32 height;
    RK_U32 h_stride;
    RK_U32 v_stride;
    RK_U8 *base = NULL;
    RK_U8 *ptr_y = NULL;
    RK_U8 *ptr_uv = NULL;
    RK_U32 y = 0;

    if (!frame || !fp_out)
        return;

    fmt = mpp_frame_get_fmt(frame);
    if (fmt != MPP_FMT_YUV420SP) {
        printf("warning: current frame fmt=%d, not NV12(MPP_FMT_YUV420SP)\n",
               fmt);
        return;
    }

    buffer = mpp_frame_get_buffer(frame);
    if (!buffer)
        return;

    width = mpp_frame_get_width(frame);
    height = mpp_frame_get_height(frame);
    h_stride = mpp_frame_get_hor_stride(frame);
    v_stride = mpp_frame_get_ver_stride(frame);
    base = (RK_U8 *)mpp_buffer_get_ptr(buffer);
    if (!base)
        return;

    ptr_y = base;
    ptr_uv = base + h_stride * v_stride;

    for (y = 0; y < height; y++)
        fwrite(ptr_y + y * h_stride, 1, width, fp_out);

    for (y = 0; y < height / 2; y++)
        fwrite(ptr_uv + y * h_stride, 1, width, fp_out);
}


/*
 * 初始化解码器核心对象。
 *
 * 这一层不关心输入来自哪里，只关心“我是不是一个准备好的解码器”。
 */
int rk_mpp_decoder_init(RkMppDecoder *dec, MppCodingType type, FILE *f_out)
{
    int i;

    memset(dec, 0, sizeof(*dec));
    dec->f_out = f_out;
    for (i = 0; i < EXT_BUF_SIZE; ++i)
        dec->ext_dma_fds[i] = -1;

    if (mpp_create(&dec->ctx, &dec->mpi) != MPP_OK) {
        printf("mpp_create error\n");
        return -1;
    }

    if (mpp_init(dec->ctx, MPP_CTX_DEC, type) != MPP_OK) {
        printf("mpp_init error\n");
        return -1;
    }

    if (mpp_packet_init(&dec->packet, NULL, 0) != MPP_OK) {
        printf("mpp_packet_init error\n");
        return -1;
    }

    if (mpp_dec_cfg_init(&dec->dec_cfg) != MPP_OK) {
        printf("mpp_dec_cfg_init error\n");
        return -1;
    }

    if (dec->mpi->control(dec->ctx, MPP_DEC_GET_CFG, dec->dec_cfg) != MPP_OK) {
        printf("MPP_DEC_GET_CFG error\n");
        return -1;
    }

    if (mpp_dec_cfg_set_u32(dec->dec_cfg, "base:split_parse", 1) != MPP_OK) {
        printf("mpp_dec_cfg_set_u32 error\n");
        return -1;
    }

    if (dec->mpi->control(dec->ctx, MPP_DEC_SET_CFG, dec->dec_cfg) != MPP_OK) {
        printf("MPP_DEC_SET_CFG error\n");
        return -1;
    }

    return 0;
}

void rk_mpp_decoder_set_frame_callback(RkMppDecoder *dec,
                                       RkMppFrameCallback callback,
                                       void *userdata)
{
    if (!dec)
        return;

    dec->frame_callback = callback;
    dec->frame_callback_userdata = userdata;
}

/*
 * 处理 info_change。
 *
 * 当 MPP 返回的 frame 带有 info_change 标志时，说明：
 * 1. 解码器已经知道输出宽高/stride/buf_size
 * 2. 但它还在等应用层准备输出缓冲
 */
int rk_mpp_decoder_handle_info_change(RkMppDecoder *dec, MppFrame frame)
{
    MPP_RET ret;
    RK_U32 width = mpp_frame_get_width(frame);
    RK_U32 height = mpp_frame_get_height(frame);
    RK_U32 h_stride = mpp_frame_get_hor_stride(frame);
    RK_U32 v_stride = mpp_frame_get_ver_stride(frame);
    RK_U32 buf_size = mpp_frame_get_buf_size(frame);
    
    printf("info_change: w=%u h=%u hs=%u vs=%u buf=%u\n",
           width, height, h_stride, v_stride, buf_size);
    


    {
        size_t required_size = (size_t)h_stride * v_stride * 2;
        int i;

        if (required_size < buf_size)
            required_size = buf_size;

        if (!dec->frm_grp) {
        ret = mpp_buffer_group_get_external(&dec->frm_grp, MPP_BUFFER_TYPE_EXT_DMA);
        if (ret) {
            printf("mpp_buffer_group_get_external failed ret=%d\n", ret);
            return -1;
        }
        } else {
            ret = mpp_buffer_group_clear(dec->frm_grp);
            if (ret) {
                printf("mpp_buffer_group_clear failed ret=%d\n", ret);
                return -1;
            }
        }

        rk_mpp_decoder_release_ext_dma_fds(dec);

        for (i = 0; i < EXT_BUF_SIZE; ++i) {
            MppBufferInfo info;

            dec->ext_dma_fds[i] = rk_drm_create_dmabuf_fd(required_size);
            if (dec->ext_dma_fds[i] < 0) {
                printf("rk_drm_create_dmabuf_fd failed index=%d size=%zu\n",
                       i, required_size);
                rk_mpp_decoder_release_ext_dma_fds(dec);
                return -1;
            }

            memset(&info, 0, sizeof(info));
            info.type = MPP_BUFFER_TYPE_EXT_DMA;
            info.fd = dec->ext_dma_fds[i];
            info.size = required_size;
            info.index = i;

            ret = mpp_buffer_commit(dec->frm_grp, &info);
            if (ret) {
                printf("mpp_buffer_commit failed index=%d ret=%d fd=%d size=%zu\n",
                       i, ret, info.fd, info.size);
                rk_mpp_decoder_release_ext_dma_fds(dec);
                return -1;
            }
        }

        ret = dec->mpi->control(dec->ctx, MPP_DEC_SET_EXT_BUF_GROUP, dec->frm_grp);
        if (ret) {
            printf("MPP_DEC_SET_EXT_BUF_GROUP failed ret=%d\n", ret);
            rk_mpp_decoder_release_ext_dma_fds(dec);
            return -1;
        }
    }

    // if (!dec->frm_grp) {
    //     ret = mpp_buffer_group_get_internal(&dec->frm_grp, MPP_BUFFER_TYPE_ION);
    //     if (ret) {
    //         printf("mpp_buffer_group_get_internal failed ret=%d\n", ret);
    //         return -1;
    //     }

    //     ret = dec->mpi->control(dec->ctx, MPP_DEC_SET_EXT_BUF_GROUP, dec->frm_grp);
    //     if (ret) {
    //         printf("MPP_DEC_SET_EXT_BUF_GROUP failed ret=%d\n", ret);
    //         return -1;
    //     }
    // } else {
    //     ret = mpp_buffer_group_clear(dec->frm_grp);
    //     if (ret) {
    //         printf("mpp_buffer_group_clear failed ret=%d\n", ret);
    //         return -1;
    //     }
    // }

    // ret = mpp_buffer_group_limit_config(dec->frm_grp, buf_size, );
    // if (ret) {
    //     printf("mpp_buffer_group_limit_config failed ret=%d\n", ret);
    //     return -1;
    // }

    ret = dec->mpi->control(dec->ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
    if (ret) {
        printf("MPP_DEC_SET_INFO_CHANGE_READY failed ret=%d\n", ret);
        return -1;
    }

    return 0;
}

/*
 * 处理一帧输出。
 *
 * 如果是 info_change，就走缓冲准备流程；
 * 如果是正常图像帧，就统计并按 NV12 写文件。
 */
int rk_mpp_decoder_handle_frame(RkMppDecoder *dec, MppFrame frame)
{
    RK_U32 fmt = mpp_frame_get_fmt(frame);
    RK_U32 width = mpp_frame_get_width(frame);
    RK_U32 height = mpp_frame_get_height(frame);
    RK_U32 h_stride = mpp_frame_get_hor_stride(frame);
    RK_U32 v_stride = mpp_frame_get_ver_stride(frame);
    MppBuffer buf = mpp_frame_get_buffer(frame);
    int fd = buf ? mpp_buffer_get_fd(buf) : -1;
    printf("frame=%p fmt=%u(%s) info_change=%d eos=%d\n",
           frame, fmt, get_mpp_frame_fmt_name(fmt),
           mpp_frame_get_info_change(frame), mpp_frame_get_eos(frame));

    if (mpp_frame_get_info_change(frame))
        return rk_mpp_decoder_handle_info_change(dec, frame);

    printf("成功读取到一帧数据 %d\n", ++dec->frame_count);
    if (dec->f_out && !mpp_frame_get_errinfo(frame))
        dump_frame_nv12(frame, dec->f_out);

    if (dec->frame_callback && !mpp_frame_get_errinfo(frame)) {
        MppBuffer buffer = mpp_frame_get_buffer(frame);
        const uint8_t *data = NULL;
        size_t size = 0;

        if (buffer) {
            data = (const uint8_t *)mpp_buffer_get_ptr(buffer);
            size = mpp_frame_get_buf_size(frame);
        }

        dec->frame_callback(data, size, fd, width, height, h_stride, v_stride, fmt,
                            dec->frame_callback_userdata);
    }

    return 0;
}

/*
 * 尽量把当前已经能取出来的 frame 全部取出来。
 *
 * 返回值约定：
 * 0  : 当前轮正常结束，可以继续送更多输入
 * 1  : 收到真正的 eos frame，整个解码完成
 * <0 : 出错
 */
int rk_mpp_decoder_poll_frames(RkMppDecoder *dec)
{
    while (1) {
        MPP_RET ret;

        dec->frame = NULL;
        ret = dec->mpi->decode_get_frame(dec->ctx, &dec->frame);
        printf("调用 decode_get_frame ret=%d\n", ret);
        if (ret == MPP_ERR_TIMEOUT) {
            dec->timeout_count++;
            if (dec->eos_sent)
                dec->eos_wait_count++;
            msleep(1);
            return 0;
        }

        if (ret != MPP_OK) {
            printf("mpi->decode_get_frame error ret=%d\n", ret);
            return -1;
        }

        dec->timeout_count = 0;
        dec->eos_wait_count = 0;

        if (!dec->frame) {
            printf("空\n");
            return 0;
        }

        if (rk_mpp_decoder_handle_frame(dec, dec->frame)) {
            mpp_frame_deinit(&dec->frame);
            return -1;
        }

        if (mpp_frame_get_eos(dec->frame)) {
            printf("got eos frame, decode finished\n");
            mpp_frame_deinit(&dec->frame);
            return 1;
        }
        
        mpp_frame_deinit(&dec->frame);
    }

}

/*
 * 对外暴露的“喂一块压缩码流”的函数。
 *
 * 这一步就是从“文件 demo”过渡到“可接其他输入源”的关键。
 * 以后无论数据来自哪里，只要最后能给你：
 *   data / len / eos
 * 基本都可以接到这里。
 */
int rk_mpp_decoder_send_data(RkMppDecoder *dec,
                             uint8_t *data,
                             size_t len,
                             int eos)
{
    if (!dec) {
        printf("decoder is null\n");
        return -1;
    }

    if (len > sizeof(dec->internal_buf)) {
        printf("input frame too large len=%zu buf=%zu\n",
               len, sizeof(dec->internal_buf));
        return -1;
    }

    if (len > 0 && !data) {
        printf("input data is null while len=%zu\n", len);
        return -1;
    }

    memset(dec->internal_buf, 0, sizeof(dec->internal_buf));
    if (len > 0)
        memcpy(dec->internal_buf, data, len);
    int pkt_done = 0;

    mpp_packet_set_data(dec->packet, dec->internal_buf);
    mpp_packet_set_pos(dec->packet, dec->internal_buf);
    mpp_packet_set_size(dec->packet, len);
    mpp_packet_set_length(dec->packet, len);
    mpp_packet_clr_eos(dec->packet);

    if (eos)
        mpp_packet_set_eos(dec->packet);
    printf("pkt_done=%d\n", pkt_done);
    while (!pkt_done) {
        MPP_RET ret = dec->mpi->decode_put_packet(dec->ctx, dec->packet);
        if (ret == MPP_OK) {
            pkt_done = 1;
            if (eos)
                dec->eos_sent = 1;
            printf("packet中的数据送往解码器成功 len=%zu eos=%d\n", len, eos);
        } else {
            if (ret == MPP_ERR_BUFFER_FULL) { // MPP_ERR_BUFFER_FULL
                
                printf("解码器内部缓冲满了，无法接受新数据了\n");
                
                ret = rk_mpp_decoder_poll_frames(dec);
                if (ret < 0) {
                    printf("解码器处理已解码帧时出错 ret=%d\n", ret);
                    return -1;
                }

            } else {
                printf("packet送往解码器失败 ret=%d\n", ret);
            }
            
            msleep(1);
        }
    }

    return rk_mpp_decoder_poll_frames(dec);
}

/*
 * 文件输入版本只是一个“外层包装”。
 *
 * 这个函数的职责非常单纯：
 * 1. fread 一块码流
 * 2. 调用 rk_mpp_decoder_send_data()
 * 3. 文件结束后进入 drain
 */
int rk_mpp_decoder_run_file(RkMppDecoder *dec, FILE *f_in)
{
    uint8_t buf[BUF_SIZE] = {0};
    size_t read_size = 0;
    size_t total_read = 0;

    while (1) {
        int ret;
        int eos = 0;

        read_size = fread(buf, 1, BUF_SIZE, f_in);
        total_read += read_size;

        if (read_size == 0) {
            eos = 1;
            printf("读到码流文件末尾, total_read=%zu\n", total_read);
        } else {
            printf("从码流文件中读取到数据 size:%zu total:%zu\n",
                   read_size, total_read);
        }

        ret = rk_mpp_decoder_send_data(dec, buf, read_size, eos);
        if (ret < 0)
            return -1;
        if (ret > 0)
            return 0;

        if (eos) {
            while (1) {
                ret = rk_mpp_decoder_poll_frames(dec);
                if (ret < 0)
                    return -1;
                if (ret > 0)
                    return 0;

                if (dec->eos_wait_count > EOS_WAIT_TIMEOUT_COUNT) {
                    printf("wait eos frame timeout after %d frames, exit normally\n",
                           dec->frame_count);
                    return 0;
                }
                msleep(1);
            }
        }
    }
}

void rk_mpp_decoder_deinit(RkMppDecoder *dec)
{
    if (dec->packet)
        mpp_packet_deinit(&dec->packet);
    if (dec->frame)
        mpp_frame_deinit(&dec->frame);
    if (dec->dec_cfg)
        mpp_dec_cfg_deinit(dec->dec_cfg);
    if (dec->ctx)
        mpp_destroy(dec->ctx);
    if (dec->frm_grp)
        mpp_buffer_group_put(dec->frm_grp);
    rk_mpp_decoder_release_ext_dma_fds(dec);
}

void decode(const char *input, const char *output, MppCodingType type)
{
    RkMppDecoder dec;
    FILE *f_in = NULL;
    FILE *f_out = NULL;

    f_in = fopen(input, "rb");
    if (f_in == NULL) {
        perror("fopen");
        return;
    }

    if (output) {
        f_out = fopen(output, "wb");
        if (f_out == NULL) {
            perror("fopen output");
            fclose(f_in);
            return;
        }
    }

    if (rk_mpp_decoder_init(&dec, type, f_out)) {
        rk_mpp_decoder_deinit(&dec);
        if (f_in)
            fclose(f_in);
        if (f_out)
            fclose(f_out);
        return;
    }

    rk_mpp_decoder_run_file(&dec, f_in);

    rk_mpp_decoder_deinit(&dec);
    if (f_in)
        fclose(f_in);
    if (f_out)
        fclose(f_out);
}
