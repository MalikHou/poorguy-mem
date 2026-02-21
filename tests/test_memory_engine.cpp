#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "pgmem/core/memory_engine.h"
#include "pgmem/core/retriever.h"
#include "pgmem/store/store_adapter.h"
#include "pgmem/util/json.h"
#include "test_framework.h"

namespace {

pgmem::WriteInput BuildSingleWrite(const std::string& workspace, const std::string& content,
                                   const std::string& dedup_key = "") {
    pgmem::WriteInput in;
    in.workspace_id = workspace;
    in.session_id   = "s1";

    pgmem::WriteRecordInput rec;
    rec.source    = "turn";
    rec.content   = content;
    rec.dedup_key = dedup_key;
    in.records.push_back(rec);
    return in;
}

class HookedStoreAdapter final : public pgmem::store::IStoreAdapter {
public:
    explicit HookedStoreAdapter(std::shared_ptr<pgmem::store::IStoreAdapter> inner) : inner_(std::move(inner)) {}

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
        const auto it = scan_error_once_.find(name_space);
        if (it != scan_error_once_.end()) {
            if (error != nullptr) {
                *error = it->second;
            }
            scan_error_once_.erase(it);
            return {};
        }
        return inner_->Scan(name_space, begin, end, limit, error);
    }

    pgmem::StoreResult BatchWrite(const std::vector<pgmem::WriteEntry>& entries) override {
        if (fail_batch_write_) {
            return pgmem::StoreResult{false, batch_error_message_};
        }
        return inner_->BatchWrite(entries);
    }

    pgmem::StoreUsage ApproximateUsage(std::string* error) override {
        if (force_usage_) {
            if (error != nullptr) {
                error->clear();
            }
            return forced_usage_;
        }
        return inner_->ApproximateUsage(error);
    }

    pgmem::StoreCompactTriggerResult TriggerStoreCompactAsync() override { return inner_->TriggerStoreCompactAsync(); }

    void SetBatchFailure(const std::string& message) {
        fail_batch_write_    = true;
        batch_error_message_ = message;
    }

    void SetScanErrorOnce(const std::string& name_space, const std::string& error) {
        scan_error_once_[name_space] = error;
    }

    void SetForcedUsage(const pgmem::StoreUsage& usage) {
        forced_usage_ = usage;
        force_usage_  = true;
    }

private:
    std::shared_ptr<pgmem::store::IStoreAdapter> inner_;
    bool fail_batch_write_{false};
    std::string batch_error_message_{"injected batch failure"};
    std::map<std::string, std::string> scan_error_once_;
    bool force_usage_{false};
    pgmem::StoreUsage forced_usage_{};
};

std::shared_ptr<pgmem::store::IStoreAdapter> MakeSharedInMemoryStore() {
    return std::shared_ptr<pgmem::store::IStoreAdapter>(pgmem::store::CreateInMemoryStoreAdapter().release());
}

bool HasHitContent(const pgmem::QueryOutput& out, const std::string& needle) {
    for (const auto& hit : out.hits) {
        if (hit.content.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE(test_memory_engine_write_and_query) {
    auto store     = pgmem::store::CreateInMemoryStoreAdapter();
    auto retriever = pgmem::core::CreateHybridRetriever();
    pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), "node-a");

    const auto write_out = engine.Write(BuildSingleWrite("ws", "remember retry with exponential backoff"));
    ASSERT_TRUE(write_out.ok);
    ASSERT_EQ(write_out.stored_ids.size(), 1U);

    pgmem::QueryInput query;
    query.workspace_id = "ws";
    query.query        = "retry backoff";
    query.top_k        = 3;

    const auto query_out = engine.Query(query);
    ASSERT_TRUE(!query_out.hits.empty());
    ASSERT_EQ(query_out.hits[0].memory_id, write_out.stored_ids[0]);
}

TEST_CASE(test_memory_engine_pin_and_query_filter) {
    auto store     = pgmem::store::CreateInMemoryStoreAdapter();
    auto retriever = pgmem::core::CreateHybridRetriever();
    pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), "node-a");

    auto write     = BuildSingleWrite("ws", "critical runbook entry");
    const auto out = engine.Write(write);
    ASSERT_EQ(out.stored_ids.size(), 1U);

    std::string error;
    ASSERT_TRUE(engine.Pin("ws", out.stored_ids[0], true, &error));

    pgmem::QueryInput query;
    query.workspace_id        = "ws";
    query.query               = "runbook";
    query.filters.pinned_only = true;

    const auto query_out = engine.Query(query);
    ASSERT_TRUE(!query_out.hits.empty());
    ASSERT_TRUE(query_out.hits[0].pinned);
}

