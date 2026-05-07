#include "rtc/rtc.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <netdb.h>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

// 这份代码是“文件推流版”的最小教学示例。
//
// 它做的事情分成三层：
// 1. WebRTC 信令层：
//    - 创建 PeerConnection
//    - 生成 offer
//    - 通过 HTTP POST 发给 SRS /rtc/v1/publish/
//    - 应用 answer
//
// 2. H264 文件处理层：
//    - 读取一个 Annex-B .h264 文件
//    - 按 start code 拆成多个 NALU
//    - 再把属于同一帧的 NALU 组装成 Access Unit
//
// 3. 发送层：
//    - 用 H264RtpPacketizer 把一帧 Annex-B H264 打包成 RTP
//    - 通过 track->sendFrame(...) 发给 SRS
//
// 你后面如果要从“文件版”切到“摄像头编码输出版”，
// 真正最值得保留的其实是：
//   track->sendFrame(frame, FrameInfo{timestamp})
// 和前面的信令逻辑。
// 文件解析这部分只是因为“大 .h264 文件本身不是一帧一帧摆好的”，
// 所以我们才需要额外拆帧。

namespace {

// 退出标志：
// - 信号处理函数只改这个标志
// - 真正的资源关闭仍然放在主线程中做
std::atomic<bool> gStopRequested{false};

void HandleSignal(int) {
	gStopRequested.store(true);
}

struct ParsedUrl {
	std::string host;
	std::string port;
	std::string path;
};

// 一个 NALU 的最小描述：
// - annexb: 含 00 00 00 01 起始码的原始字节
// - type: H264 NALU type，方便后面判断 SPS/PPS/IDR
struct Nalu {
	rtc::binary annexb;
	uint8_t type = 0;
};

// 一个 Access Unit 可以粗略理解成“一帧要送给 sendFrame 的内容”。
// 对 H264 来说，一帧往往不是只有一个 NALU：
// - 关键帧可能会带 SPS / PPS / IDR
// - 普通帧可能只有 non-IDR slice
struct AccessUnit {
	rtc::binary annexb;
	bool keyFrame = false;
};

// 下面这几个 JSON / HTTP 辅助函数和 pull 版几乎一样，
// 因为 SRS 的 publish/play 都是“HTTP 信令 + WebRTC 媒体”这套结构。
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

int ExtractCode(const std::string &jsonText) {
	static const std::regex codeRe("\\\"code\\\"\\s*:\\s*(-?\\d+)");
	std::smatch m;
	if (!std::regex_search(jsonText, m, codeRe)) {
		throw std::runtime_error("SRS response has no code field");
	}
	return std::stoi(m[1].str());
}

std::string ExtractSdp(const std::string &jsonText) {
	static const std::regex sdpRe("\\\"sdp\\\"\\s*:\\s*\\\"((?:\\\\.|[^\\\"\\\\])*)\\\"");
	std::smatch m;
	if (!std::regex_search(jsonText, m, sdpRe)) {
		throw std::runtime_error("SRS response has no sdp field");
	}
	return JsonUnescape(m[1].str());
}

ParsedUrl ParseHttpUrl(const std::string &url) {
	static const std::regex re(R"(^http://([^/:]+)(?::(\d+))?(\/.*)?$)");
	std::smatch m;
	if (!std::regex_match(url, m, re)) {
		throw std::runtime_error("Only http:// URL is supported");
	}
	return ParsedUrl{m[1].str(), m[2].matched ? m[2].str() : "80",
	                 m[3].matched ? m[3].str() : "/"};
}

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

rtc::binary ReadBinaryFile(const std::string &path) {
	// 文件版的第一步，就是先把整个 .h264 裸流读到内存里。
	// 这样后面拆 start code 时逻辑最清楚。
	std::ifstream ifs(path, std::ios::binary);
	if (!ifs) {
		throw std::runtime_error("failed to open file: " + path);
	}
	ifs.seekg(0, std::ios::end);
	const auto size = static_cast<size_t>(ifs.tellg());
	ifs.seekg(0, std::ios::beg);
	rtc::binary data(size);
	ifs.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(size));
	if (!ifs) {
		throw std::runtime_error("failed to read file: " + path);
	}
	return data;
}

