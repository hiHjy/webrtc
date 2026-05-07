#include "rtc/rtc.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <fstream>
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

// 这份代码是“拉流版”的教学示例。
//
// 学它的时候，建议你按下面的顺序看：
// 1. main() 里怎么创建 PeerConnection
// 2. addTrack(RecvOnly) 是怎么表达“我要收音视频”的
// 3. setLocalDescription -> POST /rtc/v1/play/ -> setRemoteDescription
// 4. onTrack / onFrame 是怎样拿到远端媒体的
//
// 它的定位是：
// - 帮你理解最小 WebRTC play 链路
// - 帮你观察 RTP/H264 是怎样进来的
// - 方便你后面自己手写一遍

// 这个文件是一个“最小可运行”的 SRS WebRTC 拉流示例：
// 1) 用 libdatachannel 创建 RecvOnly 的音视频 PeerConnection
// 2) 生成本地 offer SDP
// 3) 通过 HTTP POST 把 offer 发给 SRS /rtc/v1/play/
// 4) 解析 SRS 返回的 answer SDP，并 setRemoteDescription
// 5) 在 onTrack/onMessage 里接收远端 RTP
//
// 重点：这里为了减少依赖，没有用 libcurl，而是用最基础的 TCP socket 发 HTTP。
// 仅支持 http://（不支持 https://），适合本机测试 SRS（如 127.0.0.1:1985）。

namespace {

// 下面这些辅助函数基本都围绕“和 SRS 的 HTTP API 换 SDP”展开。
// 拉流和推流都会用到这套思路，所以你把这里看懂，之后看 push 会轻松很多。

// 把字符串转为 JSON string 可安全携带的内容（转义 \ " \n 等）。
std::string JsonEscape(const std::string &input) {
	std::string out;
	out.reserve(input.size() + 64);
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
		case '\t':
			out += "\\t";
			break;
		default:
			out += c;
			break;
		}
	}
	return out;
}

// 反向把 JSON string 的转义序列还原回普通字符串。
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
		case '/':
			out.push_back('/');
			break;
		default:
			out.push_back(n);
			break;
		}
	}
	return out;
}

// 从 SRS 的 JSON 响应里提取 "code" 字段。
// SRS 约定 code=0 代表成功。
int ExtractCode(const std::string &jsonText) {
	static const std::regex codeRe("\"code\"\\s*:\\s*(-?\\d+)");
	std::smatch m;
	if (!std::regex_search(jsonText, m, codeRe)) {
		throw std::runtime_error("SRS response has no code field");
	}
	return std::stoi(m[1].str());
}

// 从 SRS 的 JSON 响应里提取 "sdp" 字段（answer SDP）。
std::string ExtractSdp(const std::string &jsonText) {
	static const std::regex sdpRe("\"sdp\"\\s*:\\s*\"((?:\\\\.|[^\"\\\\])*)\"");
	std::smatch m;
	if (!std::regex_search(jsonText, m, sdpRe)) {
		throw std::runtime_error("SRS response has no sdp field");
	}
	return JsonUnescape(m[1].str());
}

// 对 http://host:port/path 做最小解析。
struct ParsedUrl {
	std::string host;
	std::string port;
	std::string path;
};

ParsedUrl ParseHttpUrl(const std::string &url) {
	static const std::regex re(R"(^http://([^/:]+)(?::(\d+))?(\/.*)?$)");
	std::smatch m;
	if (!std::regex_match(url, m, re)) {
		throw std::runtime_error("Only http:// URL is supported, got: " + url);
	}
	ParsedUrl out;
	out.host = m[1].str();
	out.port = m[2].matched ? m[2].str() : "80";
	out.path = m[3].matched ? m[3].str() : "/";
	return out;
}

// 处理 HTTP chunked body（Transfer-Encoding: chunked）。
// SRS 通常会直接返回普通 body，但这里做兼容，避免以后踩坑。
std::string DecodeChunkedBody(const std::string &chunked) {
	std::string out;
	size_t pos = 0;
	while (true) {
		const size_t lineEnd = chunked.find("\r\n", pos);
		if (lineEnd == std::string::npos) {
			throw std::runtime_error("Invalid chunked response");
		}

		const std::string sizeHex = chunked.substr(pos, lineEnd - pos);
		const size_t chunkSize = std::stoul(sizeHex, nullptr, 16);
		pos = lineEnd + 2;

		if (chunkSize == 0) {
			break;
		}
		if (pos + chunkSize + 2 > chunked.size()) {
			throw std::runtime_error("Truncated chunked response");
		}
		out.append(chunked, pos, chunkSize);
		pos += chunkSize;
		if (chunked.compare(pos, 2, "\r\n") != 0) {
			throw std::runtime_error("Invalid chunk separator");
		}
		pos += 2;
	}
	return out;
}

