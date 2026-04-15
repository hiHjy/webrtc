#include "rtc/rtc.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <netdb.h>
#include <regex>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

// 这份代码是“摄像头/编码器回调版”的最小教学模板。
//
// 它和文件版的最大区别只有一件事：
// - 文件版：程序自己从 .h264 文件里拆帧，再 sendFrame
// - 回调版：假设你的编码器已经一帧一帧地把 Annex-B H264 给你了，程序只负责建链和发送
//
// 所以后面如果你接摄像头，真正要替换的核心入口只有：
//   PushSession::SendEncodedAnnexBFrame(data, size, timestampUs)
//
// 只要你的 data/size 代表“完整的一帧 H264 Annex-B 数据”，通常就可以直接送进去。

namespace {

// 全局退出标志：
// - Ctrl+C 后信号处理函数只做一件最安全的事：把标志改成 true
// - 真正的关闭动作放回主线程里做，避免在信号处理函数里做复杂逻辑
std::atomic<bool> gStopRequested{false};

void HandleSignal(int) { gStopRequested.store(true); }

// 这个结构体只服务于“最小 HTTP 客户端”：
// 我们只支持 http://host:port/path 这种形式，
// 所以把 URL 拆成 host/port/path 三段就够了。
struct ParsedUrl {
	std::string host;
	std::string port;
	std::string path;
};

// SRS 的 publish/play API 都是 JSON body，
// 所以 offer SDP 放进去之前必须先做 JSON 转义。
std::string JsonEscape(const std::string &input) {
	std::string out;
	out.reserve(input.size() + 32);
	for (char c : input) {
		switch (c) {
		case '\\':
			out += "\\\\";
			break;
		case '"':
			out += "\\\"";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\n':
			out += "\\n";
			break;
		default:
			out += c;
			break;
		}
	}
	return out;
}

// SRS 回答里的 SDP 是 JSON string，需要反转义后才能交给 rtc::Description。
std::string JsonUnescape(const std::string &input) {
	std::string out;
	out.reserve(input.size());
	for (size_t i = 0; i < input.size(); ++i) {
		if (input[i] != '\\') {
			out.push_back(input[i]);
			continue;
		}
		if (i + 1 >= input.size()) {
			break;
		}
		const char n = input[++i];
		switch (n) {
		case 'n':
			out.push_back('\n');
			break;
		case 'r':
			out.push_back('\r');
			break;
		case 't':
			out.push_back('\t');
			break;
		case '\\':
			out.push_back('\\');
			break;
		case '"':
			out.push_back('"');
			break;
		default:
			out.push_back(n);
			break;
		}
	}
	return out;
}

// SRS API 约定 code=0 表示业务成功。
// 如果这里只拿不到 code，说明服务器返回已经不符合预期了。
int ExtractCode(const std::string &jsonText) {
	static const std::regex codeRe("\\\"code\\\"\\s*:\\s*(-?\\d+)");
	std::smatch m;
	if (!std::regex_search(jsonText, m, codeRe)) {
		throw std::runtime_error("SRS response has no code field");
	}
	return std::stoi(m[1].str());
}

// 从响应 JSON 中提取 answer SDP。
std::string ExtractSdp(const std::string &jsonText) {
	static const std::regex sdpRe("\\\"sdp\\\"\\s*:\\s*\\\"((?:\\\\.|[^\\\"\\\\])*)\\\"");
	std::smatch m;
	if (!std::regex_search(jsonText, m, sdpRe)) {
		throw std::runtime_error("SRS response has no sdp field");
	}
	return JsonUnescape(m[1].str());
}

// 这里故意只做“最小 URL 解析”：
// - 不支持 https
// - 不支持 query string 的复杂场景
// 因为这个示例的目标只是帮助你看懂 WebRTC 主链路，不引入额外库。
ParsedUrl ParseHttpUrl(const std::string &url) {
	static const std::regex re(R"(^http://([^/:]+)(?::(\d+))?(\/.*)?$)");
	std::smatch m;
	if (!std::regex_match(url, m, re)) {
		throw std::runtime_error("Only http:// URL is supported");
	}
	return ParsedUrl{m[1].str(), m[2].matched ? m[2].str() : "80", m[3].matched ? m[3].str() : "/"};
}

// HTTP 可能返回 chunked 编码，这里做兼容，避免以后换环境时踩坑。
std::string DecodeChunkedBody(const std::string &chunked) {
	std::string out;
	size_t pos = 0;
	while (true) {
		const size_t lineEnd = chunked.find("\r\n", pos);
		if (lineEnd == std::string::npos) {
			throw std::runtime_error("Invalid chunked response");
		}
		const size_t chunkSize = std::stoul(chunked.substr(pos, lineEnd - pos), nullptr, 16);
		pos = lineEnd + 2;
		if (chunkSize == 0) {
			break;
		}
		if (pos + chunkSize + 2 > chunked.size()) {
			throw std::runtime_error("Truncated chunked response");
		}
		out.append(chunked, pos, chunkSize);
		pos += chunkSize + 2;
	}
	return out;
}

// 最小 HTTP POST(JSON) 实现：
// 1. 解析 URL
// 2. TCP 连接到 SRS API
// 3. 发送请求
// 4. 读取响应
// 5. 拆出 body
//
// 这么写的目的不是“生产级 HTTP 客户端”，而是帮助你看明白：
// WebRTC 的媒体链路之外，SRS publish/play 还需要一条单独的信令交换。
std::string HttpPostJson(const std::string &url, const std::string &body) {
	const ParsedUrl parsed = ParseHttpUrl(url);

	struct addrinfo hints;
	std::memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	struct addrinfo *res = nullptr;
	const int gai = getaddrinfo(parsed.host.c_str(), parsed.port.c_str(), &hints, &res);
	if (gai != 0) {
		throw std::runtime_error(std::string("getaddrinfo failed: ") + gai_strerror(gai));
	}

	int fd = -1;
	for (auto *p = res; p != nullptr; p = p->ai_next) {
		fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
		if (fd < 0) {
			continue;
		}
		if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) {
			break;
		}
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	if (fd < 0) {
		throw std::runtime_error("connect failed to SRS API");
	}

	const std::string request =
	    "POST " + parsed.path + " HTTP/1.1\r\n" + "Host: " + parsed.host + ":" + parsed.port +
	    "\r\n" + "Content-Type: application/json\r\n" + "Connection: close\r\n" +
	    "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;

	size_t sent = 0;
	while (sent < request.size()) {
		const ssize_t n = send(fd, request.data() + sent, request.size() - sent, 0);
		if (n <= 0) {
			close(fd);
			throw std::runtime_error("send failed");
		}
		sent += static_cast<size_t>(n);
	}

	std::string raw;
	char buffer[4096];
	while (true) {
		const ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
		if (n == 0) {
			break;
		}
		if (n < 0) {
			close(fd);
			throw std::runtime_error("recv failed");
		}
		raw.append(buffer, static_cast<size_t>(n));
	}
	close(fd);

	const size_t sep = raw.find("\r\n\r\n");
	if (sep == std::string::npos) {
		throw std::runtime_error("Invalid HTTP response");
	}
	const std::string headers = raw.substr(0, sep);
	std::string responseBody = raw.substr(sep + 4);
	if (headers.find("Transfer-Encoding: chunked") != std::string::npos ||
	    headers.find("transfer-encoding: chunked") != std::string::npos) {
		responseBody = DecodeChunkedBody(responseBody);
	}
	return responseBody;
}

class PushSession {
public:
	PushSession(std::string publishApi, std::string streamUrl, int payloadType)
	    : mPublishApi(std::move(publishApi)), mStreamUrl(std::move(streamUrl)),
	      mPayloadType(payloadType) {}

