#include "pgmem/store/store_adapter.h"

#include <algorithm>
#include <cctype>

namespace pgmem::store {
namespace {

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

const char* DefaultBackend() {
#ifdef PGMEM_DEFAULT_STORE_BACKEND
    return PGMEM_DEFAULT_STORE_BACKEND;
#else
    return "eloqstore";
#endif
}

}  // namespace

std::unique_ptr<IStoreAdapter> CreateStoreAdapter(const StoreAdapterConfig& config,
                                                  std::string* error) {
    StoreAdapterConfig effective = config;
    if (effective.backend.empty()) {
        effective.backend = DefaultBackend();
    }
    if (effective.root_path.empty()) {
        effective.root_path = ".pgmem/store";
    }

    const std::string backend = ToLower(effective.backend);

    if (backend == "inmemory" || backend == "memory") {
        return CreateInMemoryStoreAdapter();
    }

    if (backend == "eloqstore") {
        return CreateEloqStoreAdapter(effective, error);
    }

    if (backend == "custom") {
#ifdef PGMEM_HAS_CUSTOM_STORE
        return CreateCustomStoreAdapter(effective, error);
#else
        if (error != nullptr) {
            *error = "backend=custom requested but binary was built without PGMEM_HAS_CUSTOM_STORE";
        }
        return nullptr;
#endif
    }

    if (error != nullptr) {
        *error = "unknown backend: " + effective.backend;
    }
    return nullptr;
}

}  // namespace pgmem::store
