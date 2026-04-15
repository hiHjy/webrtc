#include "webrtctosrs.hpp"
#include "srs_api.hpp"

// 用一个静态指针保存当前对象，主要是为了让信号处理函数里
// 能够拿到 WebRTCToSrs 实例，把退出标志位改掉。
// 这里的使用方式比较直接，适合当前这种单实例程序。
WebRTCToSrs *WebRTCToSrs::self = nullptr;

WebRTCToSrs::WebRTCToSrs() = default;

WebRTCToSrs::~WebRTCToSrs() = default;

// 信号处理函数只做一件尽量安全的事：
// 收到 Ctrl+C / kill 后，把停止标志置为 true。
// 真正复杂的清理逻辑不要塞在信号处理函数里，避免引入额外问题。
void HandleSignal(int) {
	WebRTCToSrs *instance = WebRTCToSrs::getInstance();
	if (instance) {
		instance->setStopRequested(true);
	}
}

void WebRTCToSrs::Stop() {
	if (videoTrack) {
		videoTrack->close();
	}
	if (audioTrack) {
		audioTrack->close();
	}
	if (pc) {
		pc->close();
	}
	std::cout << "WebRTCToSrs stopped" << std::endl;
}

int WebRTCToSrs::initRTC(const char *url, const char *stream_url) {
	// 这个函数负责完成一整套“最小可用”的 WebRTC 推流初始化流程：
	// 1. 注册信号处理和日志
	// 2. 创建 PeerConnection
	// 3. 配置视频/音频发送轨道
	// 4. 生成本地 offer SDP
	// 5. 调 SRS publish API 换 answer
	// 6. 应用 answer，并等待轨道真正打开
	//
	// 当前这个版本的重点是把链路打通，后面你再往里面补真实音视频数据发送逻辑。
	std::cout << "initRTC" << std::endl;

	// 注册退出信号，便于主线程中的 while 循环优雅退出。
	std::signal(SIGINT, HandleSignal);
	std::signal(SIGTERM, HandleSignal);

	// 打开 libdatachannel 日志，当前先用 Info，调试链路时最直观。
	rtc::InitLogger(rtc::LogLevel::Info);

	// 创建 PeerConnection，这是后续所有 WebRTC 状态、track、SDP 的核心对象。
	pc = std::make_shared<rtc::PeerConnection>();
	WebRTCToSrs::self = this;

	// 这两个状态回调非常有用。
	// 出现“连不上”“卡住”“轨道不打开”时，第一手线索通常就在这里。
	pc->onStateChange([](rtc::PeerConnection::State state) {
		std::cout << "[pc] state: " << state << std::endl;
	});
	pc->onIceStateChange([](rtc::PeerConnection::IceState state) {
		std::cout << "[pc] ice: " << state << std::endl;
	});

	gatherDone = false;

	// GatheringComplete 表示本地 ICE candidate 已经收集完。
	// 这里等收集完再取 localDescription，可以拿到更完整的 SDP。
	pc->onGatheringStateChange([&](rtc::PeerConnection::GatheringState state) {
		std::cout << "[pc] gathering: " << state << std::endl;
		if (state == rtc::PeerConnection::GatheringState::Complete) {
			{
				std::lock_guard<std::mutex> lock(gatherMutex);
				gatherDone = true;
			}
			gatherCond.notify_one();
		}
	});

	const std::string h264Fmtp =
	    "profile-level-id=42c01f;packetization-mode=1;level-asymmetry-allowed=1";

	// ----------------------------
	// 1. 配置视频发送轨道
	// ----------------------------
	//
	// 这里描述的是“我要发送一个 H264 视频轨”。
	// 目前只是把轨道和 RTP 打包链建起来，还没有真正 sendFrame。
	int PayloadType = 96;
	rtc::Description::Video video("video-send", rtc::Description::Direction::SendOnly);
	video.addH264Codec(static_cast<uint8_t>(PayloadType), h264Fmtp);

	/*
	    ssrc (uint32_t)

	    含义：同步源标识符（Synchronization Source）
	    作用：在RTP/RTCP协议中唯一标识一个媒体流源
	    用途：确保接收端能够正确识别和处理来自不同源的媒体流，是媒体流的唯一标识

	    cname (string)

	    含义：规范名称（Canonical Name）
	    作用：提供一个稳定的标识符，即使SSRC发生变化也能保持流的连续性
	    用途：在网络切换或其他情况下SSRC可能改变时，CNAME用于关联不同SSRC的同一媒体流


	    "stream1" (string)

	    含义：媒体流ID（Media Stream ID）
	    作用：标识一组相关的媒体轨道
	    用途：用于将音频和视频等不同类型的媒体轨道关联到同一个媒体流中


	    "video" (string)

	    含义：轨道ID（Track ID）
	    作用：标识媒体流中的具体轨道
	    用途：用于在媒体流中区分不同类型的轨道，例如视频轨道或音频轨道

	*/
	uint32_t ssrc = 12345678;
	std::string cname = "cname123"; // 在WebRTC中，音频流和视频流的cname（规范名称）通常应该相同
	video.addSSRC(ssrc, cname, "stream1", "video");

	// addTrack 后返回的就是本地视频轨对象。
	// 你后面如果要发 H264 Annex-B 数据，通常就是通过这个轨道来送。
	videoTrack = pc->addTrack(video);

	// 下面这条链路的作用是：
	// 原始 H264 帧 -> H264 RTP packetizer -> RTCP Sender Report -> NACK 处理 -> 网络发送
	//
	// 也就是说，后面你只需要关心“给 videoTrack 喂一帧视频”，
	// RTP 分片、RTCP 报文、NACK 响应都由这条链来处理。
	auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>( // 1. 创建 RTP 打包配置
	    ssrc, cname, static_cast<uint8_t>(PayloadType), rtc::H264RtpPacketizer::ClockRate);
	auto packetizer = std::make_shared<rtc::H264RtpPacketizer>( // 2. 创建 H.264 分片器（核心）
	    rtc::NalUnit::Separator::StartSequence, rtpConfig);
	auto srReporter =
	    std::make_shared<rtc::RtcpSrReporter>(rtpConfig); // 3.. 创建 RTCP 处理链（三个处理器）
	auto nackResponder = std::make_shared<rtc::RtcpNackResponder>();
	packetizer->addToChain(srReporter);
	packetizer->addToChain(nackResponder);

	// 之后只要执行 mTrack->send(二进制视频数据)，数据就会自动流经上述管道，变成标准 RTP
	// 包发送出去，并且自动处理 NACK 重传和 SR 报告。
	videoTrack->setMediaHandler(packetizer); // 4. 挂载到媒体轨道

	videoTrackOpen = false;

	// 轨道 open 说明协商和底层链路基本已经准备好了，
	// 后面再 sendFrame 才有意义。
	videoTrack->onOpen([&]() {
		std::cout << "[track] open" << std::endl;
		{
			std::lock_guard<std::mutex> lock(videoOpenMutex);
			videoTrackOpen = true;
		}
		videoOpenCv.notify_one();
	});

	// closed 一般意味着链路断开、对端关闭或者状态被重置。
	videoTrack->onClosed([&]() {
		std::cout << "[track] closed" << std::endl;
		{
			std::lock_guard<std::mutex> lock(videoOpenMutex);
			videoTrackOpen = false;
		}
		videoOpenCv.notify_one();
	});

	// ----------------------------
	// 2. 配置音频发送轨道
	// ----------------------------
	//
	// 音频这边和视频思路完全一致：
	// 先描述一个 SendOnly 的 Opus 音频轨，再给它挂 RTP 打包链。
	int audioPayloadType = 111;
	rtc::Description::Audio audio("audio-send", rtc::Description::Direction::SendOnly);
	audio.addAudioCodec(static_cast<uint8_t>(audioPayloadType), "opus/48000/1",
	                    "minptime=10;useinbandfec=1;stereo=0;sprop-stereo=0");

	uint32_t audioSsrc = 87654321;
	audio.addSSRC(audioSsrc, cname, "stream1", "audio");

	// 当前先只把轨道搭起来。
	// 后面你如果有 Opus 编码后的音频包，就通过这个轨道 sendFrame。
	audioTrack = pc->addTrack(audio);

	// 原始 Opus packet -> Opus RTP packetizer -> RTCP Sender Report -> NACK 处理 -> 网络发送
	auto audioRtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
	    audioSsrc, cname, static_cast<uint8_t>(audioPayloadType),
	    rtc::OpusRtpPacketizer::DefaultClockRate);
	auto audioPacketizer = std::make_shared<rtc::OpusRtpPacketizer>(audioRtpConfig);
	auto audioSrReporter = std::make_shared<rtc::RtcpSrReporter>(audioRtpConfig);
	auto audioNackResponder = std::make_shared<rtc::RtcpNackResponder>();
	audioPacketizer->addToChain(audioSrReporter);
	audioPacketizer->addToChain(audioNackResponder);
	audioTrack->setMediaHandler(audioPacketizer);

	audioTrackOpen = false;

	// 音频轨的 open / closed 状态单独观察，方便和视频轨分开排查问题。
	audioTrack->onOpen([&]() {
		std::cout << "[audio-track] open" << std::endl;
		{
			std::lock_guard<std::mutex> lock(audioOpenMutex);
			audioTrackOpen = true;
		}
		audioOpenCv.notify_one();
	});
	audioTrack->onClosed([&]() {
		std::cout << "[audio-track] closed" << std::endl;
		{
			std::lock_guard<std::mutex> lock(audioOpenMutex);
			audioTrackOpen = false;
		}
		audioOpenCv.notify_one();
	});

	// 触发生成本地 SDP offer。
	// 这一步之后，PeerConnection 会开始准备本地描述和 ICE candidate。
	pc->setLocalDescription();
	{
		// 最多等 5 秒，避免一直卡死在 gathering 阶段。
		std::unique_lock<std::mutex> lock(gatherMutex);
		gatherCond.wait_for(lock, std::chrono::seconds(5), [&]() { return gatherDone; });
	}

	// 取出本地 SDP。
	// 如果这里为空，说明前面的 setLocalDescription / gathering 流程没有成功完成。
	auto local = pc->localDescription();
	if (!local) {
		std::cout << "localDescription not ready" << std::endl;
		return -1;
	}

	// std::cout << "Local SDP:\n" << local << std::endl;
	const std::string offerSdp = std::string(local.value());
	std::cout << "==================== Local SDP ==========================" << std::endl;
	std::cout << "Local SDP:\n" << offerSdp << std::endl << std::endl;

	// 组装发给 SRS publish API 的 JSON body。
	// 这里已经抽到 srs_api.cpp 里了，后面如果你要改 body 格式，只改那一处。
	const std::string body = BuildSrsApiBody(url, stream_url, offerSdp);
	printf("body=%s\n", body.c_str());

	// 通过 HTTP 把本地 offer 发给 SRS，SRS 会返回 answer SDP。
	const std::string response = HttpPostJson(url, body);
	const int responseCode = ExtractCode(response);
	if (responseCode != 0) {
		// code 非 0 说明 SRS 业务层面已经拒绝了请求，
		// 这种情况下就不要再继续提取 sdp 了。
		std::cout << "SRS API error: " << response << std::endl;
		return -1;
	}

	printf("response=%s\n", response.c_str());

	// 从响应里取 answer SDP，并打印出来方便你对照抓包或调试日志。
	const std::string answerSdp = ExtractSdp(response);
	std::cout << "==================== Answer SDP ==========================" << std::endl;
	std::cout << "Answer SDP:\n" << answerSdp << std::endl << std::endl;

	// 应用远端 answer。
	// 这一步之后，PeerConnection 才算真正进入协商完成阶段。
	pc->setRemoteDescription(answerSdp);

	// 等待视频轨真正打开。
	// “协商成功”不等于“轨道已经能发数据”，这里再等一层状态更稳。
	{
		std::unique_lock<std::mutex> lock(videoOpenMutex);
		videoOpenCv.wait_for(lock, std::chrono::seconds(10), [&]() { return videoTrackOpen; });
		if (!videoTrackOpen) {
			std::cout << "Video track failed to open" << std::endl;
			return -1;
		}

		std::cout << "Video track opened successfully" << std::endl;
	}

	// 等待音频轨真正打开。
	{
		std::unique_lock<std::mutex> lock(audioOpenMutex);
		audioOpenCv.wait_for(lock, std::chrono::seconds(10), [&]() { return audioTrackOpen; });
		if (!audioTrackOpen) {
			std::cout << "Audio track failed to open" << std::endl;
			return -1;
		}
		std::cout << "Audio track opened successfully" << std::endl;
	}

	// 能走到这里，说明：
	// 1. 本地 offer 已生成
	// 2. SRS 已返回有效 answer
	// 3. 音视频轨都打开成功
	//
	// 后面你只需要继续往视频轨/音频轨里送实际编码后的媒体数据即可。
	return 0;
}

