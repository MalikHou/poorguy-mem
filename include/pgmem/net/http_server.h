#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "pgmem/net/http_common.h"

namespace pgmem::net {

class HttpServer {
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;

    HttpServer(std::string host, int port);
    ~HttpServer();

    void RegisterRoute(const std::string& method, const std::string& path, Handler handler);

    bool Start(std::string* error);
    void Stop();

private:
    void AcceptLoop();
    void HandleClient(int client_fd);

    std::string host_;
    int port_;

    int listen_fd_{-1};
    std::atomic<bool> running_{false};
    std::thread accept_thread_;

    std::mutex mu_;
    std::map<std::string, Handler> routes_;
};

}  // namespace pgmem::net
