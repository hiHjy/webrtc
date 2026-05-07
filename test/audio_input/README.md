# Audio Capture + Opus Module

## 1. 模块简介

这个模块实现了一条完整的音频处理链路：

- 从 ALSA 设备采集 PCM 音频
- 将 PCM 数据送入 Opus 编码器
- 在编码完成后，通过回调把 Opus 包交给上层业务

模块设计上分成三层：

- `audio_capture`：纯采集层，只负责和 ALSA 打交道
- `opus_encoder`：纯编码层，只负责把 PCM 编成 Opus
- `audio_stream_manager`：调度层，负责把采集层和编码层串起来，对上层提供统一接口

对业务层来说，最重要的结论是：

上层通常只需要使用 `audio_stream_manager`，不需要分别操作 `audio_capture` 和 `opus_encoder`。

## 2. 目录结构

```text
include/
  audio_capture.h
  opus_encoder.h
  audio_stream_manager.h

src/
  audio_capture.c
  opus_encoder.c
  audio_stream_manager.c
  main.c

main.conf
CMakeLists.txt
```

各文件职责如下：

- `include/audio_capture.h` / `src/audio_capture.c`：ALSA 采集模块
- `include/opus_encoder.h` / `src/opus_encoder.c`：Opus 编码模块
- `include/audio_stream_manager.h` / `src/audio_stream_manager.c`：总控模块
- `src/main.c`：演示程序，展示模块的基本使用方式
- `main.conf`：演示程序读取的采集参数配置文件

## 3. 模块调用链

运行时数据流如下：

```text
ALSA microphone
    ->
audio_capture thread
    ->
audio_capture_read()
    ->
audio_stream_manager_on_pcm_data()
    ->
audio_opus_encoder_push_pcm()
    ->
opus_encode()
    ->
audio_stream_manager_on_opus_packet()
    ->
upper layer packet callback
```

可以理解为：

- PCM 数据入口：`audio_capture -> audio_stream_manager`
- Opus 包出口：`audio_stream_manager -> 上层业务回调`

## 4. 设计思路

### 4.1 `audio_capture`

这一层只负责：

- 打开 ALSA 采集设备
- 设置采样率、声道数、采样格式、period
- 启动后台线程持续读取 PCM
- 通过回调把 PCM 数据抛给上层

这一层不负责：

- Opus 编码
- 网络发送
- 文件保存

### 4.2 `opus_encoder`

这一层只负责：

- 创建 Opus 编码器
- 接收 PCM 数据
- 在内部缓存中攒够一个完整帧
- 调用 `opus_encode()` 编码
- 将编码后的 Opus 包通过回调抛给上层

它不关心 PCM 是从麦克风来的，还是从文件来的，也不关心编码后的包要发到哪里。

### 4.3 `audio_stream_manager`

这一层是上层真正应该使用的接口层。

它负责：

- 管理采集模块和编码模块的生命周期
- 接收采集层回调过来的 PCM
- 把 PCM 转交给 Opus 编码层
- 接收 Opus 编码完成后的包
- 再把包通过统一的回调抛给上层

所以对业务层来说，可以把 `audio_stream_manager` 看成一条“音频输入流”。

## 5. 对外接口

上层最常用的接口都在 `audio_stream_manager.h` 中：

```c
void audio_stream_manager_config_init(audio_stream_manager_config_t *config);

void audio_stream_manager_set_packet_callback(audio_stream_manager_t *stream,
                                              audio_stream_packet_callback_t cb,
                                              void *user_data);

int audio_stream_manager_init(audio_stream_manager_t *stream,
                              const audio_stream_manager_config_t *config);

int audio_stream_manager_start(audio_stream_manager_t *stream);

void audio_stream_manager_stop(audio_stream_manager_t *stream);

void audio_stream_manager_close(audio_stream_manager_t *stream);
```

使用顺序建议固定为：

```text
config_init
    ->
init
    ->
set_packet_callback
    ->
start
    ->
stop
    ->
close
```

## 6. 上层如何使用

上层接入时，一般只需要完成下面几步：

### 6.1 准备配置

先准备 `audio_stream_manager_config_t`，常见参数包括：

- `device`：ALSA 采集设备，例如 `hw:0,0`
- `sample_rate`：采样率，例如 `48000`
- `channels`：声道数，例如 `1`
- `format`：采样格式，当前实际链路建议使用 `SND_PCM_FORMAT_S16_LE`
- `period_size`：每次采集的帧数，例如 `960`
- `opus_bitrate`：Opus 码率，例如 `32000`
- `opus_frame_samples`：Opus 帧长，例如 `960`

