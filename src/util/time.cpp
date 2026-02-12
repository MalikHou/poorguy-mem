#include "pgmem/util/time.h"

#include <chrono>

namespace pgmem::util {

uint64_t NowMs() {
    const auto now = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
    return static_cast<uint64_t>(now.time_since_epoch().count());
}

}  // namespace pgmem::util