bool IsStartCode3(const rtc::binary &data, size_t i) {
	return i + 3 <= data.size() && data[i] == std::byte{0} && data[i + 1] == std::byte{0} &&
	       data[i + 2] == std::byte{1};
}

bool IsStartCode4(const rtc::binary &data, size_t i) {
	return i + 4 <= data.size() && data[i] == std::byte{0} && data[i + 1] == std::byte{0} &&
	       data[i + 2] == std::byte{0} && data[i + 3] == std::byte{1};
}

std::vector<Nalu> SplitAnnexBNalus(const rtc::binary &data) {
	// 这一步只做一件事：
	// 从“大文件”里找到每个 NALU 的边界。
	//
	// 注意这里还没做“按帧拆分”，只是先得到：
	//   [NALU1][NALU2][NALU3]...
	std::vector<std::pair<size_t, size_t>> spans;
	for (size_t i = 0; i + 3 < data.size();) {
		size_t sc = 0;
		if (IsStartCode4(data, i)) {
			sc = 4;
		} else if (IsStartCode3(data, i)) {
			sc = 3;
		}
		if (sc == 0) {
			++i;
			continue;
		}
		const size_t nalStart = i + sc;
		i = nalStart;
		size_t next = i;
		while (next + 3 < data.size() && !IsStartCode3(data, next) && !IsStartCode4(data, next)) {
			++next;
		}
		spans.emplace_back(nalStart, next);
		i = next;
	}

	std::vector<Nalu> nalus;
	nalus.reserve(spans.size());
	for (const auto &span : spans) {
		if (span.second <= span.first) {
			continue;
		}
		Nalu nalu;
		nalu.annexb = {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}};
		nalu.annexb.insert(nalu.annexb.end(), data.begin() + static_cast<std::ptrdiff_t>(span.first),
		                   data.begin() + static_cast<std::ptrdiff_t>(span.second));
		nalu.type = static_cast<uint8_t>(nalu.annexb[4]) & 0x1F;
		nalus.push_back(std::move(nalu));
	}
	return nalus;
}

rtc::binary RemoveEmulationPrevention(const rtc::binary &bytes, size_t offset) {
	// H264 码流里可能插入 0x03 作为 emulation prevention byte。
	// 如果你要解析 slice header / exp-golomb，就得先把它移除。
	rtc::binary out;
	out.reserve(bytes.size() - offset);
	int zeroCount = 0;
	for (size_t i = offset; i < bytes.size(); ++i) {
		const uint8_t v = static_cast<uint8_t>(bytes[i]);
		if (zeroCount == 2 && v == 0x03) {
			zeroCount = 0;
			continue;
		}
		out.push_back(bytes[i]);
		if (v == 0x00) {
			++zeroCount;
		} else {
			zeroCount = 0;
		}
	}
	return out;
}

class BitReader {
public:
	// 这个类只服务于一个很小的需求：
	// 解析 slice 头里的 first_mb_in_slice，用它判断“是不是一帧的开头”。
	explicit BitReader(const rtc::binary &data) : mData(data) {}

	uint32_t ReadBits(size_t count) {
		uint32_t value = 0;
		for (size_t i = 0; i < count; ++i) {
			value <<= 1;
			if (mBitOffset / 8 >= mData.size()) {
				return value;
			}
			const uint8_t byte = static_cast<uint8_t>(mData[mBitOffset / 8]);
			value |= (byte >> (7 - (mBitOffset % 8))) & 0x01;
			++mBitOffset;
		}
		return value;
	}

	uint32_t ReadUE() {
		size_t zeros = 0;
		while (ReadBits(1) == 0 && mBitOffset / 8 <= mData.size()) {
			++zeros;
		}
		if (zeros == 0) {
			return 0;
		}
		const uint32_t suffix = ReadBits(zeros);
		return ((1u << zeros) - 1u) + suffix;
	}

private:
	const rtc::binary &mData;
	size_t mBitOffset = 0;
};

bool IsVcl(uint8_t type) {
	// H264 中 1~5 是视频编码层(VCL)的 NALU，也就是和真正画面最直接相关的 slice。
	return type >= 1 && type <= 5;
}