TEST_CASE(test_memory_engine_workspace_isolation) {
    auto store     = pgmem::store::CreateInMemoryStoreAdapter();
    auto retriever = pgmem::core::CreateHybridRetriever();
    pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), "node-a");

    ASSERT_EQ(engine.Write(BuildSingleWrite("ws-main", "token_main_workspace_only")).stored_ids.size(), 1U);
    ASSERT_EQ(engine.Write(BuildSingleWrite("ws-other", "token_other_workspace_only")).stored_ids.size(), 1U);

    pgmem::QueryInput cross;
    cross.workspace_id = "ws-main";
    cross.query        = "token_other_workspace_only";
    const auto out     = engine.Query(cross);

    bool leaked = false;
    for (const auto& hit : out.hits) {
        if (hit.content.find("token_other_workspace_only") != std::string::npos) {
            leaked = true;
            break;
        }
    }
    ASSERT_TRUE(!leaked);
}

TEST_CASE(test_memory_engine_dedup_key) {
    auto store     = pgmem::store::CreateInMemoryStoreAdapter();
    auto retriever = pgmem::core::CreateHybridRetriever();
    pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), "node-a");

    const auto first = engine.Write(BuildSingleWrite("ws", "first payload", "incident-1"));
    ASSERT_EQ(first.stored_ids.size(), 1U);

    const auto second = engine.Write(BuildSingleWrite("ws", "second payload", "incident-1"));
    ASSERT_TRUE(second.ok);
    ASSERT_TRUE(second.stored_ids.empty());
    ASSERT_EQ(second.deduped_ids.size(), 1U);
    ASSERT_EQ(second.deduped_ids[0], first.stored_ids[0]);
}

TEST_CASE(test_memory_engine_dedup_append_mode_keeps_new_write) {
    auto store     = pgmem::store::CreateInMemoryStoreAdapter();
    auto retriever = pgmem::core::CreateHybridRetriever();
    pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), "node-a");

    const auto first = engine.Write(BuildSingleWrite("ws", "first append payload", "append-key-1"));
    ASSERT_EQ(first.stored_ids.size(), 1U);

    auto second_in       = BuildSingleWrite("ws", "second append payload", "append-key-1");
    second_in.write_mode = "append";
    const auto second    = engine.Write(second_in);
    ASSERT_TRUE(second.ok);
    ASSERT_EQ(second.stored_ids.size(), 1U);
    ASSERT_TRUE(second.deduped_ids.empty());
}

TEST_CASE(test_memory_engine_compact_and_stats) {
    auto store     = pgmem::store::CreateInMemoryStoreAdapter();
    auto retriever = pgmem::core::CreateHybridRetriever();

    pgmem::core::MemoryEngineOptions options;
    options.enable_tombstone_gc    = true;
    options.tombstone_retention_ms = 0;
    options.gc_batch_size          = 64;
    options.effective_backend      = "inmemory";

    pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), "node-a", options);

    ASSERT_EQ(engine.Write(BuildSingleWrite("ws", std::string(2048, 'x'))).stored_ids.size(), 1U);
    ASSERT_EQ(engine.Write(BuildSingleWrite("ws", std::string(2048, 'y'))).stored_ids.size(), 1U);

    const auto compact = engine.Compact("ws");
    ASSERT_TRUE(compact.triggered);

    const auto stats = engine.Stats("ws", "5m");
    ASSERT_EQ(stats.write_ack_mode, "durable");
    ASSERT_EQ(stats.effective_backend, "inmemory");
    ASSERT_TRUE(stats.item_count >= 2);
    ASSERT_TRUE(stats.tombstone_count >= 1);
}

