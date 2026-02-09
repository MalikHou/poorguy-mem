#include "pgmem/core/metrics.h"

#include <algorithm>
#include <cmath>

namespace pgmem::core {

void Metrics::RecordReadLatency(double ms) {
    std::lock_guard<std::mutex> lock(mu_);
    read_lat_ms_.push_back(ms);
}

void Metrics::RecordWriteLatency(double ms) {
    std::lock_guard<std::mutex> lock(mu_);
    write_lat_ms_.push_back(ms);
}

void Metrics::RecordTokenReduction(size_t before_tokens, size_t after_tokens) {
    std::lock_guard<std::mutex> lock(mu_);
    token_before_ += static_cast<uint64_t>(before_tokens);
    token_after_ += static_cast<uint64_t>(after_tokens);
}

void Metrics::RecordFallback(bool used_fallback) {
    std::lock_guard<std::mutex> lock(mu_);
    ++fallback_total_;
    if (used_fallback) {
        ++fallback_used_;
    }
}

void Metrics::SetSyncLag(uint64_t lag) {
    std::lock_guard<std::mutex> lock(mu_);
    sync_lag_ops_ = lag;
}

StatsSnapshot Metrics::Snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    StatsSnapshot out;
    out.p95_read_ms = ComputeP95(read_lat_ms_);
    out.p95_write_ms = ComputeP95(write_lat_ms_);

    if (token_before_ > 0 && token_before_ >= token_after_) {
        out.token_reduction_ratio =
            static_cast<double>(token_before_ - token_after_) / static_cast<double>(token_before_);
    }

    out.sync_lag_ops = sync_lag_ops_;
    if (fallback_total_ > 0) {
        out.fallback_rate = static_cast<double>(fallback_used_) / static_cast<double>(fallback_total_);
    }
    return out;
}

double Metrics::ComputeP95(const std::vector<double>& samples) const {
    if (samples.empty()) {
        return 0.0;
    }
    std::vector<double> copy = samples;
    std::sort(copy.begin(), copy.end());
    const size_t idx = static_cast<size_t>(std::ceil((copy.size() - 1) * 0.95));
    return copy[idx];
}

}  // namespace pgmem::core
