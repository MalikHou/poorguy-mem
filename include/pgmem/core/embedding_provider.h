#pragma once

#include <memory>
#include <string>
#include <vector>

namespace pgmem::core {

class IEmbeddingProvider {
public:
    virtual ~IEmbeddingProvider()                                               = default;
    virtual std::vector<float> Embed(const std::string& text, size_t dim) const = 0;
    virtual std::string ModelId() const                                         = 0;
};

class HashEmbeddingProvider final : public IEmbeddingProvider {
public:
    std::vector<float> Embed(const std::string& text, size_t dim) const override;
    std::string ModelId() const override;
};

std::unique_ptr<IEmbeddingProvider> CreateHashEmbeddingProvider();

}  // namespace pgmem::core
