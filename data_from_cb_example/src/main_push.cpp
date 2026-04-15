#include "config_reader.hpp"
#include "srs_api.hpp"
#include "webrtctosrs.hpp"
#include <iostream>
int main(int argc, char const *argv[])
{
    ConfigReader config(std::string(WEBRTC_PROJECT_ROOT) + "/main.conf");
    if (!config.loaded()) {
        std::cerr << "Failed to load config file" << std::endl;
        return 1;
    }

    const std::string publishApi = config.read("publish_api");
    const std::string streamUrl = config.read("stream_url");
    if (publishApi.empty() || streamUrl.empty()) {
        std::cerr << "publish_api or stream_url is empty" << std::endl;
        return 1;
    }

    // printf("publishApi: %s\n", publishApi.c_str());
    // printf("streamUrl: %s\n", streamUrl.c_str());
    
    if (!PrepareSrsPublishSession(publishApi, streamUrl)) {
        std::cerr << "Failed to prepare SRS publish session: " << HttpLastError() << std::endl;
        return 1;
    }

    WebRTCToSrs app;
    if (app.initRTC(publishApi.c_str(), streamUrl.c_str()) != 0) {
        std::cerr << "Failed to initialize RTC" << std::endl;
        return 1;
    }

   
    while (!app.getStopRequested()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    app.Stop();
    return 0;
}
