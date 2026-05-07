

#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <linux/input.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <assert.h>
#include <linux/videodev2.h>
#include <sys/mman.h>
#include <stdint.h>
#include <poll.h>
#include <cam.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <linux/dma-buf.h>
#include "drm_test.h"
#define V4L2_DEV_PATH "/dev/video28"
#define FRAMEBUFFER_COUNT 4
static unsigned int sizeimage;
static int bytesperline;
static int lcd_width;
static int lcd_height;
static int lcd_fd = -1;
static unsigned short *screen_base;
static int width = 640;
static int height = 480;
static int v4l2_fd = -1;
int dmafd[FRAMEBUFFER_COUNT] = {-1, -1, -1, -1};
static int dmafd_count;
static cam_frame_callback_t g_frame_callback = NULL;
static void *g_frame_callback_userdata = NULL;
static int request_stop = 0;
struct cam_buf_info
{
    unsigned long length;
    unsigned char *start;
};


static struct cam_buf_info buf_infos[FRAMEBUFFER_COUNT];

typedef struct camera_format
{

    char description[32];     // 字符串描述信息
    unsigned int pixelformat; // 像素格式

} cam_fmt;

static int cam_drm_ioctl_retry(int fd, unsigned long request, void *arg)
{
    int ret;

    do {
        ret = ioctl(fd, request, arg);
    } while (ret == -1 && (errno == EINTR || errno == EAGAIN));

    return ret;
}

