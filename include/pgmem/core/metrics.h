#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "pgmem/types.h"

namespace pgmem::core {

class Metrics {
public:
    Metrics();

    void RecordReadLatency(double ms);
    void RecordWriteLatency(double ms);
    void RecordTokenReduction(size_t before_tokens, size_t after_tokens);
    void RecordFallback(bool used_fallback);

    StatsSnapshot Snapshot() const;

private:
    static constexpr size_t kLatencyBucketCount = 16;
    size_t BucketIndex(double ms) const;
    double ComputeP95(const std::array<std::atomic<uint64_t>, kLatencyBucketCount + 1>& hist) const;

    std::array<std::atomic<uint64_t>, kLatencyBucketCount + 1> read_lat_hist_;
    std::array<std::atomic<uint64_t>, kLatencyBucketCount + 1> write_lat_hist_;

    std::atomic<uint64_t> token_before_{0};
    std::atomic<uint64_t> token_after_{0};

    std::atomic<uint64_t> fallback_total_{0};
    std::atomic<uint64_t> fallback_used_{0};
};

}  // namespace pgmem::core
