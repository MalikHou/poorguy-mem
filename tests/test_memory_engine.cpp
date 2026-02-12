#include <atomic>
#include <chrono>
#include <functional>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "pgmem/core/memory_engine.h"
#include "pgmem/core/retriever.h"
#include "pgmem/store/store_adapter.h"
#include "pgmem/util/json.h"
#include "test_framework.h"

namespace {

class ControlledBatchStoreAdapter final : public pgmem::store::IStoreAdapter {
public:
    ControlledBatchStoreAdapter(int fail_count, bool fail_forever)
        : inner_(pgmem::store::CreateInMemoryStoreAdapter()),
          remaining_failures_(fail_count),
          fail_forever_(fail_forever) {}

    pgmem::StoreResult Put(const std::string& name_space, const std::string& key, const std::string& value,
                           uint64_t ts) override {
        return inner_->Put(name_space, key, value, ts);
    }

    pgmem::GetResult Get(const std::string& name_space, const std::string& key) override {
        return inner_->Get(name_space, key);
    }

    pgmem::StoreResult Delete(const std::string& name_space, const std::string& key, uint64_t ts) override {
        return inner_->Delete(name_space, key, ts);
    }

    std::vector<pgmem::KeyValueEntry> Scan(const std::string& name_space, const std::string& begin,
                                           const std::string& end, size_t limit, std::string* error) override {
        return inner_->Scan(name_space, begin, end, limit, error);
    }

    pgmem::StoreResult BatchWrite(const std::vector<pgmem::WriteEntry>& entries) override {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (fail_forever_) {
                return pgmem::StoreResult{false, "injected batch failure"};
            }
            if (remaining_failures_ > 0) {
                --remaining_failures_;
                return pgmem::StoreResult{false, "injected batch failure"};
            }
        }
        return inner_->BatchWrite(entries);
    }

    pgmem::StoreUsage ApproximateUsage(std::string* error) override { return inner_->ApproximateUsage(error); }

private:
    std::unique_ptr<pgmem::store::IStoreAdapter> inner_;
    std::mutex mu_;
    int remaining_failures_{0};
    bool fail_forever_{false};
};

bool WaitUntil(const std::function<bool()>& predicate, int timeout_ms, int sleep_step_ms) {
    const auto start = std::chrono::steady_clock::now();
    while (true) {
        if (predicate()) {
            return true;
        }
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        if (elapsed >= timeout_ms) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_step_ms));
    }
}

std::string ZeroPad20(uint64_t value) {
    std::ostringstream oss;
    oss << std::setw(20) << std::setfill('0') << value;
    return oss.str();
}

std::string BuildRecordPayload(const std::string& workspace_id, const std::string& memory_id,
                               const std::string& content, uint64_t updated_at_ms, uint64_t version,
                               const std::string& node_id) {
    pgmem::util::Json record;
    record.put("id", memory_id);
    record.put("workspace_id", workspace_id);
    record.put("session_id", "s1");
    record.put("source", "commit_turn");
    record.put("content", content);
    record.put("pinned", false);
    record.put("tombstone", false);
    record.put("created_at_ms", updated_at_ms);
    record.put("updated_at_ms", updated_at_ms);
    record.put("version", version);
    record.put("last_access_ms", updated_at_ms);
    record.put("hit_count", 0);
    record.put("importance_score", 1.0);
    record.put("tier", "hot");
    record.put("node_id", node_id);
    record.add_child("tags", pgmem::util::MakeArray({"turn"}));
    return pgmem::util::ToJsonString(record, false);
}

std::string BuildProjectionEventPayload(const std::string& event_key, const std::string& workspace_id,
                                        const std::string& memory_id, const std::string& record_payload,
                                        const std::string& node_id, uint64_t updated_at_ms, uint64_t version) {
    pgmem::util::Json event;
    event.put("event_key", event_key);
    event.put("workspace_id", workspace_id);
    event.put("memory_id", memory_id);
    event.put("event_type", "commit_turn");
    event.put("op_type", "upsert");
    event.put("updated_at_ms", updated_at_ms);
    event.put("version", version);
    event.put("node_id", node_id);
    event.put("record_payload", record_payload);
    return pgmem::util::ToJsonString(event, false);
}

