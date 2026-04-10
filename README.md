# webrtc

这是我本地整理的一套最小 WebRTC 示例目录，核心组合是：

- `libdatachannel`
- `SRS`
- 一个浏览器 HTML 播放页

当前目录主要用于两类事情：

- C++ 侧通过 `libdatachannel` 和 SRS 的 `/rtc/v1/play/`、`/rtc/v1/publish/` API 交换 SDP
- 浏览器侧通过一个最小 HTML 页面播放 SRS 的 WebRTC 流

## 目录说明

- `include/rtc`
  - `libdatachannel` 的头文件
- `lib/x86`
  - `x86-64` 版本 `libdatachannel.so`
- `lib/aarch64`
  - `aarch64` 版本 `libdatachannel.so`
- `pull`
  - C++ 拉流示例
- `push`
  - C++ 推流示例
- `min_srs_rtc_player.html`
  - 最小浏览器播放页，直接对接 SRS WebRTC 播放接口

## HTML 页面怎么用

这个 HTML 不需要复杂前端环境，用 Python 起一个静态 HTTP 服务就可以。

在 `~/webrtc` 目录执行：

```bash
cd ~/webrtc
python3 -m http.server 8000
```

然后浏览器打开：

```text
http://127.0.0.1:8000/min_srs_rtc_player.html
```

页面里需要填写这些参数：

- `SRS Host`
- `API Port`
- `App`
- `Stream`

比如常见配置：

- `SRS Host`: `192.168.1.27`
- `API Port`: `1985`
- `App`: `live`
- `Stream`: `test`

页面点击“开始播放”后，会向 SRS 发起 WebRTC 播放请求。

## 和 SRS 的关系

这套示例不是浏览器直接裸连媒体源，而是通过 SRS 做信令和媒体中转。

大致流程是：

1. 推流端生成 offer
2. 通过 HTTP POST 发给 SRS 的 publish API
3. SRS 返回 answer
4. 推流端开始把媒体送给 SRS
5. 播放端或浏览器再通过 SRS 的 play API 拉流

也就是说：

- `push/` 负责往 SRS 推
- `pull/` 负责从 SRS 拉
- `min_srs_rtc_player.html` 负责在浏览器里播

## 常用启动方式

### 1. 浏览器播放页

```bash
cd ~/webrtc
python3 -m http.server 8000
```

### 2. C++ 拉流/推流示例

`pull/main.cpp` 和 `push/main_push_callback.cpp`、`push/main_push_file.cpp` 都是教学示例代码，逻辑重点是：

- 用 `libdatachannel` 创建 `PeerConnection`
- 本地生成 SDP offer
- 通过最小 HTTP 客户端把 offer 发给 SRS
- 收到 answer 后完成建链

## 依赖提醒

如果你运行的是本地 `x86` 版本程序，通常需要能找到：

- `~/webrtc/lib/x86/libdatachannel.so`

如果你运行的是目标板 `aarch64` 版本程序，通常需要能找到：

- `~/webrtc/lib/aarch64/libdatachannel.so`

实际运行时如果提示找不到动态库，可以临时这样设置：

```bash
export LD_LIBRARY_PATH=~/webrtc/lib/x86:$LD_LIBRARY_PATH
```

或者：

```bash
export LD_LIBRARY_PATH=~/webrtc/lib/aarch64:$LD_LIBRARY_PATH
```
