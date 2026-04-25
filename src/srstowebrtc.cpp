#include "srstowebrtc.hpp"

#include "srs_api.hpp"

#include <chrono>
#include <iostream>
#include <thread>

namespace {

void HandlePullSignal(int) {
	SrsToWebRTC *instance = SrsToWebRTC::getInstance();
	if (instance) {
		instance->setStopRequested(true);
	}
}

} // namespace

SrsToWebRTC *SrsToWebRTC::self = nullptr;

SrsToWebRTC::SrsToWebRTC() = default;

SrsToWebRTC::~SrsToWebRTC() = default;

void SrsToWebRTC::setVideoFrameCallback(FrameCallback callback) {
	std::lock_guard<std::mutex> lock(callbackMutex);
	videoFrameCallback = std::move(callback);
}

void SrsToWebRTC::setAudioFrameCallback(FrameCallback callback) {
	std::lock_guard<std::mutex> lock(callbackMutex);
	audioFrameCallback = std::move(callback);
}

void SrsToWebRTC::Stop() {
	if (videoTrack) {
		videoTrack->close();
	}
	if (audioTrack) {
		audioTrack->close();
	}
	if (pc) {
		pc->close();
	}
	remoteTracks.clear();
	self = nullptr;
	std::cout << "SrsToWebRTC stopped" << std::endl;
}

