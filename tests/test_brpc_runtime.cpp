#include <string>

#include "pgmem/net/brpc_runtime.h"
#include "test_framework.h"

TEST_CASE(test_brpc_runtime_event_dispatcher_follows_core) {
    pgmem::net::BrpcRuntimeOptions options;
    options.core_number          = 4;
    options.event_dispatcher_num = 0;

    pgmem::net::BrpcRuntimeState state;
    std::string error;
    ASSERT_TRUE(pgmem::net::ApplyBrpcRuntimeOptions(options, &state, &error));
    ASSERT_TRUE(error.empty());
    ASSERT_EQ(state.resolved_core_number, 4);
    ASSERT_EQ(state.resolved_event_dispatcher_num, state.resolved_core_number);
    ASSERT_TRUE(state.network_io_uring_enabled);
}

TEST_CASE(test_brpc_runtime_explicit_event_dispatcher_override) {
    pgmem::net::BrpcRuntimeOptions options;
    options.core_number          = 2;
    options.event_dispatcher_num = 5;

    pgmem::net::BrpcRuntimeState state;
    std::string error;
    ASSERT_TRUE(pgmem::net::ApplyBrpcRuntimeOptions(options, &state, &error));
    ASSERT_TRUE(error.empty());
    ASSERT_EQ(state.resolved_core_number, 2);
    ASSERT_EQ(state.resolved_event_dispatcher_num, 5);
    ASSERT_TRUE(state.network_io_uring_enabled);
}

TEST_CASE(test_brpc_runtime_rejects_non_positive_or_negative_values) {
    {
        pgmem::net::BrpcRuntimeOptions options;
        options.core_number          = 0;
        options.event_dispatcher_num = 0;

        pgmem::net::BrpcRuntimeState state;
        std::string error;
        ASSERT_TRUE(!pgmem::net::ApplyBrpcRuntimeOptions(options, &state, &error));
        ASSERT_TRUE(error.find("core_number must be positive") != std::string::npos);
    }

    {
        pgmem::net::BrpcRuntimeOptions options;
        options.core_number          = -1;
        options.event_dispatcher_num = 0;

        pgmem::net::BrpcRuntimeState state;
        std::string error;
        ASSERT_TRUE(!pgmem::net::ApplyBrpcRuntimeOptions(options, &state, &error));
        ASSERT_TRUE(error.find("core_number must be positive") != std::string::npos);
    }

    {
        pgmem::net::BrpcRuntimeOptions options;
        options.core_number          = 2;
        options.event_dispatcher_num = -1;

        pgmem::net::BrpcRuntimeState state;
        std::string error;
        ASSERT_TRUE(!pgmem::net::ApplyBrpcRuntimeOptions(options, &state, &error));
        ASSERT_TRUE(error.find("event_dispatcher_num must be >= 0") != std::string::npos);
    }
}