static int cam_drm_create_dmabuf_fd(size_t size)
{
    const char *drm_device = getenv("DRM_DEVICE");
    const char *device_path = (drm_device && drm_device[0]) ? drm_device : "/dev/dri/card0";
    struct drm_mode_create_dumb create_req;
    struct drm_prime_handle prime_req;
    struct drm_mode_destroy_dumb destroy_req;
    int drm_fd = -1;
    int dma_fd = -1;

    if (size == 0) {
        printf("cam_drm_create_dmabuf_fd invalid size=%zu\n", size);
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

    if (cam_drm_ioctl_retry(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_req) < 0) {
        perror("DRM_IOCTL_MODE_CREATE_DUMB");
        close(drm_fd);
        return -1;
    }

    memset(&prime_req, 0, sizeof(prime_req));
    prime_req.handle = create_req.handle;
    prime_req.flags = DRM_CLOEXEC | DRM_RDWR;
    prime_req.fd = -1;

    if (cam_drm_ioctl_retry(drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime_req) < 0) {
        perror("DRM_IOCTL_PRIME_HANDLE_TO_FD");
        memset(&destroy_req, 0, sizeof(destroy_req));
        destroy_req.handle = create_req.handle;
        cam_drm_ioctl_retry(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);
        close(drm_fd);
        return -1;
    }

    dma_fd = prime_req.fd;

    memset(&destroy_req, 0, sizeof(destroy_req));
    destroy_req.handle = create_req.handle;
    cam_drm_ioctl_retry(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);
    close(drm_fd);

    printf("cam_drm_create_dmabuf_fd size=%zu width=%u height=%u pitch=%u alloc_size=%llu fd=%d dev=%s\n",
           size,
           create_req.width,
           create_req.height,
           create_req.pitch,
           (unsigned long long)create_req.size,
           dma_fd,
           device_path);
    return dma_fd;
}

static void cam_release_dmabuf_fds(void)
{
    int i;

    for (i = 0; i < FRAMEBUFFER_COUNT; ++i) {
        if (dmafd[i] >= 0) {
            close(dmafd[i]);
            dmafd[i] = -1;
        }
    }

    dmafd_count = 0;
}

static int cam_alloc_dmabuf_fds(int count, size_t size)
{
    int i;

    if (count <= 0 || count > FRAMEBUFFER_COUNT || size == 0) {
        printf("cam_alloc_dmabuf_fds invalid count=%d size=%zu\n", count, size);
        return -1;
    }

    cam_release_dmabuf_fds();

    for (i = 0; i < count; ++i) {
        dmafd[i] = cam_drm_create_dmabuf_fd(size);
        if (dmafd[i] < 0) {
            printf("cam_drm_create_dmabuf_fd failed index=%d size=%zu\n", i, size);
            cam_release_dmabuf_fds();
            return -1;
        }

        printf("camera dmafd[%d]=%d\n", i, dmafd[i]);
    }

    dmafd_count = count;
    return 0;
}

void alloc_dmabuf_fds(const char *heap_path, int count, size_t size)
{
    (void)heap_path;

    if (cam_alloc_dmabuf_fds(count, size) != 0) {
        printf("alloc_dmabuf_fds failed count=%d size=%zu\n", count, size);
    }
}

/*** 初始化摄像头 ***/
void v4l2_dev_init(void)
{
    struct v4l2_capability cap = {0};
    printf("正在初始化v4l2设备...\n");

    /* 打开摄像头 */
    v4l2_fd = open(V4L2_DEV_PATH, O_RDWR);
    if (v4l2_fd < 0)
    {
        perror("open v4l2_dev error");
        exit(-1);
    }
    /*查询设备功能*/
    if (ioctl(v4l2_fd, VIDIOC_QUERYCAP, &cap) < 0)
    {
        perror("ioctl VIDIOC_QUERYCAP error");
        close(v4l2_fd);
        exit(-1);
    }

    /*判断是否为视频采集设备*/
    if (!(V4L2_CAP_VIDEO_CAPTURE & cap.capabilities))
    {
        // 如果不是视频采集设备 !(V4L2_CAP_VIDEO_CAPTURE & cap.capabilities) 值为非0
        printf("非视频采集设备\n");
        close(v4l2_fd);
        exit(-1);
    }

    if (cap.capabilities & V4L2_CAP_STREAMING)
    {
        printf("支持流式io可能支持dmabuf\n");
    }

    // struct v4l2_requestbuffers req = {0};
    // req.count = 4;
    // req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    // req.memory = V4L2_MEMORY_DMABUF;

    // if (ioctl(v4l2_fd, VIDIOC_REQBUFS, &req) < 0) {
    //     perror("REQBUFS DMABUF");
    // }
    /*查询采集设备支持的所有像素格式及描述信息*/
    struct v4l2_fmtdesc fmtdesc = {0};
    fmtdesc.index = 0;
    fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    struct camera_format camfmts[10] = {0};
    while (ioctl(v4l2_fd, VIDIOC_ENUM_FMT, &fmtdesc) == 0)
    {
        printf("index:%d 像素格式:0x%x, 描述信息:%s\n",
               fmtdesc.index,
               fmtdesc.pixelformat,
               fmtdesc.description);
        /*将支持的像素格式存入结构体数组*/
        strcpy(camfmts[fmtdesc.index].description, (const char *)fmtdesc.description);
        camfmts[fmtdesc.index].pixelformat = fmtdesc.pixelformat;
        fmtdesc.index++;
    }
    printf("已获取全部支持的格式\n");

    /* 枚举出摄像头所支持的所有视频采集分辨率 */

    struct v4l2_frmsizeenum frmsize = {0};
    struct v4l2_frmivalenum frmival = {0};

    frmival.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    frmsize.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    for (int i = 0; camfmts[i].pixelformat; ++i)
    {

        frmsize.index = 0;
        frmsize.pixel_format = camfmts[i].pixelformat; // 设置要查询的像素格式
        frmival.pixel_format = camfmts[i].pixelformat; // 设置要查询帧率的像素格式

        while (ioctl(v4l2_fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) == 0)
        {

            printf("size<%d*%d> ",
                   frmsize.discrete.width,   // 宽
                   frmsize.discrete.height); // 高
            frmsize.index++;

            /*查询帧率的像素格式*/
            frmival.index = 0;
            frmival.width = frmsize.discrete.width;
            frmival.height = frmsize.discrete.height;
            while (0 == ioctl(v4l2_fd, VIDIOC_ENUM_FRAMEINTERVALS, &frmival))
            {

                printf(" <%dfps> ", frmival.discrete.denominator / frmival.discrete.numerator);
                frmival.index++;
            }
            printf("\n");
        }
        printf("\n");
    }
}

int v4l2_set_format(void)
{
    struct v4l2_format fmt = {0};
    struct v4l2_streamparm streamparm = {0};

    /* 设置帧格式 */
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = 0x56595559;

    if (ioctl(v4l2_fd, VIDIOC_S_FMT, &fmt) < 0)
    {
        perror("ioctl VIDIOC_QUERYCAP error");
        close(v4l2_fd);
        return -1;
    }

    // if (fmt.fmt.pix.pixelformat != 0x56595559)
    // {
    //     fprintf(stderr, "不支持YUYV\n");
    //     close(v4l2_fd);
    //     return -1;
    // }

    printf("当前视频分辨率为<%d * %d>\n", fmt.fmt.pix.width, fmt.fmt.pix.height);

    /* 获取 streamparm */
    streamparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(v4l2_fd, VIDIOC_G_PARM, &streamparm) < 0)
    {
        perror("ioctl VIDIOC_G_PARM error");
        close(v4l2_fd);
        return -1;
    }

    /*检测是否支持帧率设置*/
    if (V4L2_CAP_TIMEPERFRAME & streamparm.parm.capture.capability)
    {
        // 走到这里表示支持帧率设置

        /*设置30fps*/
        printf("该v4l2设备支持帧率设置\n");
        streamparm.parm.capture.timeperframe.denominator = 30;
        streamparm.parm.capture.timeperframe.numerator = 1;
        if (ioctl(v4l2_fd, VIDIOC_S_PARM, &streamparm) < 0)
        {
            perror("ioctl VIDIOC_S_PARM error");
            close(v4l2_fd);
            return -1;
        }
    }

    struct v4l2_format fmt_real;
    memset(&fmt_real, 0, sizeof(fmt_real));
    fmt_real.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (ioctl(v4l2_fd, VIDIOC_G_FMT, &fmt_real) < 0)
    {
        perror("VIDIOC_G_FMT error");
    }
    else
    {
        printf("[实际分辨率格式] %d x %d\n",
               fmt_real.fmt.pix.width,
               fmt_real.fmt.pix.height);
    }

    struct v4l2_streamparm parm_real;
    memset(&parm_real, 0, sizeof(parm_real));
    parm_real.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (ioctl(v4l2_fd, VIDIOC_G_PARM, &parm_real) < 0)
    {
        perror("VIDIOC_G_PARM error");
    }
    else
    {
        if (parm_real.parm.capture.timeperframe.numerator != 0)
        {
            double fps = (double)parm_real.parm.capture.timeperframe.denominator /
                         parm_real.parm.capture.timeperframe.numerator;

            printf("[实际帧率] %.2f fps\n", fps);
        }
    }

    switch (fmt_real.fmt.pix.pixelformat)
    {
    case V4L2_PIX_FMT_YUYV:
        printf("[实际格式] %s\n", "YUYV");
        break;
    case V4L2_PIX_FMT_MJPEG:
        printf("[实际格式] %s\n", "MJPEG");
        break;
        ;
    }

    /* 记录驱动“真实生效”的参数，后续DMA分配和RGA包装都依赖这些值。
     * 注意：这些值可能与我们请求的640x480不完全相同（尤其是bytesperline/sizeimage）。
     */
    width = fmt_real.fmt.pix.width;
    height = fmt_real.fmt.pix.height;
    bytesperline = fmt_real.fmt.pix.bytesperline;
    printf("byteperline:%d\n", bytesperline);
    sizeimage = fmt_real.fmt.pix.sizeimage;
    printf("bytesperline:%d sizeimage:%u\n", bytesperline, sizeimage);
    return 0;
}