bool AccessUnitHasNaluType(const AccessUnit &au, uint8_t type) {
	// 小工具函数：看看一个待发送帧里是否已经包含某类 NALU。
	// 这里主要用来避免重复插入 SPS/PPS。
	for (size_t i = 0; i + 4 < au.annexb.size(); ++i) {
		if ((i + 4 < au.annexb.size()) && au.annexb[i] == std::byte{0} &&
		    au.annexb[i + 1] == std::byte{0} && au.annexb[i + 2] == std::byte{0} &&
		    au.annexb[i + 3] == std::byte{1}) {
			return (static_cast<uint8_t>(au.annexb[i + 4]) & 0x1F) == type;
		}
	}
	return false;
}

bool IsFirstMbInSliceZero(const Nalu &nalu) {
	// first_mb_in_slice == 0 往往意味着“一个新 slice / 新帧的开始”。
	// 所以这里是文件拆帧逻辑最关键的判断之一。
	if (!IsVcl(nalu.type) || nalu.annexb.size() <= 5) {
		return false;
	}
	const rtc::binary rbsp = RemoveEmulationPrevention(nalu.annexb, 5);
	BitReader reader(rbsp);
	return reader.ReadUE() == 0;
}

std::string DeriveH264Fmtp(const std::vector<Nalu> &nalus) {
	// 从 SPS 里拿 profile/level，拼成 fmtp，尽量和真实文件参数对齐。
	// 如果拿不到 SPS，就退回到一个常见的 baseline 默认值。
	for (const auto &nalu : nalus) {
		if (nalu.type != 7 || nalu.annexb.size() < 8) {
			continue;
		}
		const uint8_t profileIdc = static_cast<uint8_t>(nalu.annexb[5]);
		const uint8_t profileIop = static_cast<uint8_t>(nalu.annexb[6]);
		const uint8_t levelIdc = static_cast<uint8_t>(nalu.annexb[7]);
		char buf[128];
		std::snprintf(buf, sizeof(buf),
		              "profile-level-id=%02x%02x%02x;packetization-mode=1;level-asymmetry-allowed=1",
		              profileIdc, profileIop, levelIdc);
		return std::string(buf);
	}
	return "profile-level-id=42c01f;packetization-mode=1;level-asymmetry-allowed=1";
}

std::vector<AccessUnit> BuildAccessUnits(const std::vector<Nalu> &nalus) {
	// 这一步才是真正把“很多 NALU”变成“很多帧”。
	//
	// 为什么一定要有这一步？
	// 因为 sendFrame 想要的是“一帧”，而不是一个大文件，也不是随便几段 NALU。
	// Annex-B 只告诉你 NALU 怎么分隔，不自动告诉你帧边界在哪里。
	std::vector<AccessUnit> units;
	AccessUnit current;
	bool hasVcl = false;
	Nalu lastSps;
	Nalu lastPps;

	auto flushCurrent = [&]() {
		// 当前帧收集完成，推入结果数组。
		if (!current.annexb.empty()) {
			units.push_back(std::move(current));
			current = AccessUnit{};
			hasVcl = false;
		}
	};

	for (const auto &nalu : nalus) {
		// 先记住最新 SPS/PPS，后面碰到 IDR 时可能要补进去。
		if (nalu.type == 7) {
			lastSps = nalu;
		}
		if (nalu.type == 8) {
			lastPps = nalu;
		}

		if (nalu.type == 9 && hasVcl) {
			// AUD 可以理解为“访问单元分隔提示”。
			// 如果当前已经收过 VCL，那它往往意味着当前帧该结束了。
			flushCurrent();
		}

		if (IsVcl(nalu.type) && hasVcl && IsFirstMbInSliceZero(nalu)) {
			// 遇到新一帧的起点，把上一帧先提交。
			flushCurrent();
		}

		if (nalu.type == 5) {
			// IDR 是关键帧。
			// 很多播放器/服务端希望关键帧附近能看到 SPS/PPS，
			// 所以这里尽量把最近的 SPS/PPS 补到 IDR 前面。
			const bool alreadyHasSps = AccessUnitHasNaluType(current, 7);
			if (!alreadyHasSps && !lastSps.annexb.empty()) {
				current.annexb.insert(current.annexb.end(), lastSps.annexb.begin(), lastSps.annexb.end());
			}
			const bool alreadyHasPps = AccessUnitHasNaluType(current, 8);
			if (!alreadyHasPps && !lastPps.annexb.empty()) {
				current.annexb.insert(current.annexb.end(), lastPps.annexb.begin(), lastPps.annexb.end());
			}
			current.keyFrame = true;
		}

		// 把当前 NALU 追加到当前帧里。
		current.annexb.insert(current.annexb.end(), nalu.annexb.begin(), nalu.annexb.end());
		if (nalu.type == 5) {
			current.keyFrame = true;
		}
		if (IsVcl(nalu.type)) {
			hasVcl = true;
		}
	}
	flushCurrent();
	return units;
}

