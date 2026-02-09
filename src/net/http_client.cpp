#include "pgmem/net/http_client.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <sstream>

namespace pgmem::net {
namespace {

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

int ParseStatusCode(const std::string& response) {
    const size_t line_end = response.find("\r\n");
    if (line_end == std::string::npos) {
        return 0;
    }
    std::istringstream iss(response.substr(0, line_end));
    std::string version;
    int code = 0;
    iss >> version >> code;
    return code;
}

std::string ParseBody(const std::string& response) {
    const size_t offset = response.find("\r\n\r\n");
    if (offset == std::string::npos) {
        return {};
    }
    return response.substr(offset + 4);
}

HttpClientResponse ExecRequest(const std::string& host,
                               int port,
                               const std::string& request,
                               int timeout_ms) {
    HttpClientResponse out;

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        out.error = "socket() failed";
        return out;
    }

    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        out.error = "invalid host";
        ::close(fd);
        return out;
    }

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        out.error = std::string("connect() failed: ") + std::strerror(errno);
        ::close(fd);
        return out;
    }

    if (!SendAll(fd, request)) {
        out.error = "send() failed";
        ::close(fd);
        return out;
    }

    std::string response;
    char buf[4096];
    while (true) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
        response.append(buf, static_cast<size_t>(n));
    }

    ::close(fd);

    out.status_code = ParseStatusCode(response);
    out.body = ParseBody(response);
    out.ok = (out.status_code >= 200 && out.status_code < 300);
    if (!out.ok && out.error.empty()) {
        out.error = "non-2xx status";
    }
    return out;
}

}  // namespace

HttpClientResponse HttpClient::PostJson(const std::string& host,
                                        int port,
                                        const std::string& path,
                                        const std::string& body,
                                        int timeout_ms) const {
    std::ostringstream oss;
    oss << "POST " << path << " HTTP/1.1\r\n";
    oss << "Host: " << host << ':' << port << "\r\n";
    oss << "Content-Type: application/json\r\n";
    oss << "Content-Length: " << body.size() << "\r\n";
    oss << "Connection: close\r\n\r\n";
    oss << body;
    return ExecRequest(host, port, oss.str(), timeout_ms);
}

HttpClientResponse HttpClient::Get(const std::string& host,
                                   int port,
                                   const std::string& path,
                                   int timeout_ms) const {
    std::ostringstream oss;
    oss << "GET " << path << " HTTP/1.1\r\n";
    oss << "Host: " << host << ':' << port << "\r\n";
    oss << "Connection: close\r\n\r\n";
    return ExecRequest(host, port, oss.str(), timeout_ms);
}

}  // namespace pgmem::net