int SrsToWebRTC::initRTC(const char *url, const char *streamUrl) {
	std::cout << "initRTC pull" << std::endl;

	std::signal(SIGINT, HandlePullSignal);
	std::signal(SIGTERM, HandlePullSignal);

	rtc::InitLogger(rtc::LogLevel::Info);

	pc = std::make_shared<rtc::PeerConnection>();
	self = this;
	gStopRequested.store(false);
	remoteTracks.clear();

	pc->onStateChange([](rtc::PeerConnection::State state) {
		std::cout << "[pull-pc] state: " << state << std::endl;
	});
	pc->onIceStateChange([](rtc::PeerConnection::IceState state) {
		std::cout << "[pull-pc] ice: " << state << std::endl;
	});

	gatherDone = false;
	pc->onGatheringStateChange([this](rtc::PeerConnection::GatheringState state) {
		std::cout << "[pull-pc] gathering: " << state << std::endl;
		if (state == rtc::PeerConnection::GatheringState::Complete) {
			{
				std::lock_guard<std::mutex> lock(gatherMutex);
				gatherDone = true;
			}
			gatherCond.notify_one();
		}
	});

	pc->onTrack([this](const std::shared_ptr<rtc::Track> &track) {
		std::cout << "[pull-pc] got remote track: " << track->mid() << std::endl;
		remoteTracks.push_back(track);
	});

	int audioPayloadType = 111;
	rtc::Description::Audio audio("audio", rtc::Description::Direction::RecvOnly);
	audio.addAudioCodec(static_cast<uint8_t>(audioPayloadType), "opus/48000/1",
	                    "minptime=10;useinbandfec=1;stereo=0;sprop-stereo=0");
	audioTrack = pc->addTrack(audio);
	audioTrack->setMediaHandler(std::make_shared<rtc::OpusRtpDepacketizer>());
	audioTrack->chainMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());

	audioTrackOpen = false;
	audioTrack->onOpen([this]() {
		std::cout << "[pull-audio-track] open" << std::endl;
		{
			std::lock_guard<std::mutex> lock(audioOpenMutex);
			audioTrackOpen = true;
		}
		audioOpenCv.notify_one();
	});
	audioTrack->onClosed([this]() {
		std::cout << "[pull-audio-track] closed" << std::endl;
		{
			std::lock_guard<std::mutex> lock(audioOpenMutex);
			audioTrackOpen = false;
		}
		audioOpenCv.notify_one();
	});
	audioTrack->onFrame([this](rtc::binary frame, rtc::FrameInfo info) {
		FrameCallback callback;
		{
			std::lock_guard<std::mutex> lock(callbackMutex);
			callback = audioFrameCallback;
		}
		if (callback) {
			callback(std::move(frame), info);
		}
	});

	const std::string h264Fmtp =
	    "profile-level-id=42c01f;packetization-mode=1;level-asymmetry-allowed=1";
	int videoPayloadType = 96;
	rtc::Description::Video video("video", rtc::Description::Direction::RecvOnly);
	video.addH264Codec(static_cast<uint8_t>(videoPayloadType), h264Fmtp);
	videoTrack = pc->addTrack(video);
	videoTrack->setMediaHandler(std::make_shared<rtc::H264RtpDepacketizer>());
	videoTrack->chainMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());

	videoTrackOpen = false;
	videoTrack->onOpen([this]() {
		std::cout << "[pull-video-track] open" << std::endl;
		{
			std::lock_guard<std::mutex> lock(videoOpenMutex);
			videoTrackOpen = true;
		}
		videoOpenCv.notify_one();
	});
	videoTrack->onClosed([this]() {
		std::cout << "[pull-video-track] closed" << std::endl;
		{
			std::lock_guard<std::mutex> lock(videoOpenMutex);
			videoTrackOpen = false;
		}
		videoOpenCv.notify_one();
	});
	videoTrack->onFrame([this](rtc::binary frame, rtc::FrameInfo info) {
		FrameCallback callback;
		{
			std::lock_guard<std::mutex> lock(callbackMutex);
			callback = videoFrameCallback;
		}
		if (callback) {
			callback(std::move(frame), info);
		}
	});

	pc->setLocalDescription();
	{
		std::unique_lock<std::mutex> lock(gatherMutex);
		gatherCond.wait_for(lock, std::chrono::seconds(5), [this]() { return gatherDone; });
	}

	auto local = pc->localDescription();
	if (!local) {
		std::cout << "pull localDescription not ready" << std::endl;
		return -1;
	}

	const std::string offerSdp = std::string(local.value());
	std::cout << "==================== Pull Local SDP ==========================" << std::endl;
	std::cout << "Pull Local SDP:\n" << offerSdp << std::endl << std::endl;

	const std::string body = BuildSrsApiBody(url, streamUrl, offerSdp);
	printf("pull body=%s\n", body.c_str());

	const std::string response = HttpPostJson(url, body);
	if (response.empty()) {
		std::cout << "pull http post failed: " << HttpLastError() << std::endl;
		return -1;
	}

	const int responseCode = ExtractCode(response);
	if (responseCode != 0) {
		std::cout << "SRS play API error: " << response << std::endl;
		return -1;
	}

	const std::string answerSdp = ExtractSdp(response);
	if (answerSdp.empty()) {
		std::cout << "pull answer sdp is empty: " << HttpLastError() << std::endl;
		return -1;
	}

	std::cout << "==================== Pull Answer SDP ==========================" << std::endl;
	std::cout << "Pull Answer SDP:\n" << answerSdp << std::endl << std::endl;

	pc->setRemoteDescription(answerSdp);

	{
		std::unique_lock<std::mutex> lock(videoOpenMutex);
		videoOpenCv.wait_for(lock, std::chrono::seconds(10), [this]() { return videoTrackOpen; });
		if (!videoTrackOpen) {
			std::cout << "Pull video track failed to open" << std::endl;
			return -1;
		}
		std::cout << "Pull video track opened successfully" << std::endl;
	}

	{
		std::unique_lock<std::mutex> lock(audioOpenMutex);
		audioOpenCv.wait_for(lock, std::chrono::seconds(10), [this]() { return audioTrackOpen; });
		if (!audioTrackOpen) {
			std::cout << "Pull audio track failed to open, continue with video only" << std::endl;
			return 0;
		}
		std::cout << "Pull audio track opened successfully" << std::endl;
	}

	return 0;
}
