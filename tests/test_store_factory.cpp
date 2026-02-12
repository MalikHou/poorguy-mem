#include <cstdlib>
#include <memory>
#include <vector>

#include "pgmem/store/io_uring_probe.h"
#include "pgmem/store/store_adapter.h"
#include "test_framework.h"

TEST_CASE(test_store_factory_inmemory_backend) {
    pgmem::store::StoreAdapterConfig cfg;
    cfg.backend = "inmemory";

    std::string error;
    auto store = pgmem::store::CreateStoreAdapter(cfg, &error);
    ASSERT_TRUE(store != nullptr);

    const auto put = store->Put("ns", "k", "v", 1);
    ASSERT_TRUE(put.ok);

    const auto get = store->Get("ns", "k");
    ASSERT_TRUE(get.found);
    ASSERT_EQ(get.value, "v");
}

TEST_CASE(test_store_factory_unknown_backend) {
    pgmem::store::StoreAdapterConfig cfg;
    cfg.backend = "not-a-backend";

    std::string error;
    auto store = pgmem::store::CreateStoreAdapter(cfg, &error);
    ASSERT_TRUE(store == nullptr);
    ASSERT_TRUE(error.find("unknown backend") != std::string::npos);
}

TEST_CASE(test_store_batch_delete_and_usage) {
    pgmem::store::StoreAdapterConfig cfg;
    cfg.backend = "inmemory";

    std::string error;
    auto store = pgmem::store::CreateStoreAdapter(cfg, &error);
    ASSERT_TRUE(store != nullptr);

    std::vector<pgmem::WriteEntry> writes;
    writes.push_back(pgmem::WriteEntry{
        pgmem::WriteOp::Upsert,
        pgmem::KeyValueEntry{"ns", "a", "value-a", 1},
    });
    writes.push_back(pgmem::WriteEntry{
        pgmem::WriteOp::Upsert,
        pgmem::KeyValueEntry{"ns", "b", "value-b", 2},
    });
    const auto batch_res = store->BatchWrite(writes);
    ASSERT_TRUE(batch_res.ok);

    auto usage = store->ApproximateUsage(&error);
    ASSERT_TRUE(usage.item_count >= 2);

    const auto del_res = store->Delete("ns", "a", 3);
    ASSERT_TRUE(del_res.ok);

    const auto get_a = store->Get("ns", "a");
    ASSERT_TRUE(!get_a.found);
}

TEST_CASE(test_io_uring_probe_force_unavailable_env) {
    ASSERT_TRUE(setenv("PGMEM_FORCE_IO_URING_UNAVAILABLE", "1", 1) == 0);

    std::string error;
    const bool available = pgmem::store::IsIoUringAvailable(&error);
    ASSERT_TRUE(!available);
    ASSERT_TRUE(error.find("forced unavailable") != std::string::npos);

    ASSERT_TRUE(unsetenv("PGMEM_FORCE_IO_URING_UNAVAILABLE") == 0);
}