TEST_CASE(test_memory_engine_compact_two_phase_reclaims_storage) {
    auto store     = pgmem::store::CreateInMemoryStoreAdapter();
    auto retriever = pgmem::core::CreateHybridRetriever();

    pgmem::core::MemoryEngineOptions options;
    options.enable_tombstone_gc    = true;
    options.tombstone_retention_ms = 0;
    options.gc_batch_size          = 64;
    options.max_pinned_ratio       = 0.5;

    pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), "node-a", options);

    auto unpinned = BuildSingleWrite("ws", "compact_delete_token");
    ASSERT_EQ(engine.Write(unpinned).stored_ids.size(), 1U);

    pgmem::WriteInput pinned_in;
    pinned_in.workspace_id = "ws";
    pinned_in.session_id   = "s1";
    pgmem::WriteRecordInput rec;
    rec.source  = "turn";
    rec.content = "pinned_survivor_token";
    rec.pin     = true;
    pinned_in.records.push_back(rec);
    ASSERT_EQ(engine.Write(pinned_in).stored_ids.size(), 1U);

    const auto first = engine.Compact("ws");
    ASSERT_TRUE(first.triggered);
    ASSERT_TRUE(first.tombstoned_count >= 1);

    const auto second = engine.Compact("ws");
    ASSERT_TRUE(second.triggered);
    ASSERT_TRUE(second.deleted_count >= 1);
    ASSERT_TRUE(second.postings_reclaimed >= 1);
    ASSERT_TRUE(second.vectors_reclaimed >= 1);
    ASSERT_TRUE(second.capacity_blocked);
}

TEST_CASE(test_memory_engine_resident_limit_evicts_entries) {
    auto store     = pgmem::store::CreateInMemoryStoreAdapter();
    auto retriever = pgmem::core::CreateHybridRetriever();

    pgmem::core::MemoryEngineOptions options;
    options.mem_budget_mb    = 1;
    options.max_record_bytes = 262144;

    pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), "node-a", options);

    for (int i = 0; i < 10; ++i) {
        auto in = BuildSingleWrite("ws", "resident_limit_token_" + std::to_string(i) + " " + std::string(120000, 'x'));
        in.records[0].record_id = "mem-" + std::to_string(i);
        const auto out          = engine.Write(in);
        ASSERT_TRUE(out.ok);
        ASSERT_EQ(out.stored_ids.size(), 1U);
    }

    const auto stats = engine.Stats("ws", "5m");
    ASSERT_TRUE(stats.resident_evicted_count > 0);
    ASSERT_TRUE(stats.resident_used_bytes <= stats.resident_limit_bytes);
    ASSERT_TRUE(stats.item_count < 10);
}

TEST_CASE(test_memory_engine_write_validation_and_persist_batch_failure) {
    auto shared_store = MakeSharedInMemoryStore();
    auto store        = std::make_unique<HookedStoreAdapter>(shared_store);
    store->SetBatchFailure("batch write failed for test");
    auto retriever = pgmem::core::CreateHybridRetriever();

    pgmem::core::MemoryEngineOptions options;
    options.max_record_bytes = 2048;
    pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), "node-a", options);

    pgmem::WriteInput missing_workspace = BuildSingleWrite("", "missing workspace");
    const auto missing_workspace_out    = engine.Write(missing_workspace);
    ASSERT_TRUE(!missing_workspace_out.ok);
    ASSERT_TRUE(!missing_workspace_out.warnings.empty());

    pgmem::WriteInput empty_records;
    empty_records.workspace_id   = "ws";
    const auto empty_records_out = engine.Write(empty_records);
    ASSERT_TRUE(empty_records_out.ok);
    ASSERT_TRUE(!empty_records_out.warnings.empty());

    const auto oversized_out = engine.Write(BuildSingleWrite("ws", std::string(4096, 'z')));
    ASSERT_TRUE(oversized_out.ok);
    ASSERT_TRUE(oversized_out.stored_ids.empty());
    ASSERT_TRUE(!oversized_out.warnings.empty());

    auto failing                 = BuildSingleWrite("ws", "normal sized");
    failing.records[0].record_id = "batch-fail";
    const auto failing_out       = engine.Write(failing);
    ASSERT_TRUE(!failing_out.ok);
    ASSERT_TRUE(!failing_out.warnings.empty());
}

