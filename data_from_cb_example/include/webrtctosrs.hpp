#ifndef WEBRTCTOSRS_H
#define WEBRTCTOSRS_H
#include "rtc/rtc.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <netdb.h>
#include <optional>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

class WebRTCToSrs {
public:
	WebRTCToSrs();
	~WebRTCToSrs();
	int initRTC(const char *url, const char *stream_url);
	int connectToSrs();
	bool SendEncodedAnnexBFrame(const uint8_t *data, size_t size, uint64_t timestampUs);
	bool SendEncodedOpus(const uint8_t *data, size_t size, uint64_t timestampUs);
	void Stop();

	bool getStopRequested() const {
		return gStopRequested.load();
	}

	void setStopRequested(bool value) {
		gStopRequested.store(value);
	}

	static WebRTCToSrs *getInstance() {
		return self;
	}

private:
	std::atomic<bool> gStopRequested{false};
	std::shared_ptr<rtc::PeerConnection> pc;
	std::shared_ptr<rtc::Track> videoTrack;
	std::shared_ptr<rtc::Track> audioTrack;
	static WebRTCToSrs *self;

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
