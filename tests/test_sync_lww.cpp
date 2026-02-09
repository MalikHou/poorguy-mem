#include "test_framework.h"

#include <memory>

#include "pgmem/core/memory_engine.h"
#include "pgmem/core/retriever.h"
#include "pgmem/store/store_adapter.h"

TEST_CASE(test_sync_lww_node_id_tiebreak) {
    auto store = pgmem::store::CreateInMemoryStoreAdapter();
    auto retriever = pgmem::core::CreateHybridRetriever();
    pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), nullptr, "local");

    pgmem::SyncOp op_a;
    op_a.op_seq = 1;
    op_a.op_type = pgmem::OpType::Upsert;
    op_a.workspace_id = "ws";
    op_a.memory_id = "m1";
    op_a.updated_at_ms = 1000;
    op_a.node_id = "node-a";
    op_a.payload_json =
        "{\"id\":\"m1\",\"workspace_id\":\"ws\",\"session_id\":\"s1\","
        "\"source\":\"sync\",\"content\":\"old content\",\"pinned\":false,"
        "\"created_at_ms\":1000,\"updated_at_ms\":1000,\"version\":1,\"node_id\":\"node-a\","
        "\"tags\":[\"sync\"]}";

    std::string error;
    ASSERT_TRUE(engine.ApplySyncOp(op_a, &error));

    pgmem::SyncOp op_b = op_a;
    op_b.node_id = "node-z";
    op_b.payload_json =
        "{\"id\":\"m1\",\"workspace_id\":\"ws\",\"session_id\":\"s1\","
        "\"source\":\"sync\",\"content\":\"new content\",\"pinned\":false,"
        "\"created_at_ms\":1000,\"updated_at_ms\":1000,\"version\":1,\"node_id\":\"node-z\","
        "\"tags\":[\"sync\"]}";

    ASSERT_TRUE(engine.ApplySyncOp(op_b, &error));

    pgmem::SearchInput search;
    search.workspace_id = "ws";
    search.query = "new content";
    search.top_k = 1;
    search.token_budget = 1024;

    const auto out = engine.Search(search);
    ASSERT_TRUE(!out.hits.empty());
    ASSERT_EQ(out.hits[0].memory_id, "m1");
    ASSERT_TRUE(out.hits[0].content.find("new content") != std::string::npos);
}