void PrintUsage(const char *argv0) {
	std::cout << "Usage:\n  " << argv0
	          << " <publishApi> <streamUrl> <annexb.h264> <fps> [payloadType]\n\n"
	          << "Example:\n  " << argv0
	          << " http://192.168.1.27:1985/rtc/v1/publish/"
	          << " webrtc://192.168.1.27/live/test"
	          << " /home/hjy/libdatachannel/examples/test/build/1_baseline.h264 25 96\n";
}

} // namespace

int main(int argc, char **argv) {
	// 参数：
	// 1. SRS publish API
	// 2. streamurl
	// 3. 一个 Annex-B .h264 文件
	// 4. 发送帧率
	// 5. 可选 payload type
	if (argc < 5) {
		PrintUsage(argv[0]);
		return 1;
	}

	const std::string publishApi = argv[1];
	const std::string streamUrl = argv[2];
	const std::string h264Path = argv[3];
	const int fps = std::stoi(argv[4]);
	const int payloadType = argc > 5 ? std::stoi(argv[5]) : 96;

	try {
		std::signal(SIGINT, HandleSignal);
		std::signal(SIGTERM, HandleSignal);
		rtc::InitLogger(rtc::LogLevel::Info);

		const rtc::binary fileData = ReadBinaryFile(h264Path);
		const auto nalus = SplitAnnexBNalus(fileData);
		const auto accessUnits = BuildAccessUnits(nalus);
		if (nalus.empty() || accessUnits.empty()) {
			throw std::runtime_error("no valid H264 access units found in file");
		}

		// 这里把文件基本信息打印出来，是为了帮助你“带着观察去调试”：
		// - NALU 有多少
		// - AU 有多少
		// - fmtp 推导成了什么
		const std::string h264Fmtp = DeriveH264Fmtp(nalus);
		std::cout << "[file] input=" << h264Path << std::endl;
		std::cout << "[file] size=" << fileData.size() << " bytes" << std::endl;
		std::cout << "[file] nalus=" << nalus.size() << std::endl;
		std::cout << "[file] accessUnits=" << accessUnits.size() << std::endl;
		std::cout << "[file] fmtp=" << h264Fmtp << std::endl;

		const rtc::SSRC ssrc = 42;
		const std::string cname = "video-send";

		auto pc = std::make_shared<rtc::PeerConnection>();
		// 这些状态日志非常值得保留。
		// 学 WebRTC 时，很多“看起来没反应”的问题，其实都能从状态变化里定位。
		pc->onStateChange([](rtc::PeerConnection::State state) {
			std::cout << "[pc] state: " << state << std::endl;
		});
		pc->onIceStateChange([](rtc::PeerConnection::IceState state) {
			std::cout << "[pc] ice: " << state << std::endl;
		});

		std::mutex gatherMutex;
		std::condition_variable gatherCv;
		bool gatherDone = false;
		pc->onGatheringStateChange([&](rtc::PeerConnection::GatheringState state) {
			std::cout << "[pc] gathering: " << state << std::endl;
			if (state == rtc::PeerConnection::GatheringState::Complete) {
				{
					std::lock_guard<std::mutex> lock(gatherMutex);
					gatherDone = true;
				}
				gatherCv.notify_one();
			}
		});

		rtc::Description::Video video("video", rtc::Description::Direction::SendOnly);
		video.addH264Codec(static_cast<uint8_t>(payloadType), h264Fmtp);
		video.addSSRC(ssrc, cname, "stream1", "video");
		auto track = pc->addTrack(video);

		// 关键理解：
		// 文件里拿出来的是“一帧 Annex-B H264”。
		// 真正发到网络上时，还要经过 RTP packetizer。
		// 这里选择 StartSequence，表示输入数据本身是 Annex-B。
		auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
		    ssrc, cname, static_cast<uint8_t>(payloadType), rtc::H264RtpPacketizer::ClockRate);
		auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence,
		                                                          rtpConfig);
		auto srReporter = std::make_shared<rtc::RtcpSrReporter>(rtpConfig);
		auto nackResponder = std::make_shared<rtc::RtcpNackResponder>();
		packetizer->addToChain(srReporter);
		packetizer->addToChain(nackResponder);
		track->setMediaHandler(packetizer);

		// 发送前一定等 track open。
		std::mutex openMutex;
		std::condition_variable openCv;
		bool trackOpen = false;
		track->onOpen([&]() {
			std::cout << "[track] open" << std::endl;
			{
				std::lock_guard<std::mutex> lock(openMutex);
				trackOpen = true;
			}
			openCv.notify_one();
		});
		track->onClosed([]() {
			std::cout << "[track] closed" << std::endl;
		});

		pc->setLocalDescription();
		{
			std::unique_lock<std::mutex> lock(gatherMutex);
			gatherCv.wait_for(lock, std::chrono::seconds(5), [&]() { return gatherDone; });
		}

		auto local = pc->localDescription();
		if (!local) {
			throw std::runtime_error("localDescription not ready");
		}

		const std::string offerSdp = std::string(local.value());
		std::cout << "[sdp] local offer bytes=" << offerSdp.size() << std::endl;

		// 通过 SRS /rtc/v1/publish/ 换 answer。
		const std::string body = std::string("{") +
		                         "\"api\":\"" + JsonEscape(publishApi) + "\"," +
		                         "\"streamurl\":\"" + JsonEscape(streamUrl) + "\"," +
		                         "\"clientip\":null," + "\"sdp\":\"" + JsonEscape(offerSdp) +
		                         "\"}";
		std::cout << "[http] POST " << publishApi << std::endl;
		const std::string response = HttpPostJson(publishApi, body);
		std::cout << "[http] response bytes=" << response.size() << std::endl;
		if (ExtractCode(response) != 0) {
			throw std::runtime_error("SRS publish failed, response=" + response);
		}

		pc->setRemoteDescription(rtc::Description(ExtractSdp(response), "answer"));
		std::cout << "[sdp] answer applied" << std::endl;

		{
			std::unique_lock<std::mutex> lock(openMutex);
			if (!openCv.wait_for(lock, std::chrono::seconds(10), [&]() { return trackOpen; })) {
				throw std::runtime_error("track did not open");
			}
		}

		const double frameDurationUs = 1000000.0 / static_cast<double>(fps);
		const auto startAt = std::chrono::steady_clock::now();
		size_t frameIndex = 0;
		std::cout << "[push] start loop, press Ctrl+C to stop" << std::endl;
		while (!gStopRequested.load()) {
			// 这里每次取一帧 Access Unit 发出去。
			// 这就是“文件版为什么要拆帧”的最终落点。
			const AccessUnit &au = accessUnits[frameIndex % accessUnits.size()];
			const auto presentation = std::chrono::duration<double, std::micro>(frameIndex * frameDurationUs);
			track->sendFrame(au.annexb, rtc::FrameInfo{presentation});

			if (frameIndex % static_cast<size_t>(fps) == 0) {
				// 每秒打一次日志，便于观察是否在稳定发送。
				std::cout << "[push] frame=" << frameIndex << " key=" << (au.keyFrame ? "yes" : "no")
				          << " size=" << au.annexb.size() << std::endl;
			}

			++frameIndex;
			// 用 sleep_until 控制“按帧率发送”，而不是一股脑把所有帧立刻塞出去。
			std::this_thread::sleep_until(startAt + std::chrono::microseconds(
			                                      static_cast<int64_t>(frameIndex * frameDurationUs)));
		}

		std::cout << "[push] stopping" << std::endl;
		track->close();
		pc->close();
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
	} catch (const std::exception &e) {
		std::cerr << "Error: " << e.what() << std::endl;
		return 1;
	}

	return 0;
}
