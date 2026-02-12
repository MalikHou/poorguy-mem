#include "pgmem/core/metrics.h"

#include <array>
#include <cmath>

namespace pgmem::core {
namespace {

constexpr size_t kLatencyBucketCountLocal                                    = 16;
constexpr std::array<double, kLatencyBucketCountLocal> kLatencyBucketUpperMs = {
    0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 20.0, 40.0, 80.0, 120.0, 200.0, 400.0, 800.0, 1600.0, 3200.0};

}  // namespace

Metrics::Metrics() {
    for (auto& v : read_lat_hist_) {
        v.store(0, std::memory_order_release);
    }
    for (auto& v : write_lat_hist_) {
        v.store(0, std::memory_order_release);
    }
}

void Metrics::RecordReadLatency(double ms) {
    const size_t idx = BucketIndex(ms);
    read_lat_hist_[idx].fetch_add(1, std::memory_order_acq_rel);
}

void Metrics::RecordWriteLatency(double ms) {
    const size_t idx = BucketIndex(ms);
    write_lat_hist_[idx].fetch_add(1, std::memory_order_acq_rel);
}

void Metrics::RecordTokenReduction(size_t before_tokens, size_t after_tokens) {
    token_before_.fetch_add(static_cast<uint64_t>(before_tokens), std::memory_order_acq_rel);
    token_after_.fetch_add(static_cast<uint64_t>(after_tokens), std::memory_order_acq_rel);
}

void Metrics::RecordFallback(bool used_fallback) {
    fallback_total_.fetch_add(1, std::memory_order_acq_rel);
    if (used_fallback) {
        fallback_used_.fetch_add(1, std::memory_order_acq_rel);
    }
}

StatsSnapshot Metrics::Snapshot() const {
    StatsSnapshot out;
    out.p95_read_ms  = ComputeP95(read_lat_hist_);
    out.p95_write_ms = ComputeP95(write_lat_hist_);

    const uint64_t before = token_before_.load(std::memory_order_acquire);
    const uint64_t after  = token_after_.load(std::memory_order_acquire);
    if (before > 0 && before >= after) {
        out.token_reduction_ratio = static_cast<double>(before - after) / static_cast<double>(before);
    }

    const uint64_t fallback_total = fallback_total_.load(std::memory_order_acquire);
    const uint64_t fallback_used  = fallback_used_.load(std::memory_order_acquire);
    if (fallback_total > 0) {
        out.fallback_rate = static_cast<double>(fallback_used) / static_cast<double>(fallback_total);
    }
    return out;
}

size_t Metrics::BucketIndex(double ms) const {
    if (ms < 0.0) {
        return 0;
    }
    for (size_t i = 0; i < kLatencyBucketUpperMs.size(); ++i) {
        if (ms <= kLatencyBucketUpperMs[i]) {
            return i;
        }
    }
    return kLatencyBucketCount;
}

double Metrics::ComputeP95(const std::array<std::atomic<uint64_t>, kLatencyBucketCount + 1>& hist) const {
    uint64_t total = 0;
    for (const auto& v : hist) {
        total += v.load(std::memory_order_acquire);
    }
    if (total == 0) {
        return 0.0;
    }

    const uint64_t target = static_cast<uint64_t>(std::ceil(static_cast<double>(total) * 0.95));
    uint64_t cumulative   = 0;
    for (size_t i = 0; i < hist.size(); ++i) {
        cumulative += hist[i].load(std::memory_order_acquire);
        if (cumulative >= target) {
            if (i < kLatencyBucketUpperMs.size()) {
                return kLatencyBucketUpperMs[i];
            }
            return kLatencyBucketUpperMs.back();
        }
    }
    return kLatencyBucketUpperMs.back();
}

}  // namespace pgmem::core
