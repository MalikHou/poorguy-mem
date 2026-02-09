#include "pgmem/util/text.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>

namespace pgmem::util {

std::string Trim(const std::string& input) {
    size_t start = 0;
    while (start < input.size() && std::isspace(static_cast<unsigned char>(input[start])) != 0) {
        ++start;
    }
    size_t end = input.size();
    while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1])) != 0) {
        --end;
    }
    return input.substr(start, end - start);
}

std::string ToLower(const std::string& input) {
    std::string out = input;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

std::vector<std::string> Tokenize(const std::string& input) {
    std::vector<std::string> tokens;
    std::string cur;
    for (char c : input) {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-') {
            cur.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        } else if (!cur.empty()) {
            tokens.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) {
        tokens.push_back(cur);
    }
    return tokens;
}

std::string JoinLines(const std::vector<std::string>& lines) {
    std::ostringstream oss;
    for (size_t i = 0; i < lines.size(); ++i) {
        oss << lines[i];
        if (i + 1 < lines.size()) {
            oss << "\n";
        }
    }
    return oss.str();
}

std::string RedactSecrets(const std::string& input) {
    std::string out = input;

    static const std::regex kBearer(R"(bearer\s+[A-Za-z0-9._\-]+)", std::regex::icase);
    static const std::regex kApiKey(R"((api[_-]?key\s*[:=]\s*)([A-Za-z0-9_\-]{8,}))", std::regex::icase);
    static const std::regex kPassword(R"((password\s*[:=]\s*)([^\s,;]+))", std::regex::icase);
    static const std::regex kSkToken(R"((sk-[A-Za-z0-9]{16,}))");

    out = std::regex_replace(out, kBearer, "Bearer [REDACTED]");
    out = std::regex_replace(out, kApiKey, "$1[REDACTED]");
    out = std::regex_replace(out, kPassword, "$1[REDACTED]");
    out = std::regex_replace(out, kSkToken, "[REDACTED]");

    return out;
}

size_t EstimateTokenCount(const std::string& text) {
    if (text.empty()) {
        return 0;
    }
    // A pragmatic approximation: 1 token ~= 4 chars in mixed code+text.
    return (text.size() + 3) / 4;
}

}  // namespace pgmem::util
