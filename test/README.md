# WebRTC 测试模块说明

这个 `test/` 目录是当前工程里的端到端验证区，主要用来验证：

- RK3568 摄像头 V4L2 DMABUF 采集。
- Rockchip MPP 硬件 H.264 编码 / 解码。
- WebRTC 推流到 SRS。
- WebRTC 拉流后硬解码并显示。
- ALSA 采集、Opus 编码、WebRTC 音频发送。
- ALSA 播放、Opus 解码、WebRTC 音频接收。

这里的代码更偏“把链路跑通和排问题”，不是最终库接口。后面整理公共库时，可以从这里把稳定的模块抽出去。

## 目录结构

```text
test/
  app/
    push/
      aarch64/main.cpp   # 板端摄像头 + MPP 编码 + WebRTC/SRS 推流
      x86/main.cpp       # x86 推流测试入口
    pull/
      aarch64/main.cpp   # 板端 WebRTC 拉流 + MPP 解码 + 显示/播放
  cam/                   # V4L2 摄像头采集，DMABUF 模式
  encoder/               # Rockchip MPP 编码封装
  decoder/               # Rockchip MPP 解码封装
  drm/                   # DRM 显示测试/输出
  audio_input/           # ALSA 采集 + Opus 编码
  audio_output/          # Opus 解码 + ALSA 播放
  build.sh               # CMake 构建入口
```

## 构建

在 `test/` 目录下构建：

```bash
./build.sh x86
./build.sh aarch64
./build.sh all
```

也可以在仓库根目录执行：

```bash
cd test
./build.sh aarch64
```

交叉编译默认使用：

```text
$HOME/rk3568_sysroot_fixed
```

如果你的 sysroot 不在这个路径，可以手动指定：

```bash
SYSROOT=/path/to/sysroot ./build.sh aarch64
```

构建产物会放在：

```text
test/build-x86/
test/build-aarch64/
```

这些目录已经通过 `.gitignore` 忽略，不应该提交。

## 推流链路

板端推流入口：

```text
test/app/push/aarch64/main.cpp
```

当前视频链路是：

```text
V4L2 摄像头
  -> DMABUF fd
  -> MPP H.264 encoder
  -> Annex-B H.264 packet
  -> WebRTCToSrs
  -> SRS
```

当前音频链路是：

```text
ALSA capture
  -> PCM S16_LE
  -> Opus encoder
  -> WebRTCToSrs
  -> SRS
```

推流地址在 `main.cpp` 里：

```cpp
const std::string publish_api = "http://192.168.1.27:1985/rtc/v1/publish/";
const std::string stream_url = "webrtc://192.168.1.27/live/test";
```

换环境时需要改成自己的 SRS 地址。

## 摄像头格式：为什么 YUYV 也能直接送 MPP 编码器

你现在这个推流 demo 不是偷偷把摄像头转成 NV12 了，而是直接按 YUYV 送给了 MPP encoder。

关键代码在 `test/cam/cam.c`：

```c
fmt.fmt.pix.pixelformat = 0x56595559;
```

`0x56595559` 就是 V4L2 的 `V4L2_PIX_FMT_YUYV`，也就是 YUYV / YUY2，属于 YUV422 packed 格式。

设置完成后，代码又用 `VIDIOC_G_FMT` 读取驱动实际接受的格式，并记录：

```c
width = fmt_real.fmt.pix.width;
height = fmt_real.fmt.pix.height;
bytesperline = fmt_real.fmt.pix.bytesperline;
sizeimage = fmt_real.fmt.pix.sizeimage;
```

然后在 `test/app/push/aarch64/main.cpp` 里初始化编码器：

```cpp
rk_mpp_encoder_init(&ctx.encoder,
                    MPP_VIDEO_CodingAVC,
                    cam_get_width(),
                    cam_get_height(),
                    cam_get_bytesperline(),
                    cam_get_height(),
                    MPP_FMT_YUV422_YUYV,
                    ctx.video_fps,
                    0,
                    ctx.video_fps,
                    NULL);
```

