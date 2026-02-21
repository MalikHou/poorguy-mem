#include "pgmem/core/analyzer.h"
#include "pgmem/util/text.h"

namespace pgmem::core {

std::vector<std::string> DefaultAnalyzer::Tokenize(const std::string& text) const { return util::Tokenize(text); }

std::unique_ptr<IAnalyzer> CreateDefaultAnalyzer() { return std::make_unique<DefaultAnalyzer>(); }

}  // namespace pgmem::core
