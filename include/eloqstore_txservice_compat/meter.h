#pragma once

#include <string>
#include <utility>

#include "metrics.h"

namespace metrics {

class Meter {
public:
    Meter(MetricsRegistry* registry, CommonLabels labels) : registry_(registry), labels_(std::move(labels)) {}

    void Register(const Name& name, Type type, const LabelOptions& options = {}) {
        (void)name;
        (void)type;
        (void)options;
    }

    void Collect(const Name& name, double value, const std::string& label_value = {}) {
        (void)name;
        (void)value;
        (void)label_value;
    }

    void CollectDuration(const Name& name, TimePoint start, const std::string& label_value = {}) {
        (void)name;
        (void)start;
        (void)label_value;
    }

private:
    MetricsRegistry* registry_{nullptr};
    CommonLabels labels_;
};

}  // namespace metrics