这里有两个非常关键的点：

- `fmt` 传的是 `MPP_FMT_YUV422_YUYV`，告诉 MPP 输入不是 NV12，而是 YUYV。
- `h_stride` 传的是 `cam_get_bytesperline()`，也就是 V4L2 驱动返回的每行字节数。

YUYV 是 2 字节/像素，所以如果宽度是 640，常见 `bytesperline` 是：

```text
640 * 2 = 1280
```

也就是说，MPP encoder 当前能跑，是因为你已经把输入格式和 stride 都按 YUYV 告诉它了。

## 这里还有一个代码隐患

`test/encoder/rkmpp_enc.c` 里的 buffer size 计算现在仍然写的是 NV12 模型：

```c
enc->frame_size = (size_t)enc->h_stride * enc->v_stride * 3 / 2;
```

但 YUYV 正确大小应该按一整行字节数算：

```c
enc->frame_size = (size_t)enc->h_stride * enc->v_stride;
```

因为这里的 `h_stride` 已经是 `bytesperline`，不是像素宽度。

为什么现在可能还能跑？

- V4L2 申请的 dma-buf 是按 `sizeimage` 分配的，真实内存够。
- `mpp_buffer_import()` 里的 `info.size` 偏小，有些 MPP 路径不会严格依赖它读完整帧。
- 画面尺寸和 stride 比较规整时，问题不一定立刻暴露。

但从封装正确性上说，这里应该改成按 `fmt` 计算 frame size。至少要区分：

```c
MPP_FMT_YUV420SP      -> h_stride * v_stride * 3 / 2
MPP_FMT_YUV420SP_VU   -> h_stride * v_stride * 3 / 2
MPP_FMT_YUV422_YUYV   -> h_stride * v_stride
MPP_FMT_YUV422_YVYU   -> h_stride * v_stride
MPP_FMT_YUV422_UYVY   -> h_stride * v_stride
MPP_FMT_YUV422_VYUY   -> h_stride * v_stride
```

## 为什么正式库里仍然建议优先 NV12

虽然 YUYV 直送 MPP 在这个 demo 里能跑，但正式硬件加速库仍然建议以 NV12 作为主路径：

- H.264/H.265 常规编码输出是 YUV420，NV12 更接近编码器最常用输入。
- NV12 布局简单，跨芯片、跨 MPP 版本更稳定。
- YUYV 直编要非常小心 `bytesperline`、对齐和 MPP 版本差异。
- RGA 本来就适合做 YUYV -> NV12、缩放、旋转等图像预处理。

推荐正式链路：

```text
如果摄像头已经输出 NV12：
  camera NV12 fd -> MPP encoder

如果摄像头输出 YUYV/RGB/其他：
  camera fd -> RGA 转 NV12 -> MPP encoder
```

当前 demo 的 YUYV 直编可以作为性能验证路径保留，但不要把它误认为“所有格式都可以无条件直送 MPP”。

## 拉流链路

板端拉流入口：

```text
test/app/pull/aarch64/main.cpp
```

典型链路：

```text
SRS / WebRTC
  -> H.264 Annex-B
  -> MPP decoder
  -> NV12 dma-buf
  -> DRM / 显示路径
```

音频链路：

```text
SRS / WebRTC
  -> Opus packet
  -> Opus decoder
  -> PCM
  -> ALSA playback
```

## 注意事项

- `test/build*`、`.nv12`、`.wav`、CMake 产物和运行时拷贝的 `.so` 都不应该提交。
- 板端运行前确认 `/dev/video28` 是否是当前摄像头节点。
- 推流前确认 SRS 地址、音频设备号和网络环境。
- 如果画面花屏，优先检查 `pixelformat`、`bytesperline`、`sizeimage`、MPP `fmt`、MPP `h_stride` 是否一致。
- 如果音频没有声音，优先检查 ALSA 设备号、采样率、声道数和 period size。
