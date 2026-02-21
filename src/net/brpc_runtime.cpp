#include "pgmem/net/brpc_runtime.h"

#include <gflags/gflags_declare.h>

#include <cstdint>
#include <limits>

namespace brpc {
DECLARE_int32(event_dispatcher_num);
}  // namespace brpc

namespace bthread {
DECLARE_int32(bthread_concurrency);
}  // namespace bthread

DECLARE_bool(use_io_uring);

namespace pgmem::net {
namespace {

int ResolveCoreCount(int configured) { return configured; }

bool ValidatePositiveInt32(int value, const char* name, std::string* error) {
    if (value <= 0) {
        if (error != nullptr) {
            *error = std::string(name) + " must be positive";
        }
        return false;
    }
    if (value > std::numeric_limits<int32_t>::max()) {
        if (error != nullptr) {
            *error = std::string(name) + " exceeds int32 range";
        }
        return false;
    }
    return true;
}

}  // namespace

bool ApplyBrpcRuntimeOptions(const BrpcRuntimeOptions& options, BrpcRuntimeState* state, std::string* error) {
    if (options.core_number <= 0) {
        if (error != nullptr) {
            *error = "core_number must be positive";
        }
        return false;
    }
    if (options.event_dispatcher_num < 0) {
        if (error != nullptr) {
            *error = "event_dispatcher_num must be >= 0";
        }
        return false;
    }

    const int resolved_core_number = ResolveCoreCount(options.core_number);
    const int resolved_event_dispatcher_num =
        options.event_dispatcher_num > 0 ? options.event_dispatcher_num : resolved_core_number;

    if (!ValidatePositiveInt32(resolved_core_number, "core_number", error)) {
        return false;
    }
    if (!ValidatePositiveInt32(resolved_event_dispatcher_num, "event_dispatcher_num", error)) {
        return false;
    }

    bthread::FLAGS_bthread_concurrency = static_cast<int32_t>(resolved_core_number);
    brpc::FLAGS_event_dispatcher_num   = static_cast<int32_t>(resolved_event_dispatcher_num);

    FLAGS_use_io_uring = true;

    if (state != nullptr) {
        state->resolved_core_number          = resolved_core_number;
        state->resolved_event_dispatcher_num = resolved_event_dispatcher_num;
        state->network_io_uring_enabled      = true;
    }
    return true;
}

}  // namespace pgmem::net