// 最小 HTTP POST(JSON)：
// - DNS 解析 + TCP 连接
// - 发送 HTTP 请求
// - 读取响应并拆出 body
// - 校验状态码 2xx
std::string HttpPostJson(const std::string &url, const std::string &body) {
	// 这部分虽然是“socket 发 HTTP”，但在这个示例里的意义更像是：
	// 帮你看到 WebRTC 并不是只有 PeerConnection，
	// 还需要一条单独的信令通道把 offer/answer 交换出去。
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
		throw std::runtime_error("connect failed to " + parsed.host + ":" + parsed.port);
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

	static const std::regex statusRe(R"(^HTTP/\d\.\d\s+(\d+))");
	std::smatch sm;
	if (!std::regex_search(headers, sm, statusRe)) {
		throw std::runtime_error("Cannot parse HTTP status");
	}
	const int status = std::stoi(sm[1].str());
	if (status < 200 || status >= 300) {
		throw std::runtime_error("HTTP status is " + std::to_string(status) +
		                         ", body: " + responseBody);
	}

	static const std::regex chunkedRe("Transfer-Encoding:\\s*chunked", std::regex::icase);
	if (std::regex_search(headers, chunkedRe)) {
		responseBody = DecodeChunkedBody(responseBody);
	}

	return responseBody;
}

void BindTrackDebugCallbacks(const std::shared_ptr<rtc::Track> &track, const std::string &tag) {
	// 这组回调专门用于“观察媒体是否真的进来了”。
	// 学习阶段建议保留这些日志，它能帮助你区分：
	// - 链接是否建立
	// - track 是否打开
	// - 远端是否真正开始发数据
	track->onOpen([track, tag]() {
		std::cout << "[" << tag << "] 可以开始接收数据了，开始接收 RTP 数据包" << std::endl;
		std::cout << "[" << tag << " " << track->mid() << "] open" << std::endl;
	});
	track->onClosed([track, tag]() { // 远端停止推流 或 连接断开 时，会触发该回调
		std::cout << "[" << tag << "] 远端停止推流或连接断开，会触发该回调" << std::endl;
		std::cout << "[" << tag << " " << track->mid() << "] closed" << std::endl;
	});
	// track->onMessage( // 持续触发（每收到一个包）
	//     [track, tag](rtc::binary message) {
	// 	    std::cout << "[" << tag << "] 收到 RTP 数据包，大小为: " << message.size() << std::endl;
	// 	    std::cout << "[" << tag << " " << track->mid() << "] rtp bytes=" << message.size()
	// 	              << std::endl;
	//     },
	//     nullptr);
}

std::string H264NaluTypeName(uint8_t type) {
	// 这个函数只是把 NALU type 转成人类好读的名字，
	// 方便你调试 dump.h264 时不至于只看到一堆数字。
	switch (type) {
	case 1:
		return "non-IDR slice";
	case 5:
		return "IDR slice";
	case 6:
		return "SEI";
	case 7:
		return "SPS";
	case 8:
		return "PPS";
	case 9:
		return "AUD";
	default:
		return "type=" + std::to_string(type);
	}
}

void DumpH264FrameInfo(const rtc::binary &frame) {
	// 这个函数的作用是“把一帧 Annex-B H264 再拆开看看里面有什么 NALU”。
	// 学习时非常有用，因为你能直接看到：
	// - 有没有 SPS/PPS
	// - 什么时候出现 IDR
	// - 一帧里到底有几个 NALU
	size_t i = 0;
	int naluIndex = 0;

	while (i + 4 <= frame.size()) {
		size_t startCodeSize = 0;
		if (frame[i] == std::byte{0} && frame[i + 1] == std::byte{0} &&
		    frame[i + 2] == std::byte{0} && frame[i + 3] == std::byte{1}) {
			startCodeSize = 4;
		} else if (i + 3 <= frame.size() && frame[i] == std::byte{0} &&
		           frame[i + 1] == std::byte{0} && frame[i + 2] == std::byte{1}) {
			startCodeSize = 3;
		} else {
			++i;
			continue;
		}

		const size_t naluStart = i + startCodeSize;
		size_t next = naluStart;
		while (next + 4 <= frame.size()) {
			const bool longStart = frame[next] == std::byte{0} && frame[next + 1] == std::byte{0} &&
			                       frame[next + 2] == std::byte{0} &&
			                       frame[next + 3] == std::byte{1};
			const bool shortStart =
			    frame[next] == std::byte{0} && frame[next + 1] == std::byte{0} &&
			    frame[next + 2] == std::byte{1};
			if (longStart || shortStart) {
				break;
			}
			++next;
		}
		if (next + 4 > frame.size()) {
			next = frame.size();
		}

		if (naluStart < frame.size()) {
			const uint8_t naluHeader = std::to_integer<uint8_t>(frame[naluStart]);
			const uint8_t naluType = naluHeader & 0x1F;
			std::cout << "  nalu[" << naluIndex << "] startCode=" << startCodeSize
			          << " type=" << static_cast<int>(naluType)
			          << " (" << H264NaluTypeName(naluType) << ")"
			          << " size=" << (next - naluStart) << std::endl;
			++naluIndex;
		}

		i = next;
	}
}

} // namespace

