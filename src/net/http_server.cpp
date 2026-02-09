#include "pgmem/net/http_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <thread>

namespace pgmem::net {
namespace {

std::string RouteKey(const std::string& method, const std::string& path) {
    return method + " " + path;
}

void CloseFd(int fd) {
    if (fd >= 0) {
        ::close(fd);
    }
}

bool SendAll(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

}  // namespace

HttpServer::HttpServer(std::string host, int port) : host_(std::move(host)), port_(port) {}

HttpServer::~HttpServer() {
    Stop();
}

void HttpServer::RegisterRoute(const std::string& method, const std::string& path, Handler handler) {
    std::lock_guard<std::mutex> lock(mu_);
    routes_[RouteKey(method, path)] = std::move(handler);
}

bool HttpServer::Start(std::string* error) {
    if (running_.load()) {
        return true;
    }

    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        if (error != nullptr) {
            *error = "socket() failed: " + std::string(std::strerror(errno));
        }
        return false;
    }

    int opt = 1;
    if (::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != 0) {
        if (error != nullptr) {
            *error = "setsockopt() failed: " + std::string(std::strerror(errno));
        }
        CloseFd(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port_));
    if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1) {
        if (error != nullptr) {
            *error = "invalid host: " + host_;
        }
        CloseFd(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (error != nullptr) {
            *error = "bind() failed: " + std::string(std::strerror(errno));
        }
        CloseFd(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    if (::listen(listen_fd_, 256) != 0) {
        if (error != nullptr) {
            *error = "listen() failed: " + std::string(std::strerror(errno));
        }
        CloseFd(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    running_.store(true);
    accept_thread_ = std::thread(&HttpServer::AcceptLoop, this);
    return true;
}

void HttpServer::Stop() {
    if (!running_.exchange(false)) {
        return;
    }

    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        CloseFd(listen_fd_);
        listen_fd_ = -1;
    }
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
}

void HttpServer::AcceptLoop() {
    while (running_.load()) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        const int client_fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_fd < 0) {
            if (running_.load()) {
                std::cerr << "accept() failed: " << std::strerror(errno) << "\n";
            }
            continue;
        }
        std::thread(&HttpServer::HandleClient, this, client_fd).detach();
    }
}

void HttpServer::HandleClient(int client_fd) {
    std::string raw;
    raw.reserve(4096);
    char buffer[4096];

    HttpRequest request;
    size_t consumed = 0;
    std::string parse_error;

    while (true) {
        const ssize_t n = ::recv(client_fd, buffer, sizeof(buffer), 0);
        if (n <= 0) {
            break;
        }
        raw.append(buffer, static_cast<size_t>(n));
        if (ParseHttpRequest(raw, &request, &consumed, &parse_error)) {
            break;
        }
        if (parse_error != "incomplete header" && parse_error != "incomplete body") {
            break;
        }
    }

    HttpResponse response;
    if (consumed == 0) {
        response.status_code = 400;
        response.body = "{\"error\":\"invalid HTTP request\"}";
        SendAll(client_fd, BuildHttpResponse(response));
        CloseFd(client_fd);
        return;
    }

    Handler handler;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = routes_.find(RouteKey(request.method, request.path));
        if (it != routes_.end()) {
            handler = it->second;
        }
    }

    if (!handler) {
        response.status_code = 404;
        response.body = "{\"error\":\"route not found\"}";
    } else {
        try {
            response = handler(request);
        } catch (const std::exception& ex) {
            response.status_code = 500;
            response.body = std::string("{\"error\":\"") + ex.what() + "\"}";
        }
    }

    SendAll(client_fd, BuildHttpResponse(response));
    CloseFd(client_fd);
}

}  // namespace pgmem::net