int cam_get_width(void)
{
    return width;
}

int cam_get_height(void)
{
    return height;
}

int cam_get_bytesperline(void)
{
    return bytesperline;
}

unsigned int cam_get_sizeimage(void)
{
    return sizeimage;
}

int v4l2_init_buffer(void)
{
    /*
     * 这里不是让驱动自己分配 mmap 缓冲，而是告诉驱动：
     * “我准备用 DMABUF 模式，你来管理队列，但内存由我提供”。
     *
     * 所以这里的 REQBUFS 更像是在创建“队列槽位”，
     * 真正承载图像数据的内存，在本函数内部按解码器的 DRM dumb buffer
     * 方式申请成 dma-buf fd，再绑定给 V4L2。
     */
    struct v4l2_requestbuffers reqbuf = {0};
    reqbuf.count = FRAMEBUFFER_COUNT;
    reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    reqbuf.memory = V4L2_MEMORY_DMABUF;

    if (ioctl(v4l2_fd, VIDIOC_REQBUFS, &reqbuf) < 0)
    {
        perror("ioctl VIDIOC_REQBUFS error");
        close(v4l2_fd);
        exit(-1);
    }

    int i;
    /* 某些驱动会调整最终buffer数量，按驱动返回值入队，避免越界/空洞。 */
    int qcount = (reqbuf.count < FRAMEBUFFER_COUNT) ? reqbuf.count : FRAMEBUFFER_COUNT;
    if (cam_alloc_dmabuf_fds(qcount, sizeimage) != 0)
    {
        close(v4l2_fd);
        return -1;
    }

    for (i = 0; i < qcount; i++)
    {
        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_DMABUF;
        buf.index = i;
        /*
         * 对 DMABUF 模式来说：
         * - index 表示“把第几个队列槽位入队”
         * - m.fd  表示“这个槽位背后绑定哪一个外部 dma-buf”
         * - length 给出这块 buffer 可用容量，避免有些驱动把 0 当异常
         */
        buf.length = sizeimage;
        buf.m.fd = dmafd[i];
        if (ioctl(v4l2_fd, VIDIOC_QBUF, &buf) < 0)
        {
            perror("ioctl VIDIOC_QBUF error");
            cam_release_dmabuf_fds();
            close(v4l2_fd);
            return -1;
        }
    }

    printf("帧缓存区已准备就绪!\n");
    return 0;
}

