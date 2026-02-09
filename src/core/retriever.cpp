#include "pgmem/core/retriever.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>

#include "pgmem/util/text.h"

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

}  // namespace

HybridRetriever::HybridRetriever(size_t embedding_dim) : embedding_dim_(embedding_dim) {}

void HybridRetriever::Index(const MemoryRecord& record) {
    IndexedDoc doc;
    doc.record = record;
    const auto tokens = util::Tokenize(record.content);
    for (const std::string& token : tokens) {
        ++doc.tf[token];
    }
    doc.doc_len = tokens.size();
    doc.embedding = Embed(record.content);

    std::unique_lock<std::shared_mutex> lock(mu_);
    const auto old_it = snapshots_.find(record.workspace_id);
    std::shared_ptr<const WorkspaceSnapshot> old_snapshot;
    if (old_it != snapshots_.end()) {
        old_snapshot = old_it->second;
    }

    auto next = std::make_shared<WorkspaceSnapshot>();
    if (old_snapshot) {
        next->docs = old_snapshot->docs;
        next->total_doc_len = old_snapshot->total_doc_len;
    }

    const auto prev = next->docs.find(record.id);
    if (prev != next->docs.end()) {
        if (next->total_doc_len >= prev->second.doc_len) {
            next->total_doc_len -= prev->second.doc_len;
        }
    }
    next->docs[record.id] = std::move(doc);
    next->total_doc_len += tokens.size();

    next->df = BuildDocumentFrequency(next->docs);
    snapshots_[record.workspace_id] = std::move(next);
}

void HybridRetriever::Remove(const std::string& workspace_id, const std::string& memory_id) {
    std::unique_lock<std::shared_mutex> lock(mu_);
    auto old_it = snapshots_.find(workspace_id);
    if (old_it == snapshots_.end() || !old_it->second) {
        return;
    }

    auto next = std::make_shared<WorkspaceSnapshot>(*old_it->second);
    const auto doc_it = next->docs.find(memory_id);
    if (doc_it == next->docs.end()) {
        return;
    }

    if (next->total_doc_len >= doc_it->second.doc_len) {
        next->total_doc_len -= doc_it->second.doc_len;
    }
    next->docs.erase(doc_it);

    if (next->docs.empty()) {
        snapshots_.erase(old_it);
        return;
    }

    next->df = BuildDocumentFrequency(next->docs);
    snapshots_[workspace_id] = std::move(next);
}

std::vector<SearchHit> HybridRetriever::Search(const SearchInput& input,
                                               bool* semantic_fallback_used) {
    if (semantic_fallback_used != nullptr) {
        *semantic_fallback_used = false;
    }

    std::shared_ptr<const WorkspaceSnapshot> snapshot;
    {
        std::shared_lock<std::shared_mutex> lock(mu_);
        auto ws_it = snapshots_.find(input.workspace_id);
        if (ws_it == snapshots_.end()) {
            return {};
        }
        snapshot = ws_it->second;
    }
    if (!snapshot || snapshot->docs.empty()) {
        return {};
    }

    const auto query_tokens = util::Tokenize(input.query);
    std::unordered_map<std::string, int> query_tf;
    for (const std::string& token : query_tokens) {
        ++query_tf[token];
    }

    const bool semantic_enabled = input.query.size() <= 8192;
    if (!semantic_enabled && semantic_fallback_used != nullptr) {
        *semantic_fallback_used = true;
    }

    std::vector<float> query_embedding;
    if (semantic_enabled) {
        query_embedding = Embed(input.query);
    }

    const size_t total_docs = snapshot->docs.size();
    const double avgdl = total_docs > 0
        ? static_cast<double>(snapshot->total_doc_len) / static_cast<double>(total_docs)
        : 1.0;

    std::vector<SearchHit> hits;
    hits.reserve(total_docs);

    for (const auto& kv : snapshot->docs) {
        const IndexedDoc& doc = kv.second;
        SearchHit hit;
        hit.memory_id = doc.record.id;
        hit.source = doc.record.source;
        hit.content = doc.record.content;
        hit.pinned = doc.record.pinned;
        hit.updated_at_ms = doc.record.updated_at_ms;

        hit.lexical_score = LexicalScore(doc, query_tf, total_docs, avgdl, snapshot->df);
        if (semantic_enabled) {
            hit.semantic_score = Cosine(query_embedding, doc.embedding);
        }

        hit.final_score = 0.65 * hit.lexical_score + 0.35 * hit.semantic_score;
        if (hit.pinned) {
            hit.final_score += 0.2;
        }
        hits.push_back(std::move(hit));
    }

    std::sort(hits.begin(), hits.end(), [](const SearchHit& lhs, const SearchHit& rhs) {
        if (lhs.final_score == rhs.final_score) {
            return lhs.updated_at_ms > rhs.updated_at_ms;
        }
        return lhs.final_score > rhs.final_score;
    });

    if (input.top_k > 0 && hits.size() > input.top_k) {
        hits.resize(input.top_k);
    }
    return hits;
}

std::vector<float> HybridRetriever::Embed(const std::string& text) const {
    std::vector<float> emb(embedding_dim_, 0.0f);
    const auto tokens = util::Tokenize(text);
    if (tokens.empty()) {
        return emb;
    }

    for (const std::string& token : tokens) {
        const size_t index = std::hash<std::string>{}(token) % embedding_dim_;
        emb[index] += 1.0f;
    }

    float norm = 0.0f;
    for (float v : emb) {
        norm += v * v;
    }
    norm = std::sqrt(norm);
    if (norm > std::numeric_limits<float>::epsilon()) {
        for (float& v : emb) {
            v /= norm;
        }
    }
    return emb;
}

double HybridRetriever::Cosine(const std::vector<float>& a, const std::vector<float>& b) const {
    if (a.size() != b.size() || a.empty()) {
        return 0.0;
    }
    double dot = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += static_cast<double>(a[i]) * static_cast<double>(b[i]);
    }
    return dot;
}

double HybridRetriever::LexicalScore(const IndexedDoc& doc,
                                     const std::unordered_map<std::string, int>& query_tf,
                                     size_t total_docs,
                                     double avg_doc_len,
                                     const std::unordered_map<std::string, size_t>& df) const {
    if (doc.doc_len == 0 || query_tf.empty() || total_docs == 0) {
        return 0.0;
    }

    constexpr double k1 = 1.5;
    constexpr double b = 0.75;

    const double avgdl = avg_doc_len > 0.0 ? avg_doc_len : 1.0;

    double score = 0.0;
    for (const auto& q : query_tf) {
        const auto tf_it = doc.tf.find(q.first);
        if (tf_it == doc.tf.end()) {
            continue;
        }

        const double tf = static_cast<double>(tf_it->second);
        const auto df_it = df.find(q.first);
        const double doc_freq = (df_it == df.end()) ? 0.0 : static_cast<double>(df_it->second);

        const double idf = std::log((static_cast<double>(total_docs) - doc_freq + 0.5) / (doc_freq + 0.5) + 1.0);
        const double denom = tf + k1 * (1.0 - b + b * (static_cast<double>(doc.doc_len) / avgdl));
        score += idf * (tf * (k1 + 1.0) / denom);
    }
    return score;
}

std::unique_ptr<IRetriever> CreateHybridRetriever() {
    return std::make_unique<HybridRetriever>();
}

}  // namespace pgmem::core