std::string ReadRecordContent(pgmem::store::IStoreAdapter* store, const std::string& workspace_id,
                              const std::string& memory_id) {
    const auto out = store->Get("mem_items", workspace_id + ":" + memory_id);
    if (!out.found) {
        return {};
    }
    pgmem::util::Json json;
    std::string error;
    if (!pgmem::util::ParseJson(out.value, &json, &error)) {
        return {};
    }
    return pgmem::util::GetStringOr(json, "content", "");
}

}  // namespace

TEST_CASE(test_memory_engine_commit_and_search) {
    auto store     = pgmem::store::CreateInMemoryStoreAdapter();
    auto retriever = pgmem::core::CreateHybridRetriever();

    pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), "node-a");

    pgmem::CommitTurnInput input;
    input.workspace_id   = "ws";
    input.session_id     = "sess";
    input.user_text      = "Please add retry logic";
    input.assistant_text = "I will add exponential backoff";
    input.code_snippets  = {"int retry = 3;"};
    input.commands       = {"make test"};

    const auto commit_out = engine.CommitTurn(input);
    ASSERT_EQ(commit_out.stored_ids.size(), 1U);

    pgmem::SearchInput search;
    search.workspace_id = "ws";
    search.query        = "retry backoff";
    search.top_k        = 3;
    search.token_budget = 2048;

    const auto search_out = engine.Search(search);
    ASSERT_TRUE(!search_out.hits.empty());
    ASSERT_EQ(search_out.hits[0].memory_id, commit_out.stored_ids[0]);
}

TEST_CASE(test_memory_engine_pin) {
    auto store     = pgmem::store::CreateInMemoryStoreAdapter();
    auto retriever = pgmem::core::CreateHybridRetriever();

    pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), "node-a");

    pgmem::CommitTurnInput input;
    input.workspace_id   = "ws";
    input.session_id     = "sess";
    input.user_text      = "remember this";
    input.assistant_text = "done";

    const auto commit_out = engine.CommitTurn(input);
    ASSERT_EQ(commit_out.stored_ids.size(), 1U);

    std::string error;
    ASSERT_TRUE(engine.Pin("ws", commit_out.stored_ids[0], true, &error));
}

TEST_CASE(test_memory_engine_compact_tombstone_and_stats) {
    auto store     = pgmem::store::CreateInMemoryStoreAdapter();
    auto retriever = pgmem::core::CreateHybridRetriever();

    pgmem::core::MemoryEngineOptions options;
    options.enable_tombstone_gc = false;
    options.gc_batch_size       = 32;

    pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), "node-a", options);

    std::string long_text(1600, 'x');

    pgmem::CommitTurnInput input;
    input.workspace_id    = "ws";
    input.session_id      = "sess";
    input.user_text       = "keep memory growth under control";
    input.assistant_text  = long_text;
    const auto commit_out = engine.CommitTurn(input);
    ASSERT_EQ(commit_out.stored_ids.size(), 1U);

    const auto compact_out = engine.Compact("ws");
    ASSERT_TRUE(compact_out.triggered);
    ASSERT_TRUE(compact_out.summarized_count > 0 || compact_out.tombstoned_count > 0);

    const auto stats = engine.Stats("ws", "5m");
    ASSERT_TRUE(stats.item_count > 0);
    ASSERT_TRUE(stats.tombstone_count > 0);
}

TEST_CASE(test_memory_engine_writes_projection_events_and_checkpoint) {
    auto store      = pgmem::store::CreateInMemoryStoreAdapter();
    auto* store_ptr = store.get();
    auto retriever  = pgmem::core::CreateHybridRetriever();

    pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), "node-a");

    pgmem::CommitTurnInput input;
    input.workspace_id   = "ws-proj";
    input.session_id     = "sess";
    input.user_text      = "remember retry settings";
    input.assistant_text = "retry with exponential backoff";

    const auto out = engine.CommitTurn(input);
    ASSERT_EQ(out.stored_ids.size(), 1U);

    std::string scan_error;
    std::vector<pgmem::KeyValueEntry> events;
    const bool events_persisted = WaitUntil(
        [&]() {
            scan_error.clear();
            events = store_ptr->Scan("ds_events", "ws-proj:", "ws-proj;", 0, &scan_error);
            return scan_error.empty() && !events.empty();
        },
        1000, 10);
    ASSERT_TRUE(events_persisted);
    ASSERT_TRUE(scan_error.empty());
    ASSERT_TRUE(!events.empty());

    pgmem::util::Json event_json;
    std::string parse_error;
    ASSERT_TRUE(pgmem::util::ParseJson(events.back().value, &event_json, &parse_error));
    ASSERT_EQ(pgmem::util::GetStringOr(event_json, "workspace_id", ""), "ws-proj");
    ASSERT_TRUE(!pgmem::util::GetStringOr(event_json, "record_payload", "").empty());

    const bool checkpoint_persisted =
        WaitUntil([&]() { return store_ptr->Get("ds_projection_ckpt", "ws-proj").found; }, 1000, 10);
    ASSERT_TRUE(checkpoint_persisted);
}

