#include <cmath>
#include <memory>
#include <string>

#include "pgmem/core/embedding_provider.h"
#include "pgmem/core/retriever.h"
#include "pgmem/core/vector_index.h"
#include "test_framework.h"

namespace {

pgmem::MemoryRecord BuildRecord(const std::string& id, const std::string& workspace, const std::string& content,
                                bool pinned = false) {
    pgmem::MemoryRecord rec;
    rec.id            = id;
    rec.workspace_id  = workspace;
    rec.session_id    = "s1";
    rec.source        = "turn";
    rec.content       = content;
    rec.pinned        = pinned;
    rec.updated_at_ms = 1000;
    rec.created_at_ms = 1000;
    rec.version       = 1;
    return rec;
}

}  // namespace

TEST_CASE(test_hash_embedding_provider_is_unit_norm) {
    auto provider  = pgmem::core::CreateHashEmbeddingProvider();
    const auto vec = provider->Embed("retry retry backoff", 32);
    double norm_sq = 0.0;
    for (const float v : vec) {
        norm_sq += static_cast<double>(v) * static_cast<double>(v);
    }
    ASSERT_TRUE(!vec.empty());
    ASSERT_TRUE(std::abs(std::sqrt(norm_sq) - 1.0) < 1e-6);
}

TEST_CASE(test_lsh_vector_index_prefers_similar_vector) {
    auto index = pgmem::core::CreateLshVectorIndex();
    index->Upsert("ws", "a", {1.0f, 0.0f, 0.0f, 0.0f});
    index->Upsert("ws", "b", {0.0f, 1.0f, 0.0f, 0.0f});

    pgmem::core::VectorSearchRequest req;
    req.workspace_id = "ws";
    req.query        = {1.0f, 0.0f, 0.0f, 0.0f};
    req.top_k        = 2;

    const auto out = index->Search(req);
    ASSERT_EQ(out.size(), 2U);
    ASSERT_EQ(out[0].memory_id, "a");
}

TEST_CASE(test_hybrid_retriever_returns_sparse_hits) {
    auto retriever = pgmem::core::CreateHybridRetriever();
    retriever->Index(BuildRecord("d1", "ws", "remember retry with exponential backoff"));
    retriever->Index(BuildRecord("d2", "ws", "database migration checklist"));

    pgmem::QueryInput query;
    query.workspace_id = "ws";
    query.query        = "retry backoff";
    query.top_k        = 2;

    const auto out = retriever->Query(query);
    ASSERT_TRUE(!out.hits.empty());
    ASSERT_EQ(out.hits[0].memory_id, "d1");
}

TEST_CASE(test_hybrid_retriever_pin_weight_affects_rank) {
    auto retriever = pgmem::core::CreateHybridRetriever();
    retriever->Index(BuildRecord("p1", "ws", "shared semantic payload", true));
    retriever->Index(BuildRecord("p2", "ws", "shared semantic payload", false));

    pgmem::QueryInput query;
    query.workspace_id       = "ws";
    query.query              = "shared semantic";
    query.top_k              = 2;
    query.rerank.w_sparse    = 0.0;
    query.rerank.w_dense     = 0.0;
    query.rerank.w_freshness = 0.0;
    query.rerank.w_pin       = 1.0;

    const auto out = retriever->Query(query);
    ASSERT_EQ(out.hits.size(), 2U);
    ASSERT_EQ(out.hits[0].memory_id, "p1");
    ASSERT_TRUE(out.hits[0].pinned);
}
