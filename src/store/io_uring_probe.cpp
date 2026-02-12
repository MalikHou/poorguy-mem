#include "pgmem/store/io_uring_probe.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef PGMEM_WITH_ELOQSTORE
#include <liburing.h>
#endif

namespace pgmem::store {
namespace {

bool ParseBoolEnv(const char* value) {
    if (value == nullptr) {
        return false;
    }

    std::string lowered(value);
    for (char& c : lowered) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    return lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on";
}

}  // namespace

bool IsIoUringAvailable(std::string* error) {
    if (ParseBoolEnv(std::getenv("PGMEM_FORCE_IO_URING_UNAVAILABLE"))) {
        if (error != nullptr) {
            *error = "forced unavailable by PGMEM_FORCE_IO_URING_UNAVAILABLE";
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