int v4l2_stream_on(void)
{
    /* 打开摄像头、摄像头开始采集数据 */
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (ioctl(v4l2_fd, VIDIOC_STREAMON, &type) < 0)
    {
        perror("ioctl VIDIOC_STREAMON error");
        close(v4l2_fd);
        return -1;
    }
    printf("开始视频采集\n");
    return 0;
}

void camera_init(void)
{
    v4l2_dev_init();

    if (v4l2_set_format() != 0) {
        fprintf(stderr, "v4l2_set_format failed\n");
        exit(-1);
    }

    if (v4l2_init_buffer() != 0) {
        fprintf(stderr, "v4l2_init_buffer failed\n");
        exit(-1);
    }

    if (v4l2_stream_on() != 0) {
        fprintf(stderr, "v4l2_stream_on failed\n");
        exit(-1);
    }
}

void camera_run(void)
{
    run();
}

void camera_close(void)
{
    request_stop = 1;
    if (v4l2_fd >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        struct v4l2_requestbuffers reqbuf;

        if (ioctl(v4l2_fd, VIDIOC_STREAMOFF, &type) < 0) {
            perror("ioctl VIDIOC_STREAMOFF error");
        }

        memset(&reqbuf, 0, sizeof(reqbuf));
        reqbuf.count = 0;
        reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        reqbuf.memory = V4L2_MEMORY_DMABUF;
        if (ioctl(v4l2_fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
            perror("ioctl VIDIOC_REQBUFS release error");
        }

        close(v4l2_fd);
        v4l2_fd = -1;
    }

    cam_release_dmabuf_fds();
    g_frame_callback = NULL;
    g_frame_callback_userdata = NULL;
    sizeimage = 0;
    bytesperline = 0;
}

void cam_register_frame_callback(cam_frame_callback_t callback, void *userdata)
{
    /* 这里只做保存，不做调用；真正调用发生在 run() 里 DQBUF 成功之后。 */
    g_frame_callback = callback;
    g_frame_callback_userdata = userdata;
}

void run(void)
{
    /*
     * run() 是整条采集链路最关键的地方：
     *
     * poll  等待摄像头有数据
     * DQBUF 从驱动队列里“取出一帧”
     * 回调  把这一帧交给上层（这里是交给 MPP）
     * QBUF  再把这个 buffer 还给驱动复用
     *
     * 这就是 V4L2 采集时最常见的“取帧 / 还帧”模型。
     */
    struct pollfd fds;
    fds.fd = v4l2_fd;
    fds.events = POLLIN;
    struct v4l2_buffer buf;
    int ret = -1;

    while (1)
    {
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_DMABUF;
        /* DQBUF/QBUF 全程保持 length 一致，降低驱动侧边界歧义。 */
        buf.length = sizeimage;

        ret = poll(&fds, 1, 100);
        if (request_stop) {
            break;
        }
        if (ret == 0)
            continue; // 如果超时,那么重新来

        if (ret < 0)
        {
            perror("[run]poll error");
            break;
        }

        // 如果设备错误，挂起，fd被关闭
        if (fds.revents & (POLLERR | POLLHUP | POLLNVAL))
        {
            printf("poll err revents=%d\n", fds.revents);
            // qDebug() << "poll err revents=" << fds.revents;
            break; // 或者走重连逻辑
        }

        if (fds.revents & POLLIN)
        {
            /*
             * DQBUF 成功后，buf 里最重要的两个信息是：
             * - index: 这一帧落在哪个 V4L2 槽位 / dmafd 上
             * - bytesused: 这一帧真实用了多少字节
             *
             * 对 MJPEG 来说，bytesused 尤其重要，
             * 因为它表示“这一帧 JPEG 包的真实长度”。
             */
            if (ioctl(v4l2_fd, VIDIOC_DQBUF, &buf) != 0)
            {
                perror("获取视频帧失败");
                continue;
            }

            if ((int)buf.index < 0 || (int)buf.index >= dmafd_count || dmafd[buf.index] < 0)
            {
                printf("invalid camera buffer index=%u dmafd_count=%d\n", buf.index, dmafd_count);
                if (ioctl(v4l2_fd, VIDIOC_QBUF, &buf) < 0)
                {
                    perror("QBUF invalid index");
                    break;
                }
                continue;
            }

            if (g_frame_callback != NULL)
            {
                /*
                 * 这里传给上层的是：
                 * - dmafd[buf.index] : 当前这帧输入数据所在的外部 dma-buf
                 * - buf.bytesused    : 当前帧 JPEG 的真实长度
                 *
                 * 当前版本为了调试“单帧采集 -> 单帧解码 -> 单帧落盘”，
                 * 在回调后直接 return。
                 *
                 * 如果你以后要做连续视频流处理，需要把这个 return 去掉，
                 * 然后走下面的 QBUF，把 buffer 还回驱动，形成循环。
                 */
                printf("sizeimage:%u bytesused:%u index:%u\n",
                       sizeimage,
                       buf.bytesused,
                       buf.index);
                g_frame_callback(dmafd[buf.index],
                                 buf.index,
                                 width,
                                 height,
                                 bytesperline,
                                 buf.bytesused,
                                 g_frame_callback_userdata);
                //return;
            }
            // drm_show_one_frame(buf.index);
            // drm_atomic_show_one_frame(buf.index);
            // drm_atomic_update_fb(buf.index);
            

            // printf("buf.index:%d\n", buf.index);
            // return;
            //printf("获取到一帧数据\n");
            //rga_push(buf.index);

            if (ioctl(v4l2_fd, VIDIOC_QBUF, &buf) < 0)
            {
                perror("QBUF");
                break;
            }
        }
    }
}