	// Start() 负责三件事：
	// 1. 创建 PeerConnection
	// 2. 完成和 SRS 的 offer/answer 信令
	// 3. 建立一条 SendOnly 的 H264 视频 track
	//
	// 你可以把它理解成“把推流通道先搭好”，但此时还没有真正发送任何一帧视频。
	void Start() {
		const rtc::SSRC ssrc = 42;
		const std::string cname = "video-send";

		/*这段字符串是 SDP（Session Description Protocol，会话描述协议） 中用于描述 H.264
		视频编码能力的参数， 通常出现在 SIP 或 RTSP 等多媒体会话协商中。
		它的作用类似一张“设备能力清单”，告诉接收端发送端支持哪些编码特性*/
		const std::string h264Fmtp =
		    "profile-level-id=42c01f;packetization-mode=1;level-asymmetry-allowed=1";

		// PeerConnection 是 WebRTC 的核心对象：
		// - ICE 打洞
		// - DTLS/SRTP
		// - track 管理
		// 都挂在它身上。
		mPc = std::make_shared<rtc::PeerConnection>();
		mPc->onStateChange([](rtc::PeerConnection::State state) {
			std::cout << "[pc] state: " << state << std::endl;
		});
		mPc->onIceStateChange([](rtc::PeerConnection::IceState state) {
			std::cout << "[pc] ice: " << state << std::endl;
		});
		mPc->onGatheringStateChange([this](rtc::PeerConnection::GatheringState state) {
			std::cout << "[pc] gathering: " << state << std::endl;
			if (state == rtc::PeerConnection::GatheringState::Complete) {
				{
					std::lock_guard<std::mutex> lock(mGatherMutex);
					mGatherDone = true;
				}
				mGatherCv.notify_one();
			}
		});

		// 这里声明“我要发一条 H264 视频轨道”。
		// 这一步只是描述能力，不代表已经开始发媒体。
		rtc::Description::Video video("video", rtc::Description::Direction::SendOnly);
		video.addH264Codec(static_cast<uint8_t>(mPayloadType), h264Fmtp);
		video.addSSRC(ssrc, cname, "stream1", "video");
		mTrack = mPc->addTrack(video);

		// 这里挂 packetizer，意思是：
		// 你后面送进来的数据不是 RTP 包，而是一帧完整的 H264 Annex-B 数据。
		// packetizer 会替你完成 RTP 分片、序号、时间戳等工作。

		auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>( // 1. 创建 RTP 打包配置
		    ssrc, cname, static_cast<uint8_t>(mPayloadType), rtc::H264RtpPacketizer::ClockRate);
		auto packetizer = std::make_shared<rtc::H264RtpPacketizer>( // 2. 创建 H.264 分片器（核心）
		    rtc::NalUnit::Separator::StartSequence, rtpConfig);
		auto srReporter =
		    std::make_shared<rtc::RtcpSrReporter>(rtpConfig); // 3.. 创建 RTCP 处理链（三个处理器）
		auto nackResponder = std::make_shared<rtc::RtcpNackResponder>();
		packetizer->addToChain(srReporter);
		packetizer->addToChain(nackResponder);

		// 之后只要执行 mTrack->send(二进制视频数据)，数据就会自动流经上述管道，变成标准 RTP
		// 包发送出去，并且自动处理 NACK 重传和 SR 报告。
		mTrack->setMediaHandler(packetizer); // 4. 挂载到媒体轨道

		// 只有 track 真的 open 了，sendFrame 才有意义。
		// 所以这里用条件变量等它打开，避免“看起来 send 了，实际链路还没 ready”。
		mTrack->onOpen([this]() {
			std::cout << "[track] open" << std::endl;
			{
				std::lock_guard<std::mutex> lock(mOpenMutex);
				mTrackOpen = true;
			}
			mOpenCv.notify_one();
		});
		mTrack->onClosed([]() { std::cout << "[track] closed" << std::endl; });

		// 这句会触发 offer 生成。
		// 之后 libdatachannel 会异步进行 ICE candidate gathering。
		mPc->setLocalDescription();
		{
			std::unique_lock<std::mutex> lock(mGatherMutex);
			mGatherCv.wait_for(lock, std::chrono::seconds(5), [this]() { return mGatherDone; });
		}

		auto local = mPc->localDescription();
		if (!local) {
			throw std::runtime_error("localDescription not ready");
		}
		/*
		    offer:
		    sdp] local offer bytes=762 v=0
		    o=rtc 3890685441 0 IN IP4 127.0.0.1
		    s=-
		    t=0 0
		    a=group:BUNDLE video
		    a=group:LS video
		    a=msid-semantic:WMS *
		    a=ice-options:ice2,trickle
		    a=fingerprint:sha-256
		   E3:E5:D3:5D:61:00:E4:56:16:FA:F3:13:98:FE:9C:7D:FC:42:23:74:C3:B6:70:C5:AD:94:DD:AF:FC:54:96:E1
		    m=video 47269 UDP/TLS/RTP/SAVPF 96
		    c=IN IP4 192.168.1.27
		    a=mid:video
		    a=sendonly
		    a=ssrc:42 cname:video-send
		    a=ssrc:42 msid:stream1 video
		    a=msid:stream1 video
		    a=rtcp-mux
		    a=rtpmap:96 H264/90000
		    a=rtcp-fb:96 nack
		    a=rtcp-fb:96 nack pli
		    a=rtcp-fb:96 goog-remb
		    a=fmtp:96 profile-level-id=42c01f;packetization-mode=1;level-asymmetry-allowed=1
		    a=setup:actpass
		    a=ice-ufrag:9nSA
		    a=ice-pwd:TSa5K8EB4P+r3DCjxqI512
		    a=candidate:1 1 UDP 2114977791 192.168.1.27 47269 typ host
		    a=end-of-candidates

		*/
		const std::string offerSdp = std::string(local.value());
		std::cout << "[sdp] local offer bytes=" << offerSdp.size() << " " << offerSdp << std::endl
		          << std::endl;

		// 这是和 SRS 的 publish API 交换 SDP：
		// - 我们把本地 offer 发给 SRS
		// - SRS 返回 answer
		// - 再 setRemoteDescription(answer)

		const std::string body = std::string("{") + "\"api\":\"" + JsonEscape(mPublishApi) + "\"," +
		                         "\"streamurl\":\"" + JsonEscape(mStreamUrl) + "\"," +
		                         "\"clientip\":null," + "\"sdp\":\"" + JsonEscape(offerSdp) + "\"}";

		/*	
			srs回应
		    v=0
			o=SRS/5.0.212(Bee) 107545981655168 2 IN IP4 0.0.0.0
			s=SRSPublishSession
			t=0 0
			a=ice-lite
			a=group:BUNDLE video
			a=msid-semantic: WMS live/test1
			m=video 9 UDP/TLS/RTP/SAVPF 96
			c=IN IP4 0.0.0.0
			a=ice-ufrag:n3y06022
			a=ice-pwd:1ca3xt626t9mr5r19833554uj00d91d0
			a=fingerprint:sha-256
			57:25:08:CC:65:14:33:62:49:D6:46:D0:FC:8A:FC:D0:60:A4:09:EF:1D:AA:30:DC:C8:B4:16:E8:AB:18:5A:79
			a=setup:passive
			a=mid:video
			a=recvonly
			a=rtcp-mux
			a=rtcp-rsize
			a=rtpmap:96 H264/90000
			a=rtcp-fb:96 nack
			a=rtcp-fb:96 nack pli
			a=fmtp:96 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42c01f
			a=candidate:0 1 udp 2130706431 192.168.1.27 8000 typ host generation 0

		*/
		std::cout << "[http] POST " << mPublishApi << std::endl;
		const std::string response = HttpPostJson(mPublishApi, body);
		std::cout << "[http] response bytes=" << response.size() << " " << response << std::endl
		          << std::endl;
		if (ExtractCode(response) != 0) {
			throw std::runtime_error("SRS publish failed, response=" + response);
		}

		mPc->setRemoteDescription(rtc::Description(ExtractSdp(response), "answer"));
		std::cout << "[sdp] answer applied" << std::endl;
		/* 流程 */
			// 1	mPc->setLocalDescription(offer)	你生成本地的Offer（你的能力）
			// 2	发送Offer到SRS	HTTP POST
			// 3	SRS返回Answer	包含SDP字符串
			// 4	setRemoteDescription(answer) ← 你问的这行	告诉连接：对方SRS的能力是这样的
			// 5	ICE 连接建立	双方开始尝试打洞连通
			// 6	DTLS 握手	加密通道建立
			// 7	开始发送/接收媒体	RTP包在UDP上传输

		// 等到这里，基本就能确认“发送通道已经打通”。
		{
			std::unique_lock<std::mutex> lock(mOpenMutex);
			if (!mOpenCv.wait_for(lock, std::chrono::seconds(10),
			                      [this]() { return mTrackOpen; })) {
				throw std::runtime_error("track did not open");
			}
		}
	}

