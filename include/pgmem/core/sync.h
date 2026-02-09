#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "pgmem/net/http_client.h"
#include "pgmem/types.h"

namespace pgmem::core {

class ISyncTransport {
public:
    virtual ~ISyncTransport() = default;
    virtual bool PushOps(const std::vector<SyncOp>& ops, uint64_t* ack_cursor, std::string* error) = 0;
    virtual bool PullOps(uint64_t cursor, SyncBatch* out, std::string* error) = 0;
};

class HttpSyncTransport final : public ISyncTransport {
public:
    HttpSyncTransport(std::string host, int port, int timeout_ms);

    bool PushOps(const std::vector<SyncOp>& ops, uint64_t* ack_cursor, std::string* error) override;
    bool PullOps(uint64_t cursor, SyncBatch* out, std::string* error) override;

private:
    std::string host_;
    int port_{0};
    int timeout_ms_{2000};
    net::HttpClient http_;
};

class SyncWorker {
public:
    explicit SyncWorker(std::unique_ptr<ISyncTransport> transport);
    ~SyncWorker();

    void Start();
    void Stop();
    void Enqueue(SyncOp op);
    uint64_t Lag() const;

private:
    void Run();

    std::unique_ptr<ISyncTransport> transport_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<SyncOp> queue_;
    std::atomic<bool> running_{false};
    std::thread worker_;
};

}  // namespace pgmem::core
