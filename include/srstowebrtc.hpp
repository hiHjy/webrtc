#ifndef SRSTOWEBRTC_HPP
#define SRSTOWEBRTC_HPP

#include "rtc/rtc.hpp"

#include <atomic>
#include <condition_variable>
#include <csignal>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class SrsToWebRTC {
public:
	using FrameCallback = std::function<void(rtc::binary frame, rtc::FrameInfo info)>;

	SrsToWebRTC();
	~SrsToWebRTC();

	int initRTC(const char *url, const char *streamUrl);
	void Stop();

	void setVideoFrameCallback(FrameCallback callback);
	void setAudioFrameCallback(FrameCallback callback);

	bool getStopRequested() const {
		return gStopRequested.load();
	}

	void setStopRequested(bool value) {
		gStopRequested.store(value);
	}

	static SrsToWebRTC *getInstance() {
		return self;
	}

private:
	std::atomic<bool> gStopRequested{false};
	std::shared_ptr<rtc::PeerConnection> pc;
	std::shared_ptr<rtc::Track> videoTrack;
	std::shared_ptr<rtc::Track> audioTrack;
	std::vector<std::shared_ptr<rtc::Track>> remoteTracks;

	FrameCallback videoFrameCallback;
	FrameCallback audioFrameCallback;
	std::mutex callbackMutex;

	static SrsToWebRTC *self;

	std::mutex gatherMutex;
	std::condition_variable gatherCond;
	bool gatherDone = false;

	std::mutex videoOpenMutex;
	std::condition_variable videoOpenCv;
	bool videoTrackOpen = false;

	std::mutex audioOpenMutex;
	std::condition_variable audioOpenCv;
	bool audioTrackOpen = false;
};

#endif
