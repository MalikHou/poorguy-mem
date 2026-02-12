#pragma once

#include <string>

namespace pgmem::store {

bool IsIoUringAvailable(std::string* error);

}  // namespace pgmem::store
