#pragma once

#include <cstdint>

namespace pgmem::config {

constexpr const char* kDefaultHost = "127.0.0.1";

constexpr int kDefaultMcpPort = 8765;

constexpr const char* kDefaultStoreBackend         = "eloqstore";
constexpr int kDefaultStoreThreads                 = 0;
constexpr uint32_t kDefaultStorePartitions         = 64;
constexpr int kDefaultCoreNumber                   = 4;
constexpr int kDefaultEventDispatcherNum           = 0;
constexpr bool kDefaultEnableIoUringNetworkEngine  = false;
constexpr bool kDefaultAppendMode                  = true;
constexpr bool kDefaultEnableCompression           = false;
constexpr const char* kDefaultWriteAckMode         = "accepted";
constexpr uint32_t kDefaultVolatileFlushIntervalMs = 100;
constexpr uint32_t kDefaultVolatileMaxPendingOps   = 4096;
constexpr uint32_t kDefaultShutdownDrainTimeoutMs  = 3000;

constexpr const char* kDefaultStoreRoot = ".pgmem/store";

constexpr const char* kDefaultNodeIdLocal = "local";

constexpr uint64_t kDefaultMemBudgetMb    = 512;
constexpr uint64_t kDefaultDiskBudgetGb   = 20;
constexpr double kDefaultGcHighWatermark  = 0.90;
constexpr double kDefaultGcLowWatermark   = 0.75;
constexpr uint32_t kDefaultGcBatchSize    = 256;
constexpr uint64_t kDefaultMaxRecordBytes = 131072;
constexpr bool kDefaultEnableTombstoneGc  = true;

constexpr const char* kDefaultMcpUrl  = "http://127.0.0.1:8765/mcp";
constexpr const char* kDefaultMcpName = "poorguy-mem";

}  // namespace pgmem::config
