#include "pgmem/core/sync.h"

#include <chrono>
#include <thread>

#include "pgmem/util/json.h"

namespace pgmem::core {
namespace {

std::string OpTypeToString(OpType type) {
    switch (type) {
        case OpType::Upsert:
            return "upsert";
        case OpType::Pin:
            return "pin";
        case OpType::Delete:
            return "delete";
        default:
            return "upsert";
    }
}

OpType OpTypeFromString(const std::string& s) {
    if (s == "pin") {
        return OpType::Pin;
    }
    if (s == "delete") {
        return OpType::Delete;
    }
    return OpType::Upsert;
}

}  // namespace

HttpSyncTransport::HttpSyncTransport(std::string host, int port, int timeout_ms)
    : host_(std::move(host)), port_(port), timeout_ms_(timeout_ms) {}

bool HttpSyncTransport::PushOps(const std::vector<SyncOp>& ops, uint64_t* ack_cursor, std::string* error) {
    util::Json req;
    util::Json arr;

    for (const auto& op : ops) {
        util::Json node;
        node.put("op_seq", op.op_seq);
        node.put("op_type", OpTypeToString(op.op_type));
        node.put("workspace_id", op.workspace_id);
        node.put("memory_id", op.memory_id);
        node.put("updated_at_ms", op.updated_at_ms);
        node.put("node_id", op.node_id);
        node.put("payload_json", op.payload_json);
        arr.push_back(std::make_pair("", node));
    }

    req.add_child("ops", arr);

    const auto resp = http_.PostJson(host_, port_, "/sync/push", util::ToJsonString(req, false), timeout_ms_);
    if (!resp.ok) {
        if (error != nullptr) {
            *error = resp.error.empty() ? resp.body : resp.error;
        }
        return false;
    }

    util::Json json;
    std::string parse_error;
    if (!util::ParseJson(resp.body, &json, &parse_error)) {
        if (error != nullptr) {
            *error = "invalid sync push response: " + parse_error;
        }
        return false;
    }

    if (ack_cursor != nullptr) {
        *ack_cursor = util::GetUint64Or(json, "ack_cursor", 0);
    }
    return true;
}

bool HttpSyncTransport::PullOps(uint64_t cursor, SyncBatch* out, std::string* error) {
    util::Json req;
    req.put("cursor", cursor);

    const auto resp = http_.PostJson(host_, port_, "/sync/pull", util::ToJsonString(req, false), timeout_ms_);
    if (!resp.ok) {
        if (error != nullptr) {
            *error = resp.error.empty() ? resp.body : resp.error;
        }
        return false;
    }

    util::Json json;
    std::string parse_error;
    if (!util::ParseJson(resp.body, &json, &parse_error)) {
        if (error != nullptr) {
            *error = "invalid sync pull response: " + parse_error;
        }
        return false;
    }

    if (out == nullptr) {
        if (error != nullptr) {
            *error = "null output";
        }
        return false;
    }

    out->cursor = util::GetUint64Or(json, "cursor", cursor);
    out->ops.clear();
    auto ops_opt = json.get_child_optional("ops");
    if (!ops_opt) {
        return true;
    }

    for (const auto& item : *ops_opt) {
        SyncOp op;
        op.op_seq = item.second.get<uint64_t>("op_seq", 0);
        op.op_type = OpTypeFromString(item.second.get<std::string>("op_type", "upsert"));
        op.workspace_id = item.second.get<std::string>("workspace_id", "");
        op.memory_id = item.second.get<std::string>("memory_id", "");
        op.updated_at_ms = item.second.get<uint64_t>("updated_at_ms", 0);
        op.node_id = item.second.get<std::string>("node_id", "");
        op.payload_json = item.second.get<std::string>("payload_json", "");
        out->ops.push_back(std::move(op));
    }
    return true;
}

SyncWorker::SyncWorker(std::unique_ptr<ISyncTransport> transport)
    : transport_(std::move(transport)) {}

SyncWorker::~SyncWorker() {
    Stop();
}

void SyncWorker::Start() {
    if (running_.exchange(true)) {
        return;
    }
    worker_ = std::thread(&SyncWorker::Run, this);
}

void SyncWorker::Stop() {
    if (!running_.exchange(false)) {
        return;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void SyncWorker::Enqueue(SyncOp op) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        queue_.push_back(std::move(op));
    }
    cv_.notify_one();
}

uint64_t SyncWorker::Lag() const {
    std::lock_guard<std::mutex> lock(mu_);
    return static_cast<uint64_t>(queue_.size());
}

void SyncWorker::Run() {
    int backoff_ms = 100;

    while (running_.load()) {
        std::vector<SyncOp> batch;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait_for(lock, std::chrono::milliseconds(200), [&] {
                return !running_.load() || !queue_.empty();
            });

            if (!running_.load()) {
                break;
            }

            const size_t n = std::min<size_t>(queue_.size(), 64);
            for (size_t i = 0; i < n; ++i) {
                batch.push_back(std::move(queue_.front()));
                queue_.pop_front();
            }
        }

        if (batch.empty() || !transport_) {
            continue;
        }

        uint64_t ack_cursor = 0;
        std::string error;
        if (!transport_->PushOps(batch, &ack_cursor, &error)) {
            {
                std::lock_guard<std::mutex> lock(mu_);
                for (auto it = batch.rbegin(); it != batch.rend(); ++it) {
                    queue_.push_front(std::move(*it));
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
            backoff_ms = std::min(backoff_ms * 2, 2000);
            continue;
        }

        backoff_ms = 100;
    }
}

}  // namespace pgmem::core
