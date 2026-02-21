#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace pgmem::core {

struct VectorSearchRequest {
    std::string workspace_id;
    std::vector<float> query;
    size_t top_k{100};
};

struct VectorSearchResult {
    std::string memory_id;
    double score{0.0};
};

class IVectorIndex {
public:
    virtual ~IVectorIndex() = default;

    virtual void Upsert(const std::string& workspace_id, const std::string& memory_id,
                        const std::vector<float>& vector)                                  = 0;
    virtual void Remove(const std::string& workspace_id, const std::string& memory_id)     = 0;
    virtual std::vector<VectorSearchResult> Search(const VectorSearchRequest& input) const = 0;
    virtual uint64_t EstimatedBytes(const std::string& workspace_id) const                 = 0;
};

class LshVectorIndex final : public IVectorIndex {
public:
    LshVectorIndex();

    void Upsert(const std::string& workspace_id, const std::string& memory_id,
                const std::vector<float>& vector) override;
    void Remove(const std::string& workspace_id, const std::string& memory_id) override;
    std::vector<VectorSearchResult> Search(const VectorSearchRequest& input) const override;
    uint64_t EstimatedBytes(const std::string& workspace_id) const override;

private:
    struct Entry {
        std::vector<float> vector;
        uint64_t bucket{0};
    };

    uint64_t BucketForVector(const std::vector<float>& vector) const;
    static double Cosine(const std::vector<float>& a, const std::vector<float>& b);

    mutable std::shared_mutex mu_;
    std::unordered_map<std::string, std::unordered_map<std::string, Entry>> workspace_vectors_;
};

std::unique_ptr<IVectorIndex> CreateLshVectorIndex();

}  // namespace pgmem::core
