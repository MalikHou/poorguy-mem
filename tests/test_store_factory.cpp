#include "test_framework.h"

#include <memory>

#include "pgmem/store/store_adapter.h"

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
