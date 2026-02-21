#include "pgmem/core/retriever.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <unordered_set>

#include "pgmem/util/time.h"

namespace pgmem::core {
namespace {

template <typename DocMap>
std::unordered_map<std::string, size_t> BuildDocumentFrequency(const DocMap& docs) {
    std::unordered_map<std::string, size_t> df;
    for (const auto& kv : docs) {
        for (const auto& tf_entry : kv.second.tf) {
            if (tf_entry.second > 0) {
                ++df[tf_entry.first];
            }
        }
    }
    return df;
}

uint64_t EstimateRecordBytes(const MemoryRecord& record) {
    uint64_t bytes = 0;
    bytes += static_cast<uint64_t>(record.id.size());
    bytes += static_cast<uint64_t>(record.workspace_id.size());
    bytes += static_cast<uint64_t>(record.session_id.size());
    bytes += static_cast<uint64_t>(record.source.size());
    bytes += static_cast<uint64_t>(record.content.size());
    bytes += static_cast<uint64_t>(record.node_id.size());
    bytes += static_cast<uint64_t>(record.tier.size());
    bytes += static_cast<uint64_t>(record.dedup_key.size());
    bytes += static_cast<uint64_t>(record.shard_id.size());
    bytes += static_cast<uint64_t>(record.replica_role.size());
    bytes += static_cast<uint64_t>(record.shard_hint.size());
    for (const auto& tag : record.tags) {
        bytes += static_cast<uint64_t>(tag.size());
    }
    for (const auto& kv : record.metadata) {
        bytes += static_cast<uint64_t>(kv.first.size() + kv.second.size());
    }
    bytes += 320;
    return bytes;
}

double Normalize(double value, double max_value) {
    if (value <= 0.0 || max_value <= 0.0) {
        return 0.0;
    }
    return value / max_value;
}

}  // namespace

HybridRetriever::HybridRetriever(size_t embedding_dim, std::unique_ptr<IAnalyzer> analyzer,
                                 std::unique_ptr<IEmbeddingProvider> embedding,
                                 std::unique_ptr<IVectorIndex> vector_index)
    : embedding_dim_(embedding_dim),
      analyzer_(std::move(analyzer)),
      embedding_(std::move(embedding)),
      vector_index_(std::move(vector_index)) {}

void HybridRetriever::Index(const MemoryRecord& record) {
    if (record.tombstone) {
        Remove(record.workspace_id, record.id);
        return;
    }

    IndexedDoc doc;
    doc.record        = record;
    const auto tokens = analyzer_->Tokenize(record.content);
    for (const auto& token : tokens) {
        ++doc.tf[token];
    }
    doc.doc_len   = tokens.size();
    doc.embedding = embedding_->Embed(record.content, embedding_dim_);

    {
        std::unique_lock<std::shared_mutex> lock(mu_);

        const auto old_it = snapshots_.find(record.workspace_id);
        std::shared_ptr<const WorkspaceSnapshot> old_snapshot;
        if (old_it != snapshots_.end()) {
            old_snapshot = old_it->second;
        }

        auto next = std::make_shared<WorkspaceSnapshot>();
        if (old_snapshot) {
            next->docs          = old_snapshot->docs;
            next->total_doc_len = old_snapshot->total_doc_len;
        }

        const auto prev = next->docs.find(record.id);
        if (prev != next->docs.end() && next->total_doc_len >= prev->second.doc_len) {
            next->total_doc_len -= prev->second.doc_len;
        }

        next->docs[record.id] = doc;
        next->total_doc_len += tokens.size();
        next->df                        = BuildDocumentFrequency(next->docs);
        snapshots_[record.workspace_id] = std::move(next);
    }

    vector_index_->Upsert(record.workspace_id, record.id, doc.embedding);
}

void HybridRetriever::Remove(const std::string& workspace_id, const std::string& memory_id) {
    {
        std::unique_lock<std::shared_mutex> lock(mu_);

        auto old_it = snapshots_.find(workspace_id);
        if (old_it == snapshots_.end() || !old_it->second) {
            vector_index_->Remove(workspace_id, memory_id);
            return;
        }

        auto next         = std::make_shared<WorkspaceSnapshot>(*old_it->second);
        const auto doc_it = next->docs.find(memory_id);
        if (doc_it == next->docs.end()) {
            vector_index_->Remove(workspace_id, memory_id);
            return;
        }

        if (next->total_doc_len >= doc_it->second.doc_len) {
            next->total_doc_len -= doc_it->second.doc_len;
        }
        next->docs.erase(doc_it);

        if (next->docs.empty()) {
            snapshots_.erase(old_it);
        } else {
            next->df                 = BuildDocumentFrequency(next->docs);
            snapshots_[workspace_id] = std::move(next);
        }
    }

    vector_index_->Remove(workspace_id, memory_id);
}

QueryOutput HybridRetriever::Query(const QueryInput& input) {
    QueryOutput out;

    std::shared_ptr<const WorkspaceSnapshot> snapshot;
    {
        std::shared_lock<std::shared_mutex> lock(mu_);
        auto ws_it = snapshots_.find(input.workspace_id);
        if (ws_it == snapshots_.end() || !ws_it->second || ws_it->second->docs.empty()) {
            return out;
        }
        snapshot = ws_it->second;
    }

    const auto all_start = std::chrono::steady_clock::now();

    const auto query_tokens = analyzer_->Tokenize(input.query);
    std::unordered_map<std::string, int> query_tf;
    for (const auto& token : query_tokens) {
        ++query_tf[token];
    }

    const size_t total_docs = snapshot->docs.size();
    const double avgdl =
        total_docs > 0 ? static_cast<double>(snapshot->total_doc_len) / static_cast<double>(total_docs) : 1.0;

    const size_t top_k = (input.top_k == 0) ? 8 : input.top_k;
    const size_t sparse_limit =
        std::max<size_t>(input.recall.sparse_k, top_k * std::max<size_t>(1, input.recall.oversample));
    const size_t dense_limit =
        std::max<size_t>(input.recall.dense_k, top_k * std::max<size_t>(1, input.recall.oversample));

    std::vector<std::pair<std::string, double>> sparse_candidates;
    sparse_candidates.reserve(total_docs);

    const auto sparse_start = std::chrono::steady_clock::now();
    for (const auto& kv : snapshot->docs) {
        const IndexedDoc& doc = kv.second;
        if (!PassesFilter(doc.record, input.filters)) {
            continue;
        }
        const double sparse_score = LexicalScore(doc, query_tf, total_docs, avgdl, snapshot->df);
        if (sparse_score <= 0.0) {
            continue;
        }
        sparse_candidates.emplace_back(kv.first, sparse_score);
    }
    std::sort(sparse_candidates.begin(), sparse_candidates.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.second == rhs.second) {
            return lhs.first < rhs.first;
        }
        return lhs.second > rhs.second;
    });
    if (sparse_candidates.size() > sparse_limit) {
        sparse_candidates.resize(sparse_limit);
    }
    const auto sparse_end = std::chrono::steady_clock::now();

    const auto dense_start     = std::chrono::steady_clock::now();
    const auto query_embedding = embedding_->Embed(input.query, embedding_dim_);
    VectorSearchRequest dense_req;
    dense_req.workspace_id = input.workspace_id;
    dense_req.query        = query_embedding;
    dense_req.top_k        = dense_limit;
    auto dense_raw         = vector_index_->Search(dense_req);

    std::vector<VectorSearchResult> dense_candidates;
    dense_candidates.reserve(dense_raw.size());
    for (const auto& item : dense_raw) {
        const auto it = snapshot->docs.find(item.memory_id);
        if (it == snapshot->docs.end()) {
            continue;
        }
        if (!PassesFilter(it->second.record, input.filters)) {
            continue;
        }
        dense_candidates.push_back(item);
    }
    const auto dense_end = std::chrono::steady_clock::now();

    out.debug_stats.sparse_candidates = sparse_candidates.size();
    out.debug_stats.dense_candidates  = dense_candidates.size();

    std::unordered_map<std::string, ScoreBreakdown> merged;
    merged.reserve(sparse_candidates.size() + dense_candidates.size());
    for (const auto& item : sparse_candidates) {
        merged[item.first].sparse = item.second;
    }
    for (const auto& item : dense_candidates) {
        merged[item.memory_id].dense = item.score;
    }

    out.debug_stats.merged_candidates = merged.size();

    double sparse_max = 0.0;
    double dense_max  = 0.0;
    for (const auto& kv : merged) {
        sparse_max = std::max(sparse_max, kv.second.sparse);
        dense_max  = std::max(dense_max, kv.second.dense);
    }

    const auto rerank_start = std::chrono::steady_clock::now();
    const uint64_t now_ms   = util::NowMs();

    out.hits.reserve(merged.size());
    for (auto& kv : merged) {
        const auto doc_it = snapshot->docs.find(kv.first);
        if (doc_it == snapshot->docs.end()) {
            continue;
        }

        const IndexedDoc& doc = doc_it->second;
        ScoreBreakdown scores;
        scores.sparse    = Normalize(kv.second.sparse, sparse_max);
        scores.dense     = Normalize(kv.second.dense, dense_max);
        scores.freshness = FreshnessScore(doc.record.updated_at_ms, now_ms);
        scores.pin       = doc.record.pinned ? 1.0 : 0.0;
        scores.final     = input.rerank.w_sparse * scores.sparse + input.rerank.w_dense * scores.dense +
                       input.rerank.w_freshness * scores.freshness + input.rerank.w_pin * scores.pin;

        QueryHit hit;
        hit.memory_id     = doc.record.id;
        hit.source        = doc.record.source;
        hit.content       = doc.record.content;
        hit.tags          = doc.record.tags;
        hit.metadata      = doc.record.metadata;
        hit.scores        = scores;
        hit.updated_at_ms = doc.record.updated_at_ms;
        hit.pinned        = doc.record.pinned;
        out.hits.push_back(std::move(hit));
    }

    std::sort(out.hits.begin(), out.hits.end(), [](const QueryHit& lhs, const QueryHit& rhs) {
        if (lhs.scores.final == rhs.scores.final) {
            return lhs.updated_at_ms > rhs.updated_at_ms;
        }
        return lhs.scores.final > rhs.scores.final;
    });

    if (out.hits.size() > top_k) {
        out.hits.resize(top_k);
    }
    const auto rerank_end = std::chrono::steady_clock::now();

    out.debug_stats.sparse_ms =
        std::chrono::duration_cast<std::chrono::microseconds>(sparse_end - sparse_start).count() / 1000.0;
    out.debug_stats.dense_ms =
        std::chrono::duration_cast<std::chrono::microseconds>(dense_end - dense_start).count() / 1000.0;
    out.debug_stats.rerank_ms =
        std::chrono::duration_cast<std::chrono::microseconds>(rerank_end - rerank_start).count() / 1000.0;
    out.debug_stats.total_ms =
        std::chrono::duration_cast<std::chrono::microseconds>(rerank_end - all_start).count() / 1000.0;

    return out;
}