TEST_CASE(test_memory_engine_pin_governance_and_not_found) {
    auto store     = pgmem::store::CreateInMemoryStoreAdapter();
    auto retriever = pgmem::core::CreateHybridRetriever();

    pgmem::core::MemoryEngineOptions options;
    options.pin_quota_per_workspace = 0;
    options.max_pinned_ratio        = 0.0;

    pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), "node-a", options);

    pgmem::WriteInput in;
    in.workspace_id = "ws";
    in.session_id   = "s1";
    pgmem::WriteRecordInput rec;
    rec.source  = "turn";
    rec.content = "pin_governance_token";
    rec.pin     = true;
    in.records.push_back(rec);
    const auto write_out = engine.Write(in);
    ASSERT_TRUE(write_out.ok);
    ASSERT_EQ(write_out.stored_ids.size(), 1U);
    ASSERT_TRUE(!write_out.warnings.empty());

    pgmem::QueryInput pinned_only;
    pinned_only.workspace_id        = "ws";
    pinned_only.query               = "pin_governance_token";
    pinned_only.filters.pinned_only = true;
    const auto pinned_query         = engine.Query(pinned_only);
    ASSERT_TRUE(pinned_query.hits.empty());

    std::string error;
    ASSERT_TRUE(!engine.Pin("ws", write_out.stored_ids[0], true, &error));
    ASSERT_TRUE(error.find("pin quota exceeded") != std::string::npos);

    ASSERT_TRUE(!engine.Pin("ws", "missing-id", true, &error));
    ASSERT_TRUE(error.find("record not found") != std::string::npos);
}

TEST_CASE(test_memory_engine_warmup_replay_and_load_from_store_paths) {
    auto shared_store = MakeSharedInMemoryStore();

    {
        auto store     = std::make_unique<HookedStoreAdapter>(shared_store);
        auto retriever = pgmem::core::CreateHybridRetriever();
        pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), "node-a");

        auto seed                 = BuildSingleWrite("ws", "seed_payload_old");
        seed.records[0].record_id = "mem-replay";
        const auto out            = engine.Write(seed);
        ASSERT_TRUE(out.ok);
        ASSERT_EQ(out.stored_ids.size(), 1U);
    }

    pgmem::util::Json fallback_doc;
    fallback_doc.put("session_id", "s1");
    fallback_doc.put("source", "turn");
    fallback_doc.put("content", "warmup_key_fallback_token");
    fallback_doc.put("updated_at_ms", 1);
    fallback_doc.put("version", 1);
    ASSERT_TRUE(
        shared_store->Put("mem_docs", "ws/ws/doc/mem-fallback", pgmem::util::ToJsonString(fallback_doc, false), 1).ok);

    pgmem::util::Json replay_payload;
    replay_payload.put("id", "mem-replay");
    replay_payload.put("workspace_id", "ws");
    replay_payload.put("session_id", "s1");
    replay_payload.put("source", "turn");
    replay_payload.put("content", "replayed_payload_token");
    replay_payload.put("updated_at_ms", 32503680000000ull);
    replay_payload.put("version", 999);

    pgmem::util::Json event;
    event.put("memory_id", "mem-replay");
    event.put("record_payload", pgmem::util::ToJsonString(replay_payload, false));
    event.put("updated_at_ms", 32503680000000ull);
    ASSERT_TRUE(shared_store
                    ->Put("ds_events", "ws/ws/ts/99999999999999999999/seq/99999999999999999999",
                          pgmem::util::ToJsonString(event, false), 32503680000000ull)
                    .ok);

    {
        auto store     = std::make_unique<HookedStoreAdapter>(shared_store);
        auto retriever = pgmem::core::CreateHybridRetriever();
        pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), "node-b");

        std::string warmup_error;
        ASSERT_TRUE(engine.Warmup(&warmup_error));

        pgmem::QueryInput replay_query;
        replay_query.workspace_id = "ws";
        replay_query.query        = "replayed_payload_token";
        replay_query.top_k        = 5;
        const auto replay_out     = engine.Query(replay_query);
        ASSERT_TRUE(HasHitContent(replay_out, "replayed_payload_token"));

        pgmem::QueryInput fallback_query;
        fallback_query.workspace_id = "ws";
        fallback_query.query        = "warmup_key_fallback_token";
        fallback_query.top_k        = 5;
        const auto fallback_out     = engine.Query(fallback_query);
        ASSERT_TRUE(HasHitContent(fallback_out, "warmup_key_fallback_token"));
    }

    {
        auto store     = std::make_unique<HookedStoreAdapter>(shared_store);
        auto retriever = pgmem::core::CreateHybridRetriever();
        pgmem::core::MemoryEngineOptions options;
        options.max_pinned_ratio = 1.0;
        pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), "node-c", options);

        std::string pin_error;
        if (!engine.Pin("ws", "mem-replay", true, &pin_error)) {
            throw std::runtime_error("pin from store failed: " + pin_error);
        }
    }
}

