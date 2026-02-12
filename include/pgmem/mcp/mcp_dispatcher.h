#pragma once

#include <string>

#include "pgmem/core/memory_engine.h"
#include "pgmem/util/json.h"

namespace pgmem::mcp {

class McpDispatcher {
public:
    explicit McpDispatcher(core::MemoryEngine* engine);

    util::Json Handle(const util::Json& request);
    util::Json Describe() const;

private:
    util::Json HandleBootstrap(const util::Json& params);
    util::Json HandleCommitTurn(const util::Json& params);
    util::Json HandleSearch(const util::Json& params);
    util::Json HandlePin(const util::Json& params);
    util::Json HandleStats(const util::Json& params);
    util::Json HandleCompact(const util::Json& params);
    util::Json HandleDescribe(const util::Json& params);

    util::Json Error(int code, const std::string& message, const util::Json& id) const;

    core::MemoryEngine* engine_{nullptr};
};

}  // namespace pgmem::mcp