uint64_t HybridRetriever::EstimatedBytes(const std::string& workspace_id) const {
    std::shared_lock<std::shared_mutex> lock(mu_);

    uint64_t total = vector_index_->EstimatedBytes(workspace_id);
    for (const auto& ws_kv : snapshots_) {
        if (!workspace_id.empty() && ws_kv.first != workspace_id) {
            continue;
        }
        if (!ws_kv.second) {
            continue;
        }
        for (const auto& doc_kv : ws_kv.second->docs) {
            total += EstimateDocBytes(doc_kv.second.record);
        }
    }
    return total;
}

uint64_t HybridRetriever::EstimateDocBytes(const MemoryRecord& record) const {
    if (record.tombstone) {
        return 0;
    }

    const auto tokens = analyzer_->Tokenize(record.content);
    std::unordered_set<std::string> unique_terms;
    unique_terms.reserve(tokens.size());
    for (const auto& token : tokens) {
        unique_terms.insert(token);
    }

    uint64_t bytes = EstimateRecordBytes(record);
    bytes += static_cast<uint64_t>(tokens.size()) * 8ull;
    bytes += static_cast<uint64_t>(unique_terms.size()) * 48ull;
    for (const auto& term : unique_terms) {
        bytes += static_cast<uint64_t>(term.size());
    }
    bytes += static_cast<uint64_t>(embedding_dim_) * static_cast<uint64_t>(sizeof(float));
    bytes += 128;
    return bytes;
}

