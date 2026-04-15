#include "config_reader.hpp"
#include "srstowebrtc.hpp"

#include <chrono>
#include <iostream>
#include <thread>

namespace {

std::string DerivePlayApi(const std::string &publishApi) {
    std::string playApi = publishApi;
    const std::string publishSuffix = "/rtc/v1/publish/";
    const std::string playSuffix = "/rtc/v1/play/";
    const std::size_t pos = playApi.find(publishSuffix);
    if (pos != std::string::npos) {
        playApi.replace(pos, publishSuffix.size(), playSuffix);
    }
    return playApi;
}

} // namespace

int main(int argc, char const *argv[])
{
    ConfigReader config(std::string(WEBRTC_PROJECT_ROOT) + "/main.conf");
    if (!config.loaded()) {
        std::cerr << "Failed to load config file" << std::endl;
        return 1;
    }

    const std::string publishApi = config.read("publish_api");
    const std::string playApiFromConfig = config.read("play_api");
    const std::string streamUrl = config.read("stream_url");
    if (publishApi.empty() || streamUrl.empty()) {
        std::cerr << "publish_api or stream_url is empty" << std::endl;
        return 1;
    }

    const std::string playApi = playApiFromConfig.empty() ? DerivePlayApi(publishApi)
                                                          : playApiFromConfig;

    SrsToWebRTC app;
    app.setAudioFrameCallback([](rtc::binary frame, rtc::FrameInfo info) {
        std::cout << "[pull-audio] packet size=" << frame.size()
                  << " timestamp=" << info.timestamp << std::endl;
    });
    app.setVideoFrameCallback([](rtc::binary frame, rtc::FrameInfo info) {
        std::cout << "[pull-video] frame size=" << frame.size()
                  << " timestamp=" << info.timestamp << std::endl;
    });

    if (app.initRTC(playApi.c_str(), streamUrl.c_str()) != 0) {
        std::cerr << "Failed to initialize pull RTC" << std::endl;
        return 1;
    }

    while (!app.getStopRequested()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    app.Stop();
    return 0;
}