TEST_CASE(test_memory_engine_compact_all_workspaces_runs_workspace_scoped_batches) {
    auto store     = pgmem::store::CreateInMemoryStoreAdapter();
    auto retriever = pgmem::core::CreateHybridRetriever();

    pgmem::core::MemoryEngineOptions options;
    options.enable_tombstone_gc = false;
    options.gc_batch_size       = 64;

    pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), "node-a", options);

    std::string long_text(1800, 'x');

    pgmem::CommitTurnInput ws1;
    ws1.workspace_id   = "ws1";
    ws1.session_id     = "sess";
    ws1.user_text      = "first workspace";
    ws1.assistant_text = long_text;
    ASSERT_EQ(engine.CommitTurn(ws1).stored_ids.size(), 1U);

    pgmem::CommitTurnInput ws2;
    ws2.workspace_id   = "ws2";
    ws2.session_id     = "sess";
    ws2.user_text      = "second workspace";
    ws2.assistant_text = long_text;
    ASSERT_EQ(engine.CommitTurn(ws2).stored_ids.size(), 1U);

    const auto compact_out = engine.Compact("");
    ASSERT_TRUE(compact_out.triggered);

    const auto stats_ws1 = engine.Stats("ws1", "5m");
    const auto stats_ws2 = engine.Stats("ws2", "5m");
    ASSERT_TRUE(stats_ws1.item_count > 0);
    ASSERT_TRUE(stats_ws2.item_count > 0);
}

TEST_CASE(test_memory_engine_warmup_replay_lww_conflict_resolution) {
    auto store      = pgmem::store::CreateInMemoryStoreAdapter();
    auto* store_ptr = store.get();

    const std::string workspace_id = "ws-replay";
    const std::string memory_id    = "m1";

    const std::string old_payload = BuildRecordPayload(workspace_id, memory_id, "old_payload_only", 1000, 1, "node-a");
    ASSERT_TRUE(store_ptr->Put("mem_items", workspace_id + ":" + memory_id, old_payload, 1000).ok);

    const std::string newer_payload =
        BuildRecordPayload(workspace_id, memory_id, "new_payload_applied", 2000, 2, "node-b");
    const std::string stale_late_payload =
        BuildRecordPayload(workspace_id, memory_id, "stale_payload_ignored", 1500, 1, "node-z");

    const std::string newer_event_key = workspace_id + ":" + ZeroPad20(2000) + ":" + ZeroPad20(1) + ":" + memory_id;
    const std::string stale_late_event_key =
        workspace_id + ":" + ZeroPad20(3000) + ":" + ZeroPad20(2) + ":" + memory_id;

    ASSERT_TRUE(store_ptr
                    ->Put("ds_events", newer_event_key,
                          BuildProjectionEventPayload(newer_event_key, workspace_id, memory_id, newer_payload, "node-b",
                                                      2000, 2),
                          2000)
                    .ok);
    ASSERT_TRUE(store_ptr
                    ->Put("ds_events", stale_late_event_key,
                          BuildProjectionEventPayload(stale_late_event_key, workspace_id, memory_id, stale_late_payload,
                                                      "node-z", 1500, 1),
                          3000)
                    .ok);

    auto retriever = pgmem::core::CreateHybridRetriever();
    pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), "node-a");
    std::string error;
    ASSERT_TRUE(engine.Warmup(&error));

    pgmem::SearchInput search;
    search.workspace_id = workspace_id;
    search.query        = "new_payload_applied";
    search.top_k        = 1;
    search.token_budget = 1024;
    const auto out      = engine.Search(search);
    ASSERT_TRUE(!out.hits.empty());
    ASSERT_EQ(out.hits[0].memory_id, memory_id);
    ASSERT_TRUE(out.hits[0].content.find("new_payload_applied") != std::string::npos);

    const std::string persisted_content = ReadRecordContent(store_ptr, workspace_id, memory_id);
    ASSERT_TRUE(persisted_content.find("new_payload_applied") != std::string::npos);
    ASSERT_TRUE(persisted_content.find("stale_payload_ignored") == std::string::npos);
}

