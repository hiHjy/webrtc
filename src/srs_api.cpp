#include "srs_api.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <netdb.h>
#include <nlohmann/json.hpp>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

using json = nlohmann::json;

struct ParsedUrl {
    std::string host;
    std::string port;
    std::string path;
};

struct ParsedWebRtcUrl {
    std::string app;
    std::string stream;
};

std::string g_lastError;

void SetLastError(const std::string &message) {
    g_lastError = message;
    std::cerr << message << std::endl;
}

bool ParseHttpUrl(const std::string &url, ParsedUrl &parsed) {
    const std::string prefix = "http://";
    if (url.rfind(prefix, 0) != 0) {
        SetLastError("Only http:// URL is supported: " + url);
        return false;
    }

    const std::string rest = url.substr(prefix.size());
    const std::size_t slashPos = rest.find('/');
    const std::string hostPort = slashPos == std::string::npos ? rest : rest.substr(0, slashPos);
    parsed.path = slashPos == std::string::npos ? "/" : rest.substr(slashPos);
    if (hostPort.empty()) {
        SetLastError("invalid http url: " + url);
        return false;
    }

    const std::size_t colonPos = hostPort.find(':');
    if (colonPos == std::string::npos) {
        parsed.host = hostPort;
        parsed.port = "80";
    } else {
        parsed.host = hostPort.substr(0, colonPos);
        parsed.port = hostPort.substr(colonPos + 1);
    }

    if (parsed.host.empty() || parsed.port.empty()) {
        SetLastError("invalid http url: " + url);
        return false;
    }
    return true;
}

bool ParseWebRtcUrl(const std::string &url, ParsedWebRtcUrl &parsed) {
    const std::string prefix = "webrtc://";
    if (url.rfind(prefix, 0) != 0) {
        SetLastError("invalid webrtc stream url: " + url);
        return false;
    }

    const std::string rest = url.substr(prefix.size());
    const std::size_t firstSlash = rest.find('/');
    if (firstSlash == std::string::npos) {
        SetLastError("invalid webrtc stream url: " + url);
        return false;
    }

    const std::string path = rest.substr(firstSlash + 1);
    const std::size_t secondSlash = path.find('/');
    if (secondSlash == std::string::npos) {
        SetLastError("invalid webrtc stream url: " + url);
        return false;
    }

    parsed.app = path.substr(0, secondSlash);
    parsed.stream = path.substr(secondSlash + 1);
    const std::size_t queryPos = parsed.stream.find_first_of("?#");
    if (queryPos != std::string::npos) {
        parsed.stream = parsed.stream.substr(0, queryPos);
    }

    if (parsed.app.empty() || parsed.stream.empty()) {
        SetLastError("invalid webrtc stream url: " + url);
        return false;
    }
    return true;
}

std::string BuildApiBase(const ParsedUrl &parsed) {
    return "http://" + parsed.host + ":" + parsed.port;
}

bool ParseUnsigned(const std::string &text, size_t &value) {
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(text.c_str(), &end, 16);
    if (end == text.c_str() || *end != '\0') {
        return false;
    }
    value = static_cast<size_t>(parsed);
    return true;
}

bool DecodeChunkedBody(const std::string &chunked, std::string &out) {
    out.clear();
    size_t pos = 0;
    while (true) {
        const size_t lineEnd = chunked.find("\r\n", pos);
        if (lineEnd == std::string::npos) {
            SetLastError("Invalid chunked response");
            return false;
        }

        size_t chunkSize = 0;
        if (!ParseUnsigned(chunked.substr(pos, lineEnd - pos), chunkSize)) {
            SetLastError("Invalid chunked size");
            return false;
        }

        pos = lineEnd + 2;
        if (chunkSize == 0) {
            break;
        }
        if (pos + chunkSize + 2 > chunked.size()) {
            SetLastError("Truncated chunked response");
            return false;
        }

        out.append(chunked, pos, chunkSize);
        pos += chunkSize + 2;
    }
    return true;
}

bool ParseHttpStatus(const std::string &headers, int &status) {
    const std::size_t firstSpace = headers.find(' ');
    if (firstSpace == std::string::npos || firstSpace + 4 > headers.size()) {
        SetLastError("Cannot parse HTTP status");
        return false;
    }

    const std::string code = headers.substr(firstSpace + 1, 3);
    char *end = nullptr;
    const long parsed = std::strtol(code.c_str(), &end, 10);
    if (end == code.c_str() || *end != '\0') {
        SetLastError("Cannot parse HTTP status");
        return false;
    }
    status = static_cast<int>(parsed);
    return true;
}

