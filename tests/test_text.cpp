#include "pgmem/util/text.h"
#include "test_framework.h"

TEST_CASE(test_redact_secrets) {
    const std::string input = "api_key=abcd1234 password:letmein bearer token123 sk-abcdefghijklmnopqrstuvwxyz";
    const std::string out   = pgmem::util::RedactSecrets(input);
    ASSERT_TRUE(out.find("[REDACTED]") != std::string::npos);
    ASSERT_TRUE(out.find("letmein") == std::string::npos);
}

TEST_CASE(test_tokenize) {
    const auto tokens = pgmem::util::Tokenize("Hello, Cpp17-world!");
    ASSERT_EQ(tokens.size(), 2U);
    ASSERT_EQ(tokens[0], "hello");
    ASSERT_EQ(tokens[1], "cpp17-world");
}
