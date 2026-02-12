#include "pgmem/net/http_client.h"

#include <brpc/channel.h>
#include <brpc/controller.h>
#include <brpc/http_method.h>
#include <butil/errno.h>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace pgmem::net {
namespace {

struct ChannelCache {
    std::mutex mu;
    std::unordered_map<std::string, std::weak_ptr<brpc::Channel>> by_endpoint;
};

ChannelCache& GetChannelCache() {
    static ChannelCache cache;
    return cache;
}

std::string EndpointKey(const std::string& host, int port) { return host + ":" + std::to_string(port); }

std::string BuildHttpUri(const std::string& host, int port, const std::string& path) {
    if (path.empty()) {
        return "http://" + host + ":" + std::to_string(port) + "/";
    }
    if (path.front() == '/') {
        return "http://" + host + ":" + std::to_string(port) + path;
    }
    return "http://" + host + ":" + std::to_string(port) + "/" + path;
}

std::shared_ptr<brpc::Channel> AcquireChannel(const std::string& host, int port, std::string* error) {
    const std::string endpoint_key = EndpointKey(host, port);

    {
        std::lock_guard<std::mutex> lock(GetChannelCache().mu);
        auto it = GetChannelCache().by_endpoint.find(endpoint_key);
        if (it != GetChannelCache().by_endpoint.end()) {
            auto channel = it->second.lock();
            if (channel != nullptr) {
                return channel;
            }
        }
    }

    auto channel = std::make_shared<brpc::Channel>();
    brpc::ChannelOptions options;
    options.protocol        = "http";
    options.connection_type = "pooled";
    if (channel->Init(host.c_str(), port, &options) != 0) {
        if (error != nullptr) {
            *error = std::string("brpc channel init failed: ") + berror(errno);
        }
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(GetChannelCache().mu);
        GetChannelCache().by_endpoint[endpoint_key] = channel;
    }
    return channel;
}

HttpClientResponse ExecRequest(const std::string& host, int port, const std::string& path, brpc::HttpMethod method,
                               const std::string& body, const std::string& content_type, int timeout_ms) {
    HttpClientResponse out;

    std::string channel_error;
    auto channel = AcquireChannel(host, port, &channel_error);
    if (channel == nullptr) {
        out.error = channel_error;
        return out;
    }

    brpc::Controller cntl;
    if (timeout_ms > 0) {
        cntl.set_timeout_ms(timeout_ms);
    }
    cntl.http_request().uri() = BuildHttpUri(host, port, path);
    cntl.http_request().set_method(method);
    if (!content_type.empty()) {
        cntl.http_request().set_content_type(content_type);
    }
    if (!body.empty()) {
        cntl.request_attachment().append(body);
    }

    channel->CallMethod(nullptr, &cntl, nullptr, nullptr, nullptr);

    out.status_code = cntl.http_response().status_code();
    out.body        = cntl.response_attachment().to_string();

    if (cntl.Failed()) {
        out.error = cntl.ErrorText();
        out.ok    = false;
        return out;
    }

    out.ok = (out.status_code >= 200 && out.status_code < 300);
    if (!out.ok) {
        out.error = "non-2xx status";
    }
    return out;
}

}  // namespace

HttpClientResponse HttpClient::PostJson(const std::string& host, int port, const std::string& path,
                                        const std::string& body, int timeout_ms) const {
    return ExecRequest(host, port, path, brpc::HTTP_METHOD_POST, body, "application/json", timeout_ms);
}

HttpClientResponse HttpClient::Get(const std::string& host, int port, const std::string& path, int timeout_ms) const {
    return ExecRequest(host, port, path, brpc::HTTP_METHOD_GET, std::string(), std::string(), timeout_ms);
}

}  // namespace pgmem::net