	// 这是“摄像头/编码器回调版本”的核心发送函数。
	// 只要你手里已经是一帧完整的 Annex-B H264：
	// - data 指向帧数据
	// - size 是这一帧长度
	// - timestampUs 是这一帧的时间戳（微秒）
	// 就可以直接调用它。
	//
	// 这里最关键的理解是：
	// sendFrame 要的不是“任意一段 H264 数据”，而最好是一整帧。
	// 如果你的编码器回调本来就是“每次给一帧”，那就最省事。
	// 这也是为什么摄像头回调版会比文件版短很多。
	bool SendEncodedAnnexBFrame(const uint8_t *data, size_t size, uint64_t timestampUs) {
		if (!mTrack || !mTrack->isOpen() || !data || size == 0) {
			return false;
		}

		// 这里把裸指针包装成 rtc::binary。
		// rtc::binary 本质上就是 libdatachannel 里常用的字节数组类型。
		rtc::binary frame(reinterpret_cast<const std::byte *>(data),
		                  reinterpret_cast<const std::byte *>(data) + size);

		// timestampUs 用来告诉 packetizer/发送链路“这一帧的显示时间”。
		// 即便只是最小示例，也最好传一个连续递增的时间戳。
		mTrack->sendFrame(frame,
		                  rtc::FrameInfo{std::chrono::duration<double, std::micro>(timestampUs)});
		return true;
	}