TEST_CASE(test_memory_engine_warmup_replay_checkpoint_skips_old_events) {
    auto store      = pgmem::store::CreateInMemoryStoreAdapter();
    auto* store_ptr = store.get();

    const std::string workspace_id = "ws-ckpt";
    const std::string memory_id    = "m2";

    const std::string skipped_payload =
        BuildRecordPayload(workspace_id, memory_id, "event_before_checkpoint_should_skip", 1000, 1, "node-a");
    const std::string applied_payload =
        BuildRecordPayload(workspace_id, memory_id, "event_after_checkpoint_should_apply", 2000, 2, "node-b");

    const std::string skipped_event_key = workspace_id + ":" + ZeroPad20(1000) + ":" + ZeroPad20(1) + ":" + memory_id;
    const std::string applied_event_key = workspace_id + ":" + ZeroPad20(2000) + ":" + ZeroPad20(2) + ":" + memory_id;

    ASSERT_TRUE(store_ptr
                    ->Put("ds_events", skipped_event_key,
                          BuildProjectionEventPayload(skipped_event_key, workspace_id, memory_id, skipped_payload,
                                                      "node-a", 1000, 1),
                          1000)
                    .ok);
    ASSERT_TRUE(store_ptr
                    ->Put("ds_events", applied_event_key,
                          BuildProjectionEventPayload(applied_event_key, workspace_id, memory_id, applied_payload,
                                                      "node-b", 2000, 2),
                          2000)
                    .ok);

    pgmem::util::Json ckpt;
    ckpt.put("workspace_id", workspace_id);
    ckpt.put("event_key", skipped_event_key);
    ckpt.put("updated_at_ms", 1000);
    ASSERT_TRUE(store_ptr->Put("ds_projection_ckpt", workspace_id, pgmem::util::ToJsonString(ckpt, false), 1000).ok);

    auto retriever = pgmem::core::CreateHybridRetriever();
    pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), "node-a");
    std::string error;
    ASSERT_TRUE(engine.Warmup(&error));

    const std::string persisted_content = ReadRecordContent(store_ptr, workspace_id, memory_id);
    ASSERT_TRUE(persisted_content.find("event_after_checkpoint_should_apply") != std::string::npos);
    ASSERT_TRUE(persisted_content.find("event_before_checkpoint_should_skip") == std::string::npos);

    const auto latest_ckpt = store_ptr->Get("ds_projection_ckpt", workspace_id);
    ASSERT_TRUE(latest_ckpt.found);
    pgmem::util::Json latest_ckpt_json;
    std::string parse_error;
    ASSERT_TRUE(pgmem::util::ParseJson(latest_ckpt.value, &latest_ckpt_json, &parse_error));
    ASSERT_EQ(pgmem::util::GetStringOr(latest_ckpt_json, "event_key", ""), applied_event_key);
}

TEST_CASE(test_memory_engine_accepted_mode_reports_queue_stats) {
    auto store     = pgmem::store::CreateInMemoryStoreAdapter();
    auto retriever = pgmem::core::CreateHybridRetriever();

    pgmem::core::MemoryEngineOptions options;
    options.write_ack_mode             = pgmem::core::WriteAckMode::Accepted;
    options.volatile_flush_interval_ms = 10;
    options.volatile_max_pending_ops   = 64;
    options.shutdown_drain_timeout_ms  = 1000;
    options.effective_backend          = "inmemory";

    pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), "node-a", options);

    pgmem::CommitTurnInput input;
    input.workspace_id   = "ws";
    input.session_id     = "sess";
    input.user_text      = "accepted mode memory";
    input.assistant_text = "queued then flushed";
    const auto out       = engine.CommitTurn(input);
    ASSERT_EQ(out.stored_ids.size(), 1U);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto stats = engine.Stats("ws", "5m");
    ASSERT_EQ(stats.write_ack_mode, "accepted");
    ASSERT_EQ(stats.effective_backend, "inmemory");
    ASSERT_TRUE(stats.pending_write_ops <= 64);
}

