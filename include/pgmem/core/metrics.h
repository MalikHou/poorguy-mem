#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "pgmem/types.h"

namespace pgmem::core {

class Metrics {
public:
    void RecordReadLatency(double ms);
    void RecordWriteLatency(double ms);
    void RecordTokenReduction(size_t before_tokens, size_t after_tokens);
    void RecordFallback(bool used_fallback);
    void SetSyncLag(uint64_t lag);

    StatsSnapshot Snapshot() const;

private:
    double ComputeP95(const std::vector<double>& samples) const;

    mutable std::mutex mu_;
    std::vector<double> read_lat_ms_;
    std::vector<double> write_lat_ms_;

    uint64_t token_before_{0};
    uint64_t token_after_{0};

    uint64_t fallback_total_{0};
    uint64_t fallback_used_{0};

    uint64_t sync_lag_ops_{0};
};

}  // namespace pgmem::core
