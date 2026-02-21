#pragma once

#include <string>

#include "pgmem/config/defaults.h"

namespace pgmem::net {

struct BrpcRuntimeOptions {
    int core_number{config::kDefaultCoreNumber};
    int event_dispatcher_num{config::kDefaultEventDispatcherNum};
};

struct BrpcRuntimeState {
    int resolved_core_number{1};
    int resolved_event_dispatcher_num{1};
    bool network_io_uring_enabled{true};
};

bool ApplyBrpcRuntimeOptions(const BrpcRuntimeOptions& options, BrpcRuntimeState* state, std::string* error);

}  // namespace pgmem::net