int main(int argc, char **argv) {
	// 可通过命令行覆盖：
	// argv[1] = SRS play API，如 http://127.0.0.1:1985/rtc/v1/play/
	// argv[2] = streamurl，如 webrtc://127.0.0.1/live/livestream
	const std::string playApi = argc > 1 ? argv[1] : "http://127.0.0.1:1985/rtc/v1/play/";
	const std::string streamUrl = argc > 2 ? argv[2] : "webrtc://127.0.0.1/live/livestream";

	try {
		// 打开日志，学习阶段建议 Info；线上可以调低。
		rtc::InitLogger(rtc::LogLevel::Info);

		// 1) 创建 PeerConnection
		auto pc = std::make_shared<rtc::PeerConnection>();
		// 存本地 addTrack 返回值，避免被析构（并消除 [[nodiscard]] 警告）。
		std::vector<std::shared_ptr<rtc::Track>> localRecvTracks;
		// 保存远端 track，确保生命周期覆盖整个接收过程。
		std::vector<std::shared_ptr<rtc::Track>> remoteTracks;

		// 2) 注册状态日志，便于理解连接进度
		// 建议你调试时把断点打在这些回调里，结合终端日志一起看。
		pc->onStateChange([](rtc::PeerConnection::State state) {
			std::cout << "[pc] state: " << state << std::endl;
		});
		pc->onIceStateChange([](rtc::PeerConnection::IceState state) {
			std::cout << "[pc] ice: " << state << std::endl;
		});
		pc->onGatheringStateChange([](rtc::PeerConnection::GatheringState state) {
			std::cout << "[pc] gathering: " << state << std::endl;
		});

		// 3) 收到远端 track（音频/视频）时，挂接回调观察数据流入。
		// 注意：
		// 有些协商路径下，媒体也可能主要从你 addTrack(RecvOnly) 返回的对象上观察到。
		// 所以这个 onTrack 很重要，但不是你唯一的观察入口。
		pc->onTrack([&remoteTracks](const std::shared_ptr<rtc::Track> &track) {
			std::cout << "[pc] got remote track: " << track->mid() << std::endl;
			BindTrackDebugCallbacks(track, "remote-onTrack");
			remoteTracks.push_back(track); // 保存音频和视频 track
		});

		// 4) 声明我们只“接收”音频：RecvOnly
		// payload type 111 是 Opus 常见 PT（SRS 默认路径下通常可协商）。
		// 这里本质上是在告诉对端：
		// “我希望收音频，而且方向是只收不发。”
		rtc::Description::Audio audio("audio", rtc::Description::Direction::RecvOnly);
		audio.addOpusCodec(111);
		localRecvTracks.push_back(pc->addTrack(audio));
		BindTrackDebugCallbacks(localRecvTracks.back(), "local-audio");

		// 5) 声明我们只“接收”视频：RecvOnly
		// 这里先只保留 H264，方便测试 RTP 重组和 ffplay 播放。
		// 如果你只想研究 H264，先别同时加 VP8/VP9，这样日志最清晰。
		rtc::Description::Video video("video", rtc::Description::Direction::RecvOnly);
		video.addH264Codec(102);
		auto videoTrack = pc->addTrack(video);

		// H264RtpDepacketizer 的意义：
		// - onMessage() 看到的是原始 RTP 包
		// - 挂上 depacketizer 后，onFrame() 看到的是重组后的 H264 帧
		videoTrack->setMediaHandler(std::make_shared<rtc::H264RtpDepacketizer>());
		videoTrack->chainMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());

		auto h264File = std::make_shared<std::ofstream>(
		    "dump.h264", std::ios_base::out | std::ios_base::binary | std::ios_base::trunc);
		if (!h264File->is_open()) {
			throw std::runtime_error("failed to open dump.h264");
		}
		std::cout << "[file] writing H264 Annex-B stream to dump.h264" << std::endl;

		videoTrack->onFrame([h264File](rtc::binary frame, rtc::FrameInfo info) {
			// 一旦进到这里，就说明：
			// - RTP 已经收到
			// - H264 已经完成了重组
			// 这通常比只看到 connected 更能证明“真的拉到视频了”。
			std::cout << "[h264] got frame size=" << frame.size()
			          << " timestamp=" << info.timestamp;
			if (info.timestampSeconds) {
				std::cout << " timestampSeconds=" << info.timestampSeconds->count();
			}
			std::cout << std::endl;

			if (frame.size() >= 4) {
				std::cout << "[h264] head bytes="
				          << static_cast<int>(std::to_integer<uint8_t>(frame[0])) << " "
				          << static_cast<int>(std::to_integer<uint8_t>(frame[1])) << " "
				          << static_cast<int>(std::to_integer<uint8_t>(frame[2])) << " "
				          << static_cast<int>(std::to_integer<uint8_t>(frame[3])) << std::endl;
			}

			DumpH264FrameInfo(frame);

			// 直接落成 .h264，方便你用 ffplay / ffmpeg 做二次验证。
			h264File->write(reinterpret_cast<const char *>(frame.data()), frame.size());
			h264File->flush();
		});

		localRecvTracks.push_back(videoTrack);
		BindTrackDebugCallbacks(localRecvTracks.back(), "local-video");

		// 6) 等 ICE gathering 完成后再取 localDescription，
		// 这样 offer 里更完整（包含候选）。
		// 创建同步工具（用于线程间通信）
		std::mutex mu;              // 互斥锁（保护共享变量）
		std::condition_variable cv; // 条件变量（等待/通知机制）
		bool gatherDone = false;    // 标志位（是否收集完成）

		// 监听收集状态变化
		pc->onGatheringStateChange([&](rtc::PeerConnection::GatheringState state) {
			std::cout << "[pc] gathering: " << state << std::endl;

			// 当状态变为 Complete（完成）时
			if (state == rtc::PeerConnection::GatheringState::Complete) {
				{
					std::lock_guard<std::mutex> lock(mu); // 加锁保护
					gatherDone = true;                    // 设置标志
				} // 离开作用域自动解锁
				cv.notify_one(); // 通知等待的线程
			}
		});

		// 触发创建 offer
		pc->setLocalDescription();//告诉对方"我的电话号码是 xxx"

		// 最多等 5 秒（防止永久卡住）。
		{
			std::unique_lock<std::mutex> lock(mu);
			cv.wait_for(lock, std::chrono::seconds(5), [&gatherDone]() { return gatherDone; }); //总等待时间 5 秒，如果超过 5 秒，会退出wait_for
		}

		// 7) 取出 offer SDP
		auto local = pc->localDescription();
		if (!local) {
			throw std::runtime_error("localDescription not ready");
		}
		const std::string offerSdp = std::string(local.value());
		std::cout << "[sdp] local offer bytes: " << offerSdp.size() << std::endl;

		// 8) 组装 SRS /rtc/v1/play/ 需要的 JSON body。
		// 字段格式与 SRS HTTP API 约定一致：api / streamurl / clientip / sdp
    /*
      // 示例 JSON body：
      {
        "api": "play",
        "streamurl": "webrtc://192.168.1.27/live/test",
        "clientip": null,
        "sdp": "v=0\r\no=- 123456 2 IN IP4 127.0.0.1\r\n..."
      }
    */////
		const std::string body = std::string("{") + "\"api\":\"" + JsonEscape(playApi) + "\"," +
		                         "\"streamurl\":\"" + JsonEscape(streamUrl) + "\"," +
		                         "\"clientip\":null," + "\"sdp\":\"" + JsonEscape(offerSdp) + "\"}";

		// 9) POST 到 SRS，拿 answer
		std::cout << "[http] POST " << playApi << std::endl;
		const std::string response = HttpPostJson(playApi, body);
		std::cout << "[http] response bytes: " << response.size() << ", " << response << std::endl << std::endl;

		// 10) 校验 SRS 业务返回码
		const int code = ExtractCode(response);
		if (code != 0) {
			throw std::runtime_error("SRS play failed, code=" + std::to_string(code) +
			                         ", response=" + response);
		}

		// 11) 应用远端 answer，连接正式建立，开始收流
		const std::string answerSdp = ExtractSdp(response);
		rtc::Description answer(answerSdp, "answer");
		pc->setRemoteDescription(answer); //记下对方的电话号码

		std::cout << "[sdp] remote answer applied, receiving..." << std::endl;

		// 12) 保持进程存活，持续接收 onTrack/onMessage 回调
		// 这里故意用最笨的 while(true)，因为它最直白：
		// 示例程序的重点是“通信链路和回调”，不是退出框架。
		while (true) {
			std::this_thread::sleep_for(std::chrono::seconds(1));
		}

	} catch (const std::exception &e) {
		std::cerr << "Error: " << e.what() << std::endl;
		return 1;
	}

	return 0;
}