bool HybridRetriever::PassesFilter(const MemoryRecord& record, const QueryFilter& filters) {
    if (!filters.session_id.empty() && record.session_id != filters.session_id) {
        return false;
    }
    if (!filters.sources.empty() && !ContainsAnySource(record.source, filters.sources)) {
        return false;
    }
    if (!filters.tags_any.empty() && !ContainsAnyTag(record.tags, filters.tags_any)) {
        return false;
    }
    if (filters.updated_after_ms > 0 && record.updated_at_ms < filters.updated_after_ms) {
        return false;
    }
    if (filters.updated_before_ms > 0 && record.updated_at_ms > filters.updated_before_ms) {
        return false;
    }
    if (filters.pinned_only && !record.pinned) {
        return false;
    }
    return !record.tombstone;
}

bool HybridRetriever::ContainsAnyTag(const std::vector<std::string>& haystack, const std::vector<std::string>& needle) {
    for (const auto& want : needle) {
        if (std::find(haystack.begin(), haystack.end(), want) != haystack.end()) {
            return true;
        }
    }
    return false;
}

bool HybridRetriever::ContainsAnySource(const std::string& source, const std::vector<std::string>& allow) {
    return std::find(allow.begin(), allow.end(), source) != allow.end();
}

double HybridRetriever::FreshnessScore(uint64_t updated_at_ms, uint64_t now_ms) {
    const double age_hours = static_cast<double>(now_ms > updated_at_ms ? (now_ms - updated_at_ms) : 0) / 3600000.0;
    return 1.0 / (1.0 + age_hours);
}

