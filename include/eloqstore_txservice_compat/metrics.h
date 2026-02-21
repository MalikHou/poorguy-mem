#pragma once

#include <chrono>
#include <map>
#include <string>
#include <vector>

namespace metrics {

using Clock     = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Name      = std::string;

using CommonLabels = std::map<std::string, std::string>;
using LabelOptions = std::map<std::string, std::vector<std::string>>;

enum class Type {
    Counter,
    Gauge,
    Histogram,
};

class MetricsRegistry {};

}  // namespace metrics
