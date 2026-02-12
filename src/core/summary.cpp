#include "pgmem/core/summary.h"

#include <sstream>

#include "pgmem/util/text.h"

namespace pgmem::core {

std::string SummaryEngine::Update(const std::string& existing_summary, const std::string& user_text,
                                  const std::string& assistant_text, const std::vector<std::string>& code_snippets,
                                  const std::vector<std::string>& commands, size_t max_tokens) const {
    std::ostringstream oss;
    if (!existing_summary.empty()) {
        oss << existing_summary << "\n";
    }
    oss << "User: " << util::Trim(user_text) << "\n";
    oss << "Assistant: " << util::Trim(assistant_text) << "\n";

    if (!code_snippets.empty()) {
        oss << "Code: " << util::Trim(code_snippets.back()) << "\n";
    }
    if (!commands.empty()) {
        oss << "Cmd: " << util::Trim(commands.back()) << "\n";
    }

    std::string out = oss.str();

    // Keep most recent content within token budget.
    while (util::EstimateTokenCount(out) > max_tokens && out.size() > 64) {
        const size_t drop = out.find('\n');
        if (drop == std::string::npos) {
            break;
        }
        out = out.substr(drop + 1);
    }

    return out;
}

}  // namespace pgmem::core
