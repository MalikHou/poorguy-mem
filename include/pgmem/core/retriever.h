#pragma once

#include <cstddef>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "pgmem/core/analyzer.h"
#include "pgmem/core/embedding_provider.h"
#include "pgmem/core/vector_index.h"
#include "pgmem/types.h"

namespace pgmem::core {

class IRetriever {
public:
    virtual ~IRetriever() = default;

    virtual void Index(const MemoryRecord& record)                                     = 0;
    virtual void Remove(const std::string& workspace_id, const std::string& memory_id) = 0;
    virtual QueryOutput Query(const QueryInput& input)                                 = 0;
    virtual uint64_t EstimatedBytes(const std::string& workspace_id = {}) const        = 0;
    virtual uint64_t EstimateDocBytes(const MemoryRecord& record) const                = 0;
};

class HybridRetriever final : public IRetriever {
public:
    explicit HybridRetriever(size_t embedding_dim = 64, std::unique_ptr<IAnalyzer> analyzer = CreateDefaultAnalyzer(),
                             std::unique_ptr<IEmbeddingProvider> embedding = CreateHashEmbeddingProvider(),
                             std::unique_ptr<IVectorIndex> vector_index    = CreateLshVectorIndex());

    void Index(const MemoryRecord& record) override;
    void Remove(const std::string& workspace_id, const std::string& memory_id) override;
    QueryOutput Query(const QueryInput& input) override;
    uint64_t EstimatedBytes(const std::string& workspace_id = {}) const override;
    uint64_t EstimateDocBytes(const MemoryRecord& record) const override;

private:
    struct IndexedDoc {
        MemoryRecord record;
        std::unordered_map<std::string, int> tf;
        size_t doc_len{0};
        std::vector<float> embedding;
    };

    struct WorkspaceSnapshot {
        std::unordered_map<std::string, IndexedDoc> docs;
        std::unordered_map<std::string, size_t> df;
        size_t total_doc_len{0};
    };

    static bool PassesFilter(const MemoryRecord& record, const QueryFilter& filters);
    static bool ContainsAnyTag(const std::vector<std::string>& haystack, const std::vector<std::string>& needle);
    static bool ContainsAnySource(const std::string& source, const std::vector<std::string>& allow);
    static double FreshnessScore(uint64_t updated_at_ms, uint64_t now_ms);

    double LexicalScore(const IndexedDoc& doc, const std::unordered_map<std::string, int>& query_tf, size_t total_docs,
                        double avg_doc_len, const std::unordered_map<std::string, size_t>& df) const;

    size_t embedding_dim_;
    std::unique_ptr<IAnalyzer> analyzer_;
    std::unique_ptr<IEmbeddingProvider> embedding_;
    std::unique_ptr<IVectorIndex> vector_index_;

    mutable std::shared_mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<const WorkspaceSnapshot>> snapshots_;
};

std::unique_ptr<IRetriever> CreateHybridRetriever();

}  // namespace pgmem::core
