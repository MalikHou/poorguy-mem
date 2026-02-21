#pragma once

#include <memory>
#include <string>
#include <vector>

namespace pgmem::core {

class IAnalyzer {
public:
    virtual ~IAnalyzer()                                                     = default;
    virtual std::vector<std::string> Tokenize(const std::string& text) const = 0;
};

class DefaultAnalyzer final : public IAnalyzer {
public:
    std::vector<std::string> Tokenize(const std::string& text) const override;
};

std::unique_ptr<IAnalyzer> CreateDefaultAnalyzer();

}  // namespace pgmem::core
