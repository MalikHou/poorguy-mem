#include "pgmem/net/http_common.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace pgmem::net {
namespace {

std::string Trim(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start])) != 0) {
        ++start;
    }
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1])) != 0) {
        --end;
    }
    return s.substr(start, end - start);
}

std::string ToLower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string StatusText(int code) {
    switch (code) {
        case 200:
            return "OK";
        case 201:
            return "Created";
        case 204:
            return "No Content";
        case 400:
            return "Bad Request";
        case 404:
            return "Not Found";
        case 409:
            return "Conflict";
        case 422:
            return "Unprocessable Entity";
        case 429:
            return "Too Many Requests";
        case 500:
            return "Internal Server Error";
        case 503:
            return "Service Unavailable";
        default:
            return "Unknown";
    }
}

}  // namespace

bool ParseHttpRequest(const std::string& raw, HttpRequest* request, size_t* consumed, std::string* error) {
    if (request == nullptr || consumed == nullptr) {
        if (error != nullptr) {
            *error = "null output argument";
        }
        return false;
    }

    const size_t header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        if (error != nullptr) {
            *error = "incomplete header";
        }
        return false;
    }

    std::istringstream iss(raw.substr(0, header_end));
    std::string first_line;
    if (!std::getline(iss, first_line)) {
        if (error != nullptr) {
            *error = "missing request line";
        }
        return false;
    }
    if (!first_line.empty() && first_line.back() == '\r') {
        first_line.pop_back();
    }

    std::istringstream first(first_line);
    if (!(first >> request->method >> request->path >> request->version)) {
        if (error != nullptr) {
            *error = "invalid request line";
        }
        return false;
    }

    request->headers.clear();
    std::string line;
    size_t content_length = 0;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        const size_t sep = line.find(':');
        if (sep == std::string::npos) {
            continue;
        }
        const std::string key   = ToLower(Trim(line.substr(0, sep)));
        const std::string value = Trim(line.substr(sep + 1));
        request->headers[key]   = value;
        if (key == "content-length") {
            try {
                content_length = static_cast<size_t>(std::stoull(value));
            } catch (const std::exception&) {
                if (error != nullptr) {
                    *error = "invalid content-length";
                }
                return false;
            }
        }
    }

    const size_t body_offset = header_end + 4;
    if (raw.size() < body_offset + content_length) {
        if (error != nullptr) {
            *error = "incomplete body";
        }
        return false;
    }

    request->body = raw.substr(body_offset, content_length);
    *consumed     = body_offset + content_length;
    return true;
}

std::string BuildHttpResponse(const HttpResponse& response) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << response.status_code << ' ' << StatusText(response.status_code) << "\r\n";
    oss << "Content-Type: " << response.content_type << "\r\n";
    oss << "Content-Length: " << response.body.size() << "\r\n";
    oss << "Connection: close\r\n";
    for (const auto& kv : response.headers) {
        oss << kv.first << ": " << kv.second << "\r\n";
    }
    oss << "\r\n";
    oss << response.body;
    return oss.str();
}

}  // namespace pgmem::net
