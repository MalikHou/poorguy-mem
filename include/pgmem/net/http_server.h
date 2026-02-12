#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "pgmem/net/http_common.h"

namespace brpc {
class Server;
}

namespace pgmem::net {

struct HttpServerOptions {
    int num_threads{0};
};

class HttpServer {
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;

    HttpServer(std::string host, int port, HttpServerOptions options = {});
    ~HttpServer();

    void RegisterRoute(const std::string& method, const std::string& path, Handler handler);

    bool Start(std::string* error);
    void Stop();
    HttpResponse Dispatch(const HttpRequest& request);

private:
    std::string host_;
    int port_;
    HttpServerOptions options_;

    std::atomic<bool> running_{false};
    std::unique_ptr<brpc::Server> server_;

    std::mutex mu_;
    std::map<std::string, Handler> routes_;
};

}  // namespace pgmem::net