TEST_CASE(test_memory_engine_accepted_mode_retries_flush_failures) {
    auto store     = std::make_unique<ControlledBatchStoreAdapter>(2, false);
    auto retriever = pgmem::core::CreateHybridRetriever();

    pgmem::core::MemoryEngineOptions options;
    options.write_ack_mode             = pgmem::core::WriteAckMode::Accepted;
    options.volatile_flush_interval_ms = 5;
    options.volatile_max_pending_ops   = 512;
    options.shutdown_drain_timeout_ms  = 1000;
    options.effective_backend          = "inmemory";

    pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), "node-a", options);

    pgmem::CommitTurnInput input;
    input.workspace_id   = "ws-retry";
    input.session_id     = "sess";
    input.user_text      = "retry flush";
    input.assistant_text = "background retry";
    const auto out       = engine.CommitTurn(input);
    ASSERT_EQ(out.stored_ids.size(), 1U);

    const bool drained = WaitUntil([&]() { return engine.Stats("ws-retry", "5m").pending_write_ops == 0; }, 3000, 20);
    ASSERT_TRUE(drained);

    const auto stats = engine.Stats("ws-retry", "5m");
    ASSERT_EQ(stats.write_ack_mode, "accepted");
    ASSERT_TRUE(stats.flush_failures_total >= 2);
    ASSERT_TRUE(stats.last_flush_error.empty());

    pgmem::SearchInput search;
    search.workspace_id   = "ws-retry";
    search.query          = "retry flush";
    search.top_k          = 1;
    search.token_budget   = 1024;
    const auto search_out = engine.Search(search);
    ASSERT_TRUE(!search_out.hits.empty());
}

TEST_CASE(test_memory_engine_accepted_mode_queue_full_falls_back_to_sync_write) {
    auto store     = pgmem::store::CreateInMemoryStoreAdapter();
    auto retriever = pgmem::core::CreateHybridRetriever();

    pgmem::core::MemoryEngineOptions options;
    options.write_ack_mode             = pgmem::core::WriteAckMode::Accepted;
    options.volatile_flush_interval_ms = 1000;
    options.volatile_max_pending_ops   = 1;
    options.shutdown_drain_timeout_ms  = 1000;
    options.effective_backend          = "inmemory";

    pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), "node-a", options);

    pgmem::CommitTurnInput input;
    input.workspace_id   = "ws-fallback";
    input.session_id     = "sess";
    input.user_text      = "queue full fallback";
    input.assistant_text = "sync write fallback should persist";
    const auto out       = engine.CommitTurn(input);
    ASSERT_EQ(out.stored_ids.size(), 1U);

    const auto stats = engine.Stats("ws-fallback", "5m");
    ASSERT_EQ(stats.write_ack_mode, "accepted");
    ASSERT_EQ(stats.pending_write_ops, 0U);
    ASSERT_EQ(stats.flush_failures_total, 0U);

    pgmem::SearchInput search;
    search.workspace_id   = "ws-fallback";
    search.query          = "queue full fallback";
    search.top_k          = 1;
    search.token_budget   = 1024;
    const auto search_out = engine.Search(search);
    ASSERT_TRUE(!search_out.hits.empty());
}

TEST_CASE(test_memory_engine_accepted_mode_shutdown_drain_timeout_does_not_hang) {
    auto store     = std::make_unique<ControlledBatchStoreAdapter>(0, true);
    auto retriever = pgmem::core::CreateHybridRetriever();

    pgmem::core::MemoryEngineOptions options;
    options.write_ack_mode             = pgmem::core::WriteAckMode::Accepted;
    options.volatile_flush_interval_ms = 5;
    options.volatile_max_pending_ops   = 512;
    options.shutdown_drain_timeout_ms  = 50;
    options.effective_backend          = "inmemory";

    const auto start = std::chrono::steady_clock::now();
    {
        pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), "node-a", options);

        pgmem::CommitTurnInput input;
        input.workspace_id   = "ws-shutdown";
        input.session_id     = "sess";
        input.user_text      = "shutdown timeout";
        input.assistant_text = "flush always failing";
        const auto out       = engine.CommitTurn(input);
        ASSERT_EQ(out.stored_ids.size(), 1U);

        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        const auto stats = engine.Stats("ws-shutdown", "5m");
        ASSERT_TRUE(stats.pending_write_ops > 0);
    }
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    ASSERT_TRUE(elapsed_ms < 1500);
}

