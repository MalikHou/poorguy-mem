#include <atomic>
#include <chrono>
#include <csignal>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "pgmem/core/memory_engine.h"
#include "pgmem/core/retriever.h"
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

pgmem::OpType ParseOpType(const std::string& op) {
    if (op == "pin") {
        return pgmem::OpType::Pin;
    }
    if (op == "delete") {
        return pgmem::OpType::Delete;
    }
    return pgmem::OpType::Upsert;
}

std::string OpTypeToString(pgmem::OpType type) {
    switch (type) {
        case pgmem::OpType::Pin:
            return "pin";
        case pgmem::OpType::Delete:
            return "delete";
        case pgmem::OpType::Upsert:
        default:
            return "upsert";
    }
}

struct Args {
    std::string host{"127.0.0.1"};
    int port{9765};
    std::string store_backend{PGMEM_DEFAULT_STORE_BACKEND};
    std::string store_root{".pgmem/sync-store"};
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
    std::string node_id{"syncd"};
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
            std::cerr << "[pgmem-syncd] failed to initialize eloqstore backend: " << error
                      << ", using in-memory fallback\n";
            store = pgmem::store::CreateInMemoryStoreAdapter();
        } else {
            std::cerr << "[pgmem-syncd] failed to initialize backend '" << store_config.backend
                      << "': " << error << "\n";
            return 1;
        }
    }

    auto retriever = pgmem::core::CreateHybridRetriever();
    pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), nullptr, args.node_id);
    engine.Warmup(&error);

    std::mutex sync_mu;
    std::vector<pgmem::SyncOp> sync_history;
    uint64_t ack_cursor = 0;

    pgmem::net::HttpServer server(args.host, args.port);
    server.RegisterRoute("GET", "/health", [](const pgmem::net::HttpRequest&) {
        pgmem::net::HttpResponse resp;
        resp.status_code = 200;
        resp.body = "{\"status\":\"ok\"}";
        return resp;
    });

    server.RegisterRoute("POST", "/sync/push", [&](const pgmem::net::HttpRequest& req) {
        pgmem::net::HttpResponse resp;

        pgmem::util::Json json;
        std::string parse_error;
        if (!pgmem::util::ParseJson(req.body, &json, &parse_error)) {
            resp.status_code = 400;
            resp.body = std::string("{\"error\":\"invalid JSON: ") + parse_error + "\"}";
            return resp;
        }

        auto ops_opt = json.get_child_optional("ops");
        if (!ops_opt) {
            resp.status_code = 422;
            resp.body = "{\"error\":\"missing ops\"}";
            return resp;
        }

        uint64_t local_ack = ack_cursor;

        for (const auto& item : *ops_opt) {
            pgmem::SyncOp op;
            op.op_seq = item.second.get<uint64_t>("op_seq", 0);
            op.op_type = ParseOpType(item.second.get<std::string>("op_type", "upsert"));
            op.workspace_id = item.second.get<std::string>("workspace_id", "");
            op.memory_id = item.second.get<std::string>("memory_id", "");
            op.updated_at_ms = item.second.get<uint64_t>("updated_at_ms", 0);
            op.node_id = item.second.get<std::string>("node_id", "");
            op.payload_json = item.second.get<std::string>("payload_json", "");

            std::string apply_error;
            if (!engine.ApplySyncOp(op, &apply_error)) {
                resp.status_code = 500;
                resp.body = std::string("{\"error\":\"apply failed: ") + apply_error + "\"}";
                return resp;
            }

            {
                std::lock_guard<std::mutex> lock(sync_mu);
                sync_history.push_back(op);
            }
            if (op.op_seq > local_ack) {
                local_ack = op.op_seq;
            }
        }

        {
            std::lock_guard<std::mutex> lock(sync_mu);
            ack_cursor = std::max(ack_cursor, local_ack);
        }

        pgmem::util::Json out;
        out.put("ack_cursor", local_ack);
        resp.status_code = 200;
        resp.body = pgmem::util::ToJsonString(out, false);
        return resp;
    });

    server.RegisterRoute("POST", "/sync/pull", [&](const pgmem::net::HttpRequest& req) {
        pgmem::net::HttpResponse resp;

        pgmem::util::Json json;
        std::string parse_error;
        if (!pgmem::util::ParseJson(req.body, &json, &parse_error)) {
            resp.status_code = 400;
            resp.body = std::string("{\"error\":\"invalid JSON: ") + parse_error + "\"}";
            return resp;
        }

        const uint64_t cursor = json.get<uint64_t>("cursor", 0);
        const uint64_t limit = json.get<uint64_t>("limit", 128);

        pgmem::util::Json out;
        pgmem::util::Json ops;

        uint64_t max_cursor = cursor;
        {
            std::lock_guard<std::mutex> lock(sync_mu);
            uint64_t emitted = 0;
            for (const auto& op : sync_history) {
                if (op.op_seq <= cursor) {
                    continue;
                }
                pgmem::util::Json node;
                node.put("op_seq", op.op_seq);
                node.put("op_type", OpTypeToString(op.op_type));
                node.put("workspace_id", op.workspace_id);
                node.put("memory_id", op.memory_id);
                node.put("updated_at_ms", op.updated_at_ms);
                node.put("node_id", op.node_id);
                node.put("payload_json", op.payload_json);
                ops.push_back(std::make_pair("", node));

                max_cursor = std::max(max_cursor, op.op_seq);
                ++emitted;
                if (emitted >= limit) {
                    break;
                }
            }
        }

        out.put("cursor", max_cursor);
        out.add_child("ops", ops);
        resp.status_code = 200;
        resp.body = pgmem::util::ToJsonString(out, false);
        return resp;
    });

    if (!server.Start(&error)) {
        std::cerr << "[pgmem-syncd] start failed: " << error << "\n";
        return 1;
    }

    std::cerr << "[pgmem-syncd] listening on http://" << args.host << ":" << args.port << "\n";

    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    server.Stop();
    return 0;
}
