#ifndef __CAM_H__
#define __CAM_H__

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 这个回调把“摄像头刚取到的一帧”交给上层。
 *
 * 参数含义：
 * - fd: 这一帧输入数据所在的 dma-buf fd
 * - index: 这帧来自哪一个 V4L2 buffer 槽位
 * - w / h: 当前流的宽高
 * - stride: 对未压缩格式通常有意义；对 MJPEG 这种压缩流常常为 0
 * - size: 当前帧真实有效数据长度；对 MJPEG 来说它对应 bytesused
 * - userdata: 注册回调时传进来的上层私有数据
 */
typedef void (*cam_frame_callback_t)(int fd,
                                     int index,
                                     int w,
                                     int h,
                                     int stride,
                                     unsigned int size,
                                     void *userdata);

// ===================== 宏定义 =====================
// V4L2 摄像头设备节点
// 摄像头缓存帧数量（4缓冲）
#define FRAMEBUFFER_COUNT  4

// ===================== 外部函数声明 =====================
/**
 * @brief  摄像头模块初始化总入口（上层直接调用这个）
 * @note   内部会依次执行：打开设备->设置格式->分配DMA缓存->队列缓存->启动流
 *         上层不需要再单独调用 alloc_dmabuf_fds/v4l2_init_buffer/v4l2_stream_on。
 */
void camera_init(void);

/**
 * @brief  摄像头采集主循环（阻塞运行，不断取帧送显示）
 */
void camera_run(void);

/**
 * @brief  关闭摄像头并释放内部申请的 V4L2 队列和 dma-buf fd
 */
void camera_close(void);

/**
 * @brief  兼容旧调用的 DMA-BUF 分配入口；新代码不需要主动调用。
 * @note   heap_path 已不再使用，内部改为和解码器一致的 DRM dumb buffer + PRIME fd。
 */
void alloc_dmabuf_fds(const char *heap_path, int count, size_t size);

/**
 * @brief  V4L2设备基础初始化（打开设备、查询能力、枚举格式）
 */
void v4l2_dev_init(void);

/**
 * @brief  设置摄像头分辨率、格式(YUYV)、帧率
 * @return 0成功，负数失败
 */
int v4l2_set_format(void);

/**
 * @brief  V4L2 DMABUF模式缓冲区初始化（申请、入队）
 * @return 0成功，负数失败
 */
int v4l2_init_buffer(void);

/**
 * @brief  启动摄像头数据流采集
 * @return 0成功，负数失败
 */
int v4l2_stream_on(void);

/*
 * 注册“取到一帧后，上层要怎么处理它”。
 * 当前工程里，上层给它注册的是：把 MJPEG 交给 MPP 解码。
 */
void cam_register_frame_callback(cam_frame_callback_t callback, void *userdata);

void run(void);
int cam_get_width(void);
int cam_get_height(void);
int cam_get_bytesperline(void);
unsigned int cam_get_sizeimage(void);

// // ===================== 外部全局变量声明 =====================
// // 摄像头输出图像宽度(默认640)
// extern int width;
// // 摄像头输出图像高度(默认480)
// extern int height;
// // 一帧图像大小
// extern unsigned int sizeimage;
// // V4L2设备文件描述符
// extern int v4l2_fd;
// // 摄像头DMA-BUF文件描述符数组
extern int dmafd[FRAMEBUFFER_COUNT];

#ifdef __cplusplus
}
#endif

#endif