TEST_CASE(test_memory_engine_accepted_mode_shutdown_reports_dropped_count) {
    auto store     = std::make_unique<ControlledBatchStoreAdapter>(0, true);
    auto retriever = pgmem::core::CreateHybridRetriever();

    pgmem::core::MemoryEngineOptions options;
    options.write_ack_mode             = pgmem::core::WriteAckMode::Accepted;
    options.volatile_flush_interval_ms = 5;
    options.volatile_max_pending_ops   = 512;
    options.shutdown_drain_timeout_ms  = 20;
    options.effective_backend          = "inmemory";

    pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), "node-a", options);

    pgmem::CommitTurnInput input;
    input.workspace_id   = "ws-drop";
    input.session_id     = "sess";
    input.user_text      = "drop test";
    input.assistant_text = "flush always failing";
    const auto out       = engine.CommitTurn(input);
    ASSERT_EQ(out.stored_ids.size(), 1U);

    const bool queued = WaitUntil([&]() { return engine.Stats("ws-drop", "5m").pending_write_ops > 0; }, 1000, 10);
    ASSERT_TRUE(queued);

    const auto start = std::chrono::steady_clock::now();
    engine.Shutdown();
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    ASSERT_TRUE(elapsed_ms < 1500);

    const auto stats = engine.Stats("ws-drop", "5m");
    ASSERT_EQ(stats.pending_write_ops, 0U);
    ASSERT_TRUE(stats.volatile_dropped_on_shutdown > 0);
}

TEST_CASE(test_memory_engine_compact_with_tombstone_gc_physically_deletes_records) {
    auto store      = pgmem::store::CreateInMemoryStoreAdapter();
    auto* store_ptr = store.get();
    auto retriever  = pgmem::core::CreateHybridRetriever();

    pgmem::core::MemoryEngineOptions options;
    options.enable_tombstone_gc    = true;
    options.tombstone_retention_ms = 0;
    options.gc_batch_size          = 64;

    pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), "node-a", options);

    std::string long_text(1800, 'x');
    pgmem::CommitTurnInput input;
    input.workspace_id    = "ws-gc";
    input.session_id      = "sess";
    input.user_text       = "force tombstone";
    input.assistant_text  = long_text;
    const auto commit_out = engine.CommitTurn(input);
    ASSERT_EQ(commit_out.stored_ids.size(), 1U);
    const std::string original_id = commit_out.stored_ids[0];

    const auto first_compact = engine.Compact("ws-gc");
    ASSERT_TRUE(first_compact.triggered);
    ASSERT_TRUE(first_compact.tombstoned_count > 0);

    const auto second_compact = engine.Compact("ws-gc");
    ASSERT_TRUE(second_compact.triggered);
    ASSERT_TRUE(second_compact.deleted_count > 0);

    const auto original_record = store_ptr->Get("mem_items", "ws-gc:" + original_id);
    ASSERT_TRUE(!original_record.found);
}

TEST_CASE(test_memory_engine_bootstrap_respects_token_budget) {
    auto store     = pgmem::store::CreateInMemoryStoreAdapter();
    auto retriever = pgmem::core::CreateHybridRetriever();

    pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), "node-a");

    pgmem::CommitTurnInput in1;
    in1.workspace_id   = "ws-bootstrap";
    in1.session_id     = "s1";
    in1.user_text      = "budget token alpha alpha alpha alpha alpha";
    in1.assistant_text = "budget token alpha alpha alpha alpha alpha";
    ASSERT_EQ(engine.CommitTurn(in1).stored_ids.size(), 1U);

    pgmem::CommitTurnInput in2;
    in2.workspace_id   = "ws-bootstrap";
    in2.session_id     = "s1";
    in2.user_text      = "budget token beta beta beta beta beta";
    in2.assistant_text = "budget token beta beta beta beta beta";
    ASSERT_EQ(engine.CommitTurn(in2).stored_ids.size(), 1U);

    pgmem::BootstrapInput bootstrap;
    bootstrap.workspace_id = "ws-bootstrap";
    bootstrap.session_id   = "s2";
    bootstrap.task_text    = "budget token";
    bootstrap.open_files   = {};
    bootstrap.token_budget = 35;

    const auto out = engine.Bootstrap(bootstrap);
    ASSERT_TRUE(!out.recalled_items.empty());
    ASSERT_TRUE(out.recalled_items.size() <= 1U);
}
