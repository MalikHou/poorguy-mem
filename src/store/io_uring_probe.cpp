#include "pgmem/store/io_uring_probe.h"

#include <cstring>
#include <string>

#ifdef PGMEM_WITH_ELOQSTORE
#include <liburing.h>
#endif

namespace pgmem::store {

bool IsIoUringAvailable(std::string* error, bool force_unavailable) {
    if (force_unavailable) {
        if (error != nullptr) {
            *error = "forced unavailable by test flag";
        }
        return false;
    }

#ifdef PGMEM_WITH_ELOQSTORE
    io_uring ring;
    std::memset(&ring, 0, sizeof(ring));
    const int rc = io_uring_queue_init(2, &ring, 0);
    if (rc < 0) {
        if (error != nullptr) {
            *error = std::string("io_uring_queue_init failed: ") + std::strerror(-rc);
        }
        return false;
    }
    io_uring_queue_exit(&ring);
    return true;
#else
    if (error != nullptr) {
        *error = "binary built without PGMEM_WITH_ELOQSTORE";
    }
    return false;
#endif
}

}  // namespace pgmem::store