TEST_CASE(test_memory_engine_warmup_scan_error_policy) {
    auto shared_store = MakeSharedInMemoryStore();

    {
        auto store = std::make_unique<HookedStoreAdapter>(shared_store);
        store->SetScanErrorOnce("mem_docs", "resource not found for namespace mem_docs");
        auto retriever = pgmem::core::CreateHybridRetriever();
        pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), "node-a");
        std::string warmup_error;
        ASSERT_TRUE(engine.Warmup(&warmup_error));
    }

    {
        auto store = std::make_unique<HookedStoreAdapter>(shared_store);
        store->SetScanErrorOnce("mem_docs", "permission denied");
        auto retriever = pgmem::core::CreateHybridRetriever();
        pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), "node-b");
        std::string warmup_error;
        ASSERT_TRUE(!engine.Warmup(&warmup_error));
        ASSERT_TRUE(warmup_error.find("permission denied") != std::string::npos);
    }
}

TEST_CASE(test_memory_engine_gc_loop_runs_when_usage_high) {
    auto shared_store = MakeSharedInMemoryStore();
    auto store        = std::make_unique<HookedStoreAdapter>(shared_store);
    pgmem::StoreUsage forced_usage;
    forced_usage.mem_used_bytes  = 10;
    forced_usage.disk_used_bytes = 100;
    forced_usage.item_count      = 1;
    store->SetForcedUsage(forced_usage);

    auto retriever = pgmem::core::CreateHybridRetriever();
    pgmem::core::MemoryEngineOptions options;
    options.enable_tombstone_gc = true;
    options.disk_budget_gb      = 1;
    options.gc_high_watermark   = 0.0;
    options.gc_low_watermark    = 0.0;

    pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), "node-a", options);
    const auto out = engine.Write(BuildSingleWrite("ws", "gc_loop_trigger_token"));
    ASSERT_TRUE(out.ok);

    bool gc_ran = false;
    for (int i = 0; i < 50; ++i) {
        const auto stats = engine.Stats("ws", "5m");
        if (stats.gc_last_run_ms > 0) {
            gc_ran = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ASSERT_TRUE(gc_ran);
}

TEST_CASE(test_memory_engine_ttl_expiry_filtered_from_query) {
    auto store     = pgmem::store::CreateInMemoryStoreAdapter();
    auto retriever = pgmem::core::CreateHybridRetriever();
    pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), "node-a");

    pgmem::WriteInput in;
    in.workspace_id = "ws";
    in.session_id   = "s1";
    pgmem::WriteRecordInput rec;
    rec.source  = "turn";
    rec.content = "ttl_token_alpha";
    rec.ttl_s   = 1;
    in.records.push_back(rec);

    const auto write_out = engine.Write(in);
    ASSERT_TRUE(write_out.ok);
    ASSERT_EQ(write_out.stored_ids.size(), 1U);

    pgmem::QueryInput q;
    q.workspace_id = "ws";
    q.query        = "ttl_token_alpha";
    q.top_k        = 5;

    const auto before_expire = engine.Query(q);
    ASSERT_TRUE(!before_expire.hits.empty());

    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    const auto after_expire = engine.Query(q);

    bool found = false;
    for (const auto& hit : after_expire.hits) {
        if (hit.memory_id == write_out.stored_ids[0]) {
            found = true;
            break;
        }
    }
    ASSERT_TRUE(!found);
}
