#pragma once

#include <string>

namespace pgmem::net {

struct HttpClientResponse {
    bool ok{false};
    int status_code{0};
    std::string body;
    std::string error;
};

class HttpClient {
public:
    HttpClientResponse PostJson(const std::string& host,
                                int port,
                                const std::string& path,
                                const std::string& body,
                                int timeout_ms) const;

    HttpClientResponse Get(const std::string& host,
                           int port,
                           const std::string& path,
                           int timeout_ms) const;
};

}  // namespace pgmem::net
