#include "pgmem/net/http_server.h"

#include <brpc/closure_guard.h>
#include <brpc/controller.h>
#include <brpc/http_method.h>
#include <brpc/server.h>
#include <butil/errno.h>

#include <cctype>
#include <utility>

#include "http_router.pb.h"

namespace pgmem::net {
namespace {

std::string ToUpper(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return value;
}

std::string RouteKey(const std::string& method, const std::string& path) { return ToUpper(method) + " " + path; }

class HttpMasterServiceImpl final : public HttpMasterService {
public:
    explicit HttpMasterServiceImpl(HttpServer* server) : server_(server) {}

    void default_method(::google::protobuf::RpcController* cntl_base, const HttpMasterRequest*, HttpMasterResponse*,
                        ::google::protobuf::Closure* done) override {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(cntl_base);

        HttpRequest request;
        const char* method_text = brpc::HttpMethod2Str(cntl->http_request().method());
        request.method          = method_text != nullptr ? method_text : "GET";
        request.path            = cntl->http_request().uri().path();
        if (request.path.empty()) {
            request.path = "/";
        }
        if (!cntl->http_request().uri().query().empty()) {
            request.path.append("?");
            request.path.append(cntl->http_request().uri().query());
        }
        request.version = "HTTP/1.1";

        for (auto it = cntl->http_request().HeaderBegin(); it != cntl->http_request().HeaderEnd(); ++it) {
            request.headers[it->first] = it->second;
        }
        request.body = cntl->request_attachment().to_string();

        const HttpResponse response = server_->Dispatch(request);

        cntl->http_response().set_status_code(response.status_code);
        cntl->http_response().set_content_type(response.content_type);
        for (const auto& kv : response.headers) {
            cntl->http_response().SetHeader(kv.first, kv.second);
        }
        cntl->response_attachment().append(response.body);
    }

private:
    HttpServer* server_;
};

}  // namespace

HttpServer::HttpServer(std::string host, int port, HttpServerOptions options)
    : host_(std::move(host)), port_(port), options_(options) {}

HttpServer::~HttpServer() { Stop(); }

void HttpServer::RegisterRoute(const std::string& method, const std::string& path, Handler handler) {
    std::lock_guard<std::mutex> lock(mu_);
    routes_[RouteKey(method, path)] = std::move(handler);
}

bool HttpServer::Start(std::string* error) {
    if (running_.load()) {
        return true;
    }

    auto server = std::make_unique<brpc::Server>();
    brpc::ServerOptions server_options;
    if (options_.num_threads > 0) {
        server_options.num_threads = options_.num_threads;
    }
    server_options.http_master_service = new HttpMasterServiceImpl(this);

    const std::string endpoint = host_ + ":" + std::to_string(port_);
    if (server->Start(endpoint.c_str(), &server_options) != 0) {
        if (error != nullptr) {
            *error = std::string("brpc start failed: ") + berror(errno);
        }
        return false;
    }

    server_ = std::move(server);
    running_.store(true);
    return true;
}

void HttpServer::Stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (server_ != nullptr) {
        server_->Stop(0);
        server_->Join();
        server_.reset();
    }
}

HttpResponse HttpServer::Dispatch(const HttpRequest& request) {
    HttpResponse response;

    Handler handler;
    {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = routes_.find(RouteKey(request.method, request.path));
        if (it != routes_.end()) {
            handler = it->second;
        }
    }

    if (!handler) {
        response.status_code = 404;
        response.body        = "{\"error\":\"route not found\"}";
        return response;
    }

    try {
        return handler(request);
    } catch (const std::exception& ex) {
        response.status_code = 500;
        response.body        = std::string("{\"error\":\"") + ex.what() + "\"}";
        return response;
    }
}

}  // namespace pgmem::net