double HybridRetriever::LexicalScore(const IndexedDoc& doc, const std::unordered_map<std::string, int>& query_tf,
                                     size_t total_docs, double avg_doc_len,
                                     const std::unordered_map<std::string, size_t>& df) const {
    if (doc.doc_len == 0 || query_tf.empty() || total_docs == 0) {
        return 0.0;
    }

    constexpr double k1 = 1.5;
    constexpr double b  = 0.75;

    const double avgdl = avg_doc_len > 0.0 ? avg_doc_len : 1.0;

    double score = 0.0;
    for (const auto& q : query_tf) {
        const auto tf_it = doc.tf.find(q.first);
        if (tf_it == doc.tf.end()) {
            continue;
        }

        const double tf       = static_cast<double>(tf_it->second);
        const auto df_it      = df.find(q.first);
        const double doc_freq = (df_it == df.end()) ? 0.0 : static_cast<double>(df_it->second);

        const double idf   = std::log((static_cast<double>(total_docs) - doc_freq + 0.5) / (doc_freq + 0.5) + 1.0);
        const double denom = tf + k1 * (1.0 - b + b * (static_cast<double>(doc.doc_len) / avgdl));
        score += idf * (tf * (k1 + 1.0) / denom);
    }
    return score;
}

std::unique_ptr<IRetriever> CreateHybridRetriever() { return std::make_unique<HybridRetriever>(); }

}  // namespace pgmem::core
