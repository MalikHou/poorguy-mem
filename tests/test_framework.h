#pragma once

#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

using TestFn = std::function<void()>;

struct TestCase {
    std::string name;
    TestFn fn;
};

std::vector<TestCase>& TestRegistry();

struct Registrar {
    Registrar(std::string name, TestFn fn);
};

#define TEST_CASE(name)                        \
    static void name();                        \
    static Registrar reg_##name(#name, name); \
    static void name()

#define ASSERT_TRUE(cond)                                                \
    do {                                                                 \
        if (!(cond)) {                                                   \
            throw std::runtime_error(std::string("Assertion failed: ") + #cond); \
        }                                                                \
    } while (0)

#define ASSERT_EQ(lhs, rhs) ASSERT_TRUE((lhs) == (rhs))
