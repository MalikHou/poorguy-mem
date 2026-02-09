#pragma once

#include <string>
#include <vector>

namespace pgmem::core {

class SummaryEngine {
public:
    std::string Update(const std::string& existing_summary,
                       const std::string& user_text,
                       const std::string& assistant_text,
                       const std::vector<std::string>& code_snippets,
                       const std::vector<std::string>& commands,
                       size_t max_tokens) const;
};

}  // namespace pgmem::core
