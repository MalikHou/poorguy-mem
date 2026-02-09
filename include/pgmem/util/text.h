#pragma once

#include <string>
#include <vector>

namespace pgmem::util {

std::string Trim(const std::string& input);
std::string ToLower(const std::string& input);
std::vector<std::string> Tokenize(const std::string& input);
std::string JoinLines(const std::vector<std::string>& lines);
std::string RedactSecrets(const std::string& input);
size_t EstimateTokenCount(const std::string& text);

}  // namespace pgmem::util
