// Example custom backend integration.
// Build with:
//   cmake -S . -B build-custom \
//     -DPGMEM_STORE_BACKEND=custom \
//     -DPGMEM_CUSTOM_STORE_SOURCES=/absolute/path/to/this/file

#include "pgmem/store/store_adapter.h"

namespace pgmem::store {

std::unique_ptr<IStoreAdapter> CreateCustomStoreAdapter(const StoreAdapterConfig& config,
                                                        std::string* error) {
    (void)config;
    (void)error;

    // Replace with your own adapter implementation.
    // Using in-memory fallback here only as a build example.
    return CreateInMemoryStoreAdapter();
}

}  // namespace pgmem::store
