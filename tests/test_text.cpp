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

TEST_CASE(test_tokenize_mixed_cjk_and_ascii) {
    const auto tokens = pgmem::util::Tokenize("发布窗口 Friday-20:00, retry5");
    ASSERT_EQ(tokens.size(), 6U);
    ASSERT_EQ(tokens[0], "发布");
    ASSERT_EQ(tokens[1], "布窗");
    ASSERT_EQ(tokens[2], "窗口");
    ASSERT_EQ(tokens[3], "friday-20");
    ASSERT_EQ(tokens[4], "00");
    ASSERT_EQ(tokens[5], "retry5");
}

TEST_CASE(test_tokenize_cjk_single_char_fallback) {
    const auto tokens = pgmem::util::Tokenize("中 A 文");
    ASSERT_EQ(tokens.size(), 3U);
    ASSERT_EQ(tokens[0], "中");
    ASSERT_EQ(tokens[1], "a");
    ASSERT_EQ(tokens[2], "文");
}
