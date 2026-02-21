#include <cmath>
#include <cstddef>
#include <limits>

#include "pgmem/core/embedding_provider.h"
#include "pgmem/util/text.h"

namespace pgmem::core {

std::vector<float> HashEmbeddingProvider::Embed(const std::string& text, size_t dim) const {
    if (dim == 0) {
        return {};
    }

    std::vector<float> emb(dim, 0.0f);
    const auto tokens = util::Tokenize(text);
    if (tokens.empty()) {
        return emb;
    }

    for (const auto& token : tokens) {
        const size_t index = std::hash<std::string>{}(token) % dim;
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

std::string HashEmbeddingProvider::ModelId() const { return "hash-embed-v1"; }

std::unique_ptr<IEmbeddingProvider> CreateHashEmbeddingProvider() { return std::make_unique<HashEmbeddingProvider>(); }

}  // namespace pgmem::core
