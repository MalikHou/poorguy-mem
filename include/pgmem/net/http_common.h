#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <string>

namespace pgmem::net {

struct HttpRequest {
    std::string method;
    std::string path;
    std::string version;
    std::map<std::string, std::string> headers;
    std::string body;
};

struct HttpResponse {
    int status_code{200};
    std::string content_type{"application/json"};
    std::map<std::string, std::string> headers;
    std::string body;
};

bool ParseHttpRequest(const std::string& raw, HttpRequest* request, size_t* consumed, std::string* error);
std::string BuildHttpResponse(const HttpResponse& response);

}  // namespace pgmem::net