### 6.2 注册 Opus 包回调

模块在编码完成后，会通过 `audio_stream_packet_callback_t` 把 Opus 包抛给上层。

这个回调通常用于：

- RTP/UDP 发送
- WebSocket 发送
- 落盘调试
- 统计包长、码率

### 6.3 启动和停止

- `audio_stream_manager_start()`：启动整条链路
- `audio_stream_manager_stop()`：停止采集
- `audio_stream_manager_close()`：释放资源并 flush 尾帧

## 7. 上层使用示例

下面给出一个典型示例。这个例子里，上层通过回调拿到 Opus 包，并简单打印包长。

```c
#include "audio_stream_manager.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int on_opus_packet(const uint8_t *packet, size_t packet_len, void *user_data) {
    (void)packet;
    (void)user_data;

    printf("got opus packet, len=%zu\n", packet_len);
    return 0;
}

int main(void) {
    audio_stream_manager_t stream;
    audio_stream_manager_config_t config;
    int ret;

    audio_stream_manager_config_init(&config);

    snprintf(config.device, sizeof(config.device), "%s", "hw:0,0");
    config.sample_rate = 48000;
    config.channels = 1;
    config.format = SND_PCM_FORMAT_S16_LE;
    config.period_size = 960;
    config.opus_bitrate = 32000;
    config.opus_frame_samples = 960;

    ret = audio_stream_manager_init(&stream, &config);
    if (ret < 0) {
        fprintf(stderr, "audio_stream_manager_init failed: %d\n", ret);
        return 1;
    }

    audio_stream_manager_set_packet_callback(&stream, on_opus_packet, NULL);

    ret = audio_stream_manager_start(&stream);
    if (ret < 0) {
        fprintf(stderr, "audio_stream_manager_start failed: %d\n", ret);
        audio_stream_manager_close(&stream);
        return 1;
    }

    sleep(10);

    audio_stream_manager_stop(&stream);
    audio_stream_manager_close(&stream);
    return 0;
}
```

说明：

- `on_opus_packet()` 就是上层拿到编码结果的地方
- 以后如果要接网络发送逻辑，通常就在这个回调里做
- 示例里 `sleep(10)` 只是为了演示，实际项目里一般由业务线程控制 stop 时机

## 8. demo 程序说明

仓库中的 `src/main.c` 是一个最小可运行示例，它做了这些事情：

- 从 `main.conf` 读取 ALSA 采集参数
- 初始化 `audio_stream_manager_config_t`
- 调用 `audio_stream_manager_init()`
- 调用 `audio_stream_manager_start()`
- 启一个辅助线程，5 秒后调用 `audio_stream_manager_stop()`
- 最后调用 `audio_stream_manager_close()`

这个 demo 的重点是展示生命周期，而不是展示最终发送逻辑。

当前 demo 没有注册 `audio_stream_manager_set_packet_callback()`，因此虽然 Opus 包已经编码出来，但没有继续向业务层发送。

## 9. 编译与运行

### 9.1 编译

```bash
mkdir -p build
cd build
cmake ..
make -j$(nproc)
```

生成结果：

```text
libaudio_capture.a
audio_capture_demo
```

### 9.2 运行

先检查 `main.conf` 中的设备配置是否正确，例如：

```ini
device=hw:0,0
sample_rate=48000
channels=1
format=S16_LE
period_size=960
read_frames=960
```

然后运行：

```bash
./build/audio_capture_demo
```

## 10. 当前实现限制

当前代码可以正常构成完整链路，但有几条需要明确说明：

- 当前 `audio_stream_manager` 实际只支持 `SND_PCM_FORMAT_S16_LE` 直接送入 Opus
- `read_frames` 已经在 `main.conf` 中解析，但当前采集线程仍按 `period_size` 固定读取
- demo 只负责跑通采集和编码，没有实现发送层
- 若上层想异步保存 PCM 回调里的数据，需要自行复制缓冲区，不能长期直接持有回调里的 `data` 指针

## 11. 适合的上层接法

这个模块比较适合挂在如下场景中：

- 本地麦克风采集后编码并发送到服务器
- 设备侧语音上报
- 音频前端采集后交给 WebRTC/RTP/私有协议发送模块
- 调试阶段先把 Opus 包落盘分析

推荐接法是：

- 让 `audio_stream_manager` 负责音频采集和编码
- 让业务层只关心 `packet callback`
- 在 `packet callback` 中接入网络发送或缓存逻辑

这样模块边界会比较清晰，后续维护成本也更低。
