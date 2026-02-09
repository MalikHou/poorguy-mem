#include <csignal>
#include <cstdlib>
#include <cctype>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "pgmem/core/memory_engine.h"
#include "pgmem/core/retriever.h"
#include "pgmem/core/sync.h"
#include "pgmem/mcp/mcp_dispatcher.h"
#include "pgmem/net/http_server.h"
#include "pgmem/store/store_adapter.h"
#include "pgmem/util/json.h"

namespace {

#ifndef PGMEM_DEFAULT_STORE_BACKEND
#define PGMEM_DEFAULT_STORE_BACKEND "eloqstore"
#endif

std::atomic<bool> g_running{true};

void OnSignal(int) {
    g_running.store(false);
}

std::string ToLower(std::string v) {
    for (char& c : v) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return v;
}

bool ParseBoolFlag(const std::string& value, bool fallback) {
    const std::string v = ToLower(value);
    if (v == "1" || v == "true" || v == "yes" || v == "on") {
        return true;
    }
    if (v == "0" || v == "false" || v == "no" || v == "off") {
        return false;
    }
    return fallback;
}

struct Args {
    std::string host{"127.0.0.1"};
    int port{8765};
    std::string store_backend{PGMEM_DEFAULT_STORE_BACKEND};
    std::string store_root{".pgmem/store"};
    int store_threads{0};
    bool append_mode{true};
    bool enable_compression{false};
    std::string s3_bucket_path;
    std::string s3_provider{"aws"};
    std::string s3_endpoint;
    std::string s3_region{"us-east-1"};
    std::string s3_access_key;
    std::string s3_secret_key;
    bool s3_verify_ssl{false};
    std::string node_id{"local"};
    std::string sync_host;
    int sync_port{0};
};

Args ParseArgs(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (key == "--host" && i + 1 < argc) {
            args.host = argv[++i];
        } else if (key == "--port" && i + 1 < argc) {
            args.port = std::atoi(argv[++i]);
        } else if (key == "--store-backend" && i + 1 < argc) {
            args.store_backend = argv[++i];
        } else if (key == "--store-root" && i + 1 < argc) {
            args.store_root = argv[++i];
        } else if (key == "--store-threads" && i + 1 < argc) {
            args.store_threads = std::atoi(argv[++i]);
        } else if (key == "--append-mode" && i + 1 < argc) {
            args.append_mode = ParseBoolFlag(argv[++i], args.append_mode);
        } else if (key == "--enable-compression" && i + 1 < argc) {
            args.enable_compression = ParseBoolFlag(argv[++i], args.enable_compression);
        } else if (key == "--s3-bucket-path" && i + 1 < argc) {
            args.s3_bucket_path = argv[++i];
        } else if (key == "--s3-provider" && i + 1 < argc) {
            args.s3_provider = argv[++i];
        } else if (key == "--s3-endpoint" && i + 1 < argc) {
            args.s3_endpoint = argv[++i];
        } else if (key == "--s3-region" && i + 1 < argc) {
            args.s3_region = argv[++i];
        } else if (key == "--s3-access-key" && i + 1 < argc) {
            args.s3_access_key = argv[++i];
        } else if (key == "--s3-secret-key" && i + 1 < argc) {
            args.s3_secret_key = argv[++i];
        } else if (key == "--s3-verify-ssl" && i + 1 < argc) {
            args.s3_verify_ssl = ParseBoolFlag(argv[++i], args.s3_verify_ssl);
        } else if (key == "--node-id" && i + 1 < argc) {
            args.node_id = argv[++i];
        } else if (key == "--sync-host" && i + 1 < argc) {
            args.sync_host = argv[++i];
        } else if (key == "--sync-port" && i + 1 < argc) {
            args.sync_port = std::atoi(argv[++i]);
        }
    }
    return args;
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = ParseArgs(argc, argv);

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    pgmem::store::StoreAdapterConfig store_config;
    store_config.backend = args.store_backend;
    store_config.root_path = args.store_root;
    if (args.store_threads > 0) {
        store_config.num_threads = static_cast<uint16_t>(args.store_threads);
    }
    store_config.append_mode = args.append_mode;
    store_config.enable_compression = args.enable_compression;

    store_config.s3.enabled = !args.s3_bucket_path.empty();
    store_config.s3.bucket_path = args.s3_bucket_path;
    store_config.s3.provider = args.s3_provider;
    store_config.s3.endpoint = args.s3_endpoint;
    store_config.s3.region = args.s3_region;
    store_config.s3.verify_ssl = args.s3_verify_ssl;
    store_config.s3.access_key = args.s3_access_key;
    store_config.s3.secret_key = args.s3_secret_key;

    if (store_config.s3.access_key.empty()) {
        const char* env = std::getenv("PGMEM_S3_ACCESS_KEY");
        if (env != nullptr) {
            store_config.s3.access_key = env;
        }
    }
    if (store_config.s3.secret_key.empty()) {
        const char* env = std::getenv("PGMEM_S3_SECRET_KEY");
        if (env != nullptr) {
            store_config.s3.secret_key = env;
        }
    }

    std::string error;
    std::unique_ptr<pgmem::store::IStoreAdapter> store =
        pgmem::store::CreateStoreAdapter(store_config, &error);
    if (!store) {
        const std::string backend = ToLower(store_config.backend);
        if (backend == "eloqstore") {
            std::cerr << "[pgmemd] failed to initialize eloqstore backend: " << error
                      << ", using in-memory fallback\n";
            store = pgmem::store::CreateInMemoryStoreAdapter();
        } else {
            std::cerr << "[pgmemd] failed to initialize backend '" << store_config.backend
                      << "': " << error << "\n";
            return 1;
        }
    }

    auto retriever = pgmem::core::CreateHybridRetriever();

    std::unique_ptr<pgmem::core::SyncWorker> sync_worker;
    if (!args.sync_host.empty() && args.sync_port > 0) {
        auto transport = std::make_unique<pgmem::core::HttpSyncTransport>(args.sync_host, args.sync_port, 2000);
        sync_worker = std::make_unique<pgmem::core::SyncWorker>(std::move(transport));
    }

    pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), std::move(sync_worker), args.node_id);
    engine.Warmup(&error);

    pgmem::mcp::McpDispatcher dispatcher(&engine);

    pgmem::net::HttpServer server(args.host, args.port);
    server.RegisterRoute("GET", "/health", [](const pgmem::net::HttpRequest&) {
        pgmem::net::HttpResponse resp;
        resp.status_code = 200;
        resp.body = "{\"status\":\"ok\"}";
        return resp;
    });

    server.RegisterRoute("POST", "/mcp", [&](const pgmem::net::HttpRequest& req) {
        pgmem::net::HttpResponse resp;

        pgmem::util::Json json;
        std::string parse_error;
        if (!pgmem::util::ParseJson(req.body, &json, &parse_error)) {
            resp.status_code = 400;
            resp.body = std::string("{\"error\":\"invalid JSON: ") + parse_error + "\"}";
            return resp;
        }

        const auto out = dispatcher.Handle(json);
        resp.status_code = 200;
        resp.body = pgmem::util::ToJsonString(out, false);
        return resp;
    });

    if (!server.Start(&error)) {
        std::cerr << "[pgmemd] start failed: " << error << "\n";
        return 1;
    }

    std::cerr << "[pgmemd] listening on http://" << args.host << ":" << args.port << "/mcp\n";

    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    server.Stop();
    return 0;
}
