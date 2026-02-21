#include "pgmem/util/text.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <regex>
#include <sstream>

namespace pgmem::util {
namespace {

bool IsAsciiWordChar(unsigned char c) { return std::isalnum(c) != 0 || c == '_' || c == '-'; }

bool DecodeUtf8At(const std::string& input, size_t i, uint32_t* codepoint, size_t* length) {
    if (i >= input.size() || codepoint == nullptr || length == nullptr) {
        return false;
    }

    const unsigned char b0 = static_cast<unsigned char>(input[i]);
    if (b0 < 0x80) {
        *codepoint = b0;
        *length    = 1;
        return true;
    }

    if (b0 >= 0xC2 && b0 <= 0xDF) {
        if (i + 1 >= input.size()) {
            return false;
        }
        const unsigned char b1 = static_cast<unsigned char>(input[i + 1]);
        if ((b1 & 0xC0) != 0x80) {
            return false;
        }
        *codepoint = (static_cast<uint32_t>(b0 & 0x1F) << 6) | static_cast<uint32_t>(b1 & 0x3F);
        *length    = 2;
        return true;
    }

    if (b0 >= 0xE0 && b0 <= 0xEF) {
        if (i + 2 >= input.size()) {
            return false;
        }
        const unsigned char b1 = static_cast<unsigned char>(input[i + 1]);
        const unsigned char b2 = static_cast<unsigned char>(input[i + 2]);
        if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) {
            return false;
        }
        if (b0 == 0xE0 && b1 < 0xA0) {
            return false;  // Overlong
        }
        if (b0 == 0xED && b1 >= 0xA0) {
            return false;  // UTF-16 surrogate range
        }
        *codepoint = (static_cast<uint32_t>(b0 & 0x0F) << 12) | (static_cast<uint32_t>(b1 & 0x3F) << 6) |
                     static_cast<uint32_t>(b2 & 0x3F);
        *length = 3;
        return true;
    }

    if (b0 >= 0xF0 && b0 <= 0xF4) {
        if (i + 3 >= input.size()) {
            return false;
        }
        const unsigned char b1 = static_cast<unsigned char>(input[i + 1]);
        const unsigned char b2 = static_cast<unsigned char>(input[i + 2]);
        const unsigned char b3 = static_cast<unsigned char>(input[i + 3]);
        if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80) {
            return false;
        }
        if (b0 == 0xF0 && b1 < 0x90) {
            return false;  // Overlong
        }
        if (b0 == 0xF4 && b1 > 0x8F) {
            return false;  // > U+10FFFF
        }
        *codepoint = (static_cast<uint32_t>(b0 & 0x07) << 18) | (static_cast<uint32_t>(b1 & 0x3F) << 12) |
                     (static_cast<uint32_t>(b2 & 0x3F) << 6) | static_cast<uint32_t>(b3 & 0x3F);
        *length = 4;
        return true;
    }

    return false;
}

bool IsCjkCodepoint(uint32_t cp) {
    return (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0x20000 && cp <= 0x2A6DF) || (cp >= 0x2A700 && cp <= 0x2B73F) || (cp >= 0x2B740 && cp <= 0x2B81F) ||
           (cp >= 0x2B820 && cp <= 0x2CEAF) || (cp >= 0x2CEB0 && cp <= 0x2EBEF) || (cp >= 0x30000 && cp <= 0x3134F);
}

void FlushAsciiToken(std::string* cur, std::vector<std::string>* tokens) {
    if (cur == nullptr || tokens == nullptr || cur->empty()) {
        return;
    }
    tokens->push_back(*cur);
    cur->clear();
}

void FlushCjkRunAsBigrams(std::vector<std::string>* cjk_run, std::vector<std::string>* tokens) {
    if (cjk_run == nullptr || tokens == nullptr || cjk_run->empty()) {
        return;
    }
    if (cjk_run->size() == 1) {
        tokens->push_back((*cjk_run)[0]);
        cjk_run->clear();
        return;
    }
    for (size_t i = 0; i + 1 < cjk_run->size(); ++i) {
        tokens->push_back((*cjk_run)[i] + (*cjk_run)[i + 1]);
    }
    cjk_run->clear();
}

}  // namespace

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
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::vector<std::string> Tokenize(const std::string& input) {
    std::vector<std::string> tokens;
    std::string cur;
    std::vector<std::string> cjk_run;
    for (size_t i = 0; i < input.size();) {
        const unsigned char c = static_cast<unsigned char>(input[i]);
        if (c < 0x80) {
            FlushCjkRunAsBigrams(&cjk_run, &tokens);
            if (IsAsciiWordChar(c)) {
                cur.push_back(static_cast<char>(std::tolower(c)));
            } else {
                FlushAsciiToken(&cur, &tokens);
            }
            ++i;
            continue;
        }

        FlushAsciiToken(&cur, &tokens);

        uint32_t cp = 0;
        size_t len  = 0;
        if (!DecodeUtf8At(input, i, &cp, &len)) {
            FlushCjkRunAsBigrams(&cjk_run, &tokens);
            ++i;
            continue;
        }

        if (IsCjkCodepoint(cp)) {
            cjk_run.push_back(input.substr(i, len));
        } else {
            FlushCjkRunAsBigrams(&cjk_run, &tokens);
        }
        i += len;
    }
    FlushAsciiToken(&cur, &tokens);
    FlushCjkRunAsBigrams(&cjk_run, &tokens);
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
