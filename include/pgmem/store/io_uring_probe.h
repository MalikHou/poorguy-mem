#pragma once

#include <string>

namespace pgmem::store {

bool IsIoUringAvailable(std::string* error, bool force_unavailable = false);

}  // namespace pgmem::store