bool WebRTCToSrs::SendEncodedAnnexBFrame(const uint8_t *data, size_t size, uint64_t timestampUs) {
	if (!videoTrack || !videoTrack->isOpen() || !data || size == 0) {
		std::cout << "video track is not open" << std::endl;
		return false;
	}

	// 这里把裸指针包装成 rtc::binary。
	// rtc::binary 本质上就是 libdatachannel 里常用的字节数组类型。
	rtc::binary frame(reinterpret_cast<const std::byte *>(data),
	                  reinterpret_cast<const std::byte *>(data) + size);

	// timestampUs 用来告诉 packetizer/发送链路“这一帧的显示时间”。
	// 即便只是最小示例，也最好传一个连续递增的时间戳。
	videoTrack->sendFrame(frame,
	                      rtc::FrameInfo{std::chrono::duration<double, std::micro>(timestampUs)});
	return true;
}

bool WebRTCToSrs::SendEncodedOpus(const uint8_t *data, size_t size, uint64_t timestampUs) {
	if (!audioTrack || !audioTrack->isOpen() || !data || size == 0) {
		std::cout << "audio track is not open" << std::endl;
		return false;
	}

	// 这里把裸指针包装成 rtc::binary。
	// rtc::binary 本质上就是 libdatachannel 里常用的字节数组类型。
	rtc::binary frame(reinterpret_cast<const std::byte *>(data),
	                  reinterpret_cast<const std::byte *>(data) + size);

	// timestampUs 用来告诉 packetizer/发送链路“这一帧的显示时间”。
	// 即便只是最小示例，也最好传一个连续递增的时间戳。
	audioTrack->sendFrame(frame,
	                      rtc::FrameInfo{std::chrono::duration<double, std::micro>(timestampUs)});
	return true;
}
