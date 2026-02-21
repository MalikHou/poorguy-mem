#include <algorithm>
#include <cmath>
#include <mutex>

#include "pgmem/core/vector_index.h"

namespace pgmem::core {

LshVectorIndex::LshVectorIndex() = default;

void LshVectorIndex::Upsert(const std::string& workspace_id, const std::string& memory_id,
                            const std::vector<float>& vector) {
    std::unique_lock<std::shared_mutex> lock(mu_);
    workspace_vectors_[workspace_id][memory_id] = Entry{vector, BucketForVector(vector)};
}

void LshVectorIndex::Remove(const std::string& workspace_id, const std::string& memory_id) {
    std::unique_lock<std::shared_mutex> lock(mu_);
    auto ws_it = workspace_vectors_.find(workspace_id);
    if (ws_it == workspace_vectors_.end()) {
        return;
    }
    ws_it->second.erase(memory_id);
    if (ws_it->second.empty()) {
        workspace_vectors_.erase(ws_it);
    }
}

std::vector<VectorSearchResult> LshVectorIndex::Search(const VectorSearchRequest& input) const {
    std::vector<VectorSearchResult> out;

    std::shared_lock<std::shared_mutex> lock(mu_);
    const auto ws_it = workspace_vectors_.find(input.workspace_id);
    if (ws_it == workspace_vectors_.end() || input.query.empty()) {
        return out;
    }

    const uint64_t query_bucket = BucketForVector(input.query);
    out.reserve(ws_it->second.size());

    for (const auto& kv : ws_it->second) {
        const Entry& entry = kv.second;
        double score       = Cosine(input.query, entry.vector);

        // Prefer vectors that fall in the same rough LSH bucket.
        if (entry.bucket == query_bucket) {
            score += 0.05;
        }

        out.push_back(VectorSearchResult{kv.first, score});
    }

    std::sort(out.begin(), out.end(), [](const VectorSearchResult& lhs, const VectorSearchResult& rhs) {
        if (lhs.score == rhs.score) {
            return lhs.memory_id < rhs.memory_id;
        }
        return lhs.score > rhs.score;
    });

    if (input.top_k > 0 && out.size() > input.top_k) {
        out.resize(input.top_k);
    }
    return out;
}

uint64_t LshVectorIndex::EstimatedBytes(const std::string& workspace_id) const {
    std::shared_lock<std::shared_mutex> lock(mu_);

    uint64_t total = 0;
    for (const auto& ws_kv : workspace_vectors_) {
        if (!workspace_id.empty() && ws_kv.first != workspace_id) {
            continue;
        }
        for (const auto& doc_kv : ws_kv.second) {
            total += static_cast<uint64_t>(doc_kv.first.size());
            total += static_cast<uint64_t>(doc_kv.second.vector.size()) * sizeof(float);
            total += sizeof(uint64_t) + 64;
        }
    }
    return total;
}

uint64_t LshVectorIndex::BucketForVector(const std::vector<float>& vector) const {
    uint64_t bucket       = 0;
    const size_t max_bits = std::min<size_t>(64, vector.size());
    for (size_t i = 0; i < max_bits; ++i) {
        if (vector[i] > 0.0f) {
            bucket |= (1ull << i);
        }
    }
    return bucket;
}

double LshVectorIndex::Cosine(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) {
        return 0.0;
    }
    double dot = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += static_cast<double>(a[i]) * static_cast<double>(b[i]);
    }
    return dot;
}

std::unique_ptr<IVectorIndex> CreateLshVectorIndex() { return std::make_unique<LshVectorIndex>(); }

}  // namespace pgmem::core
