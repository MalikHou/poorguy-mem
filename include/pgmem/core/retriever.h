#pragma once

#include <cstddef>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "pgmem/types.h"

namespace pgmem::core {

class IRetriever {
public:
    virtual ~IRetriever()                                                                         = default;
    virtual void Index(const MemoryRecord& record)                                                = 0;
    virtual void Remove(const std::string& workspace_id, const std::string& memory_id)            = 0;
    virtual std::vector<SearchHit> Search(const SearchInput& input, bool* semantic_fallback_used) = 0;
};

class HybridRetriever final : public IRetriever {
public:
    explicit HybridRetriever(size_t embedding_dim = 64);

    void Index(const MemoryRecord& record) override;
    void Remove(const std::string& workspace_id, const std::string& memory_id) override;
    std::vector<SearchHit> Search(const SearchInput& input, bool* semantic_fallback_used) override;

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

    std::vector<float> Embed(const std::string& text) const;
    double Cosine(const std::vector<float>& a, const std::vector<float>& b) const;
    double LexicalScore(const IndexedDoc& doc, const std::unordered_map<std::string, int>& query_tf, size_t total_docs,
                        double avg_doc_len, const std::unordered_map<std::string, size_t>& df) const;

    size_t embedding_dim_;
    mutable std::shared_mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<const WorkspaceSnapshot>> snapshots_;
};

std::unique_ptr<IRetriever> CreateHybridRetriever();

}  // namespace pgmem::core