bool HttpRequest(const std::string &method, const std::string &url, const std::string &body,
                 const std::string &contentType, std::string &responseBody) {
    ParsedUrl parsed;
    if (!ParseHttpUrl(url, parsed)) {
        return false;
    }

    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = nullptr;
    const int gai = getaddrinfo(parsed.host.c_str(), parsed.port.c_str(), &hints, &res);
    if (gai != 0) {
        SetLastError(std::string("getaddrinfo failed: ") + gai_strerror(gai));
        return false;
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
        SetLastError("connect failed to SRS API");
        return false;
    }

    std::string request =
        method + " " + parsed.path + " HTTP/1.1\r\n" + "Host: " + parsed.host + ":" + parsed.port +
        "\r\n" + "Connection: close\r\n";
    if (!body.empty()) {
        request += "Content-Type: " + contentType + "\r\n";
        request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    request += "\r\n";
    request += body;

    size_t sent = 0;
    while (sent < request.size()) {
        const ssize_t n = send(fd, request.data() + sent, request.size() - sent, 0);
        if (n <= 0) {
            close(fd);
            SetLastError("send failed");
            return false;
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
            SetLastError("recv failed");
            return false;
        }
        raw.append(buffer, static_cast<size_t>(n));
    }
    close(fd);

    const size_t sep = raw.find("\r\n\r\n");
    if (sep == std::string::npos) {
        SetLastError("Invalid HTTP response");
        return false;
    }

    const std::string headers = raw.substr(0, sep);
    responseBody = raw.substr(sep + 4);

    int status = 0;
    if (!ParseHttpStatus(headers, status)) {
        return false;
    }

    if (headers.find("Transfer-Encoding: chunked") != std::string::npos ||
        headers.find("transfer-encoding: chunked") != std::string::npos) {
        std::string decoded;
        if (!DecodeChunkedBody(responseBody, decoded)) {
            return false;
        }
        responseBody = decoded;
    }

    if (status < 200 || status >= 300) {
        SetLastError("HTTP status is " + std::to_string(status) + ", body: " + responseBody);
        return false;
    }

    g_lastError.clear();
    return true;
}

const json *FindStreamsArray(const json &root) {
    if (root.contains("streams") && root["streams"].is_array()) {
        return &root["streams"];
    }
    if (root.contains("data") && root["data"].is_array()) {
        return &root["data"];
    }
    if (root.contains("data") && root["data"].is_object()) {
        const json &data = root["data"];
        if (data.contains("streams") && data["streams"].is_array()) {
            return &data["streams"];
        }
    }
    return nullptr;
}

std::string FindPublishClientId(const json &streamsRoot, const std::string &app,
                                const std::string &stream) {
    const json *streams = FindStreamsArray(streamsRoot);
    if (!streams) {
        return "";
    }

    for (const auto &item : *streams) {
        if (!item.is_object()) {
            continue;
        }

        const std::string itemApp = item.value("app", "");
        const std::string itemName = item.value("name", "");
        if (!itemApp.empty() && itemApp != app) {
            continue;
        }
        if (itemName != stream) {
            continue;
        }

        if (!item.contains("publish") || !item["publish"].is_object()) {
            return "";
        }

        const json &publish = item["publish"];
        if (!publish.value("active", false)) {
            return "";
        }
        if (!publish.contains("cid")) {
            return "";
        }

        if (publish["cid"].is_string()) {
            return publish["cid"].get<std::string>();
        }
        if (publish["cid"].is_number_integer() || publish["cid"].is_number_unsigned()) {
            return std::to_string(publish["cid"].get<long long>());
        }
        return "";
    }

    return "";
}

} // namespace

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

std::string BuildSrsApiBody(const std::string &api, const std::string &streamUrl,
                            const std::string &offerSdp) {
    return std::string("{") + "\"api\":\"" + JsonEscape(api) + "\"," +
           "\"streamurl\":\"" + JsonEscape(streamUrl) + "\"," +
           "\"clientip\":null," + "\"sdp\":\"" + JsonEscape(offerSdp) + "\"}";
}

std::string HttpPostJson(const std::string &url, const std::string &body) {
    std::string response;
    if (!HttpRequest("POST", url, body, "application/json", response)) {
        return "";
    }
    return response;
}

std::string HttpLastError() {
    return g_lastError;
}

int ExtractCode(const std::string &jsonText) {
    const json root = json::parse(jsonText, nullptr, false);
    if (root.is_discarded()) {
        SetLastError("failed to parse SRS response JSON");
        return -1;
    }
    if (!root.contains("code") || !root["code"].is_number_integer()) {
        SetLastError("SRS response has no code field");
        return -1;
    }
    return root["code"].get<int>();
}

std::string ExtractSdp(const std::string &jsonText) {
    const json root = json::parse(jsonText, nullptr, false);
    if (root.is_discarded()) {
        SetLastError("failed to parse SRS response JSON");
        return "";
    }
    if (!root.contains("sdp") || !root["sdp"].is_string()) {
        SetLastError("SRS response has no sdp field");
        return "";
    }
    return root["sdp"].get<std::string>();
}

bool PrepareSrsPublishSession(const std::string &publishApi, const std::string &streamUrl) {
    ParsedUrl publishParsed;
    ParsedWebRtcUrl parsedStream;
    if (!ParseHttpUrl(publishApi, publishParsed) || !ParseWebRtcUrl(streamUrl, parsedStream)) {
        return false;
    }

    const std::string apiBase = BuildApiBase(publishParsed);
    const std::string streamsUrl = apiBase + "/api/v1/streams/";

    std::cout << "[srs] checking stale publish session for " << parsedStream.app
              << "/" << parsedStream.stream << std::endl;

    std::string streamsResponse;
    if (!HttpRequest("GET", streamsUrl, "", "application/json", streamsResponse)) {
        return false;
    }

    const json streamsRoot = json::parse(streamsResponse, nullptr, false);
    if (streamsRoot.is_discarded()) {
        SetLastError("failed to parse /api/v1/streams response");
        return false;
    }

    const std::string publishCid =
        FindPublishClientId(streamsRoot, parsedStream.app, parsedStream.stream);

    if (publishCid.empty()) {
        std::cout << "[srs] no stale publish client found" << std::endl;
        return true;
    }

    const std::string deleteUrl = apiBase + "/api/v1/clients/" + publishCid;
    std::cout << "[srs] removing stale publish client, cid=" << publishCid << std::endl;

    std::string deleteResponse;
    if (!HttpRequest("DELETE", deleteUrl, "", "application/json", deleteResponse)) {
        return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    return true;
}