	// 关闭顺序也值得记一下：
	// - 先关 track
	// - 再关 PeerConnection
	// 这样更符合“先停媒体，再停连接”的直觉。
	void Stop() {
		if (mTrack) {
			mTrack->close();
		}
		if (mPc) {
			mPc->close();
		}
	}

private:
	std::string mPublishApi;
	std::string mStreamUrl;
	int mPayloadType = 96;

	std::shared_ptr<rtc::PeerConnection> mPc;
	std::shared_ptr<rtc::Track> mTrack;

	std::mutex mGatherMutex;
	std::condition_variable mGatherCv;
	bool mGatherDone = false;

	std::mutex mOpenMutex;
	std::condition_variable mOpenCv;
	bool mTrackOpen = false;
};

void PrintUsage(const char *argv0) {
	std::cout << "Usage:\n  " << argv0 << " <publishApi> <streamUrl> [payloadType]\n\n"
	          << "Example:\n  " << argv0 << " http://192.168.1.27:1985/rtc/v1/publish/"
	          << " webrtc://192.168.1.27/live/test 96\n";
}

} // namespace

int main(int argc, char **argv) {
	// 这份模板的 main 非常短，故意就是为了让你把注意力放在：
	// - 建链
	// - 真正的发送入口
	// 而不是被“文件解析”分散掉。
	if (argc < 3) {
		PrintUsage(argv[0]);
		return 1;
	}

	const std::string publishApi = argv[1];
	const std::string streamUrl = argv[2];
	const int payloadType = argc > 3 ? std::stoi(argv[3]) : 96;

	try {
		std::signal(SIGINT, HandleSignal);
		std::signal(SIGTERM, HandleSignal);
		rtc::InitLogger(rtc::LogLevel::Info);

		PushSession session(publishApi, streamUrl, payloadType);
		session.Start();

		std::cout << "[demo] 连接已经建立。" << std::endl;
		std::cout << "[demo] 现在请把你的编码器回调接到: SendEncodedAnnexBFrame(data, size, tsUs)"
		          << std::endl;
		std::cout << "[demo] 这份模板本身不主动产生视频，只负责展示最小推流主链路。" << std::endl;
		std::cout << "[demo] 学习时你可以把断点打在 Start() 和 SendEncodedAnnexBFrame() 上。"
		          << std::endl;

		// 这里故意不造假帧，也不读文件。
		// 原因是这份代码的目的就是告诉你：
		// 如果摄像头编码器已经给了你“逐帧回调”，那最小发送入口只有 SendEncodedAnnexBFrame。
		while (!gStopRequested.load()) {
			std::this_thread::sleep_for(std::chrono::seconds(1));
		}

		session.Stop();
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
	} catch (const std::exception &e) {
		std::cerr << "Error: " << e.what() << std::endl;
		return 1;
	}

	return 0;
}
