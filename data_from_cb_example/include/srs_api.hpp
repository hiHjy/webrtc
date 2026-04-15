#ifndef SRS_API_H
#define SRS_API_H

#include <string>

std::string JsonEscape(const std::string &input);
std::string JsonUnescape(const std::string &input);
std::string BuildSrsApiBody(const std::string &api, const std::string &streamUrl,
                            const std::string &offerSdp);
std::string HttpPostJson(const std::string &url, const std::string &body);
std::string HttpLastError();
int ExtractCode(const std::string &jsonText);
std::string ExtractSdp(const std::string &jsonText);
bool PrepareSrsPublishSession(const std::string &publishApi, const std::string &streamUrl);

#endif
