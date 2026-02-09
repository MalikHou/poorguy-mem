#include "test_framework.h"

#include <exception>
#include <iostream>

std::vector<TestCase>& TestRegistry() {
    static std::vector<TestCase> tests;
    return tests;
}

Registrar::Registrar(std::string name, TestFn fn) {
    TestRegistry().push_back(TestCase{std::move(name), std::move(fn)});
}

int main() {
    int failed = 0;
    for (const auto& test : TestRegistry()) {
        try {
            test.fn();
            std::cout << "[PASS] " << test.name << "\n";
        } catch (const std::exception& ex) {
            ++failed;
            std::cerr << "[FAIL] " << test.name << ": " << ex.what() << "\n";
        }
    }

    if (failed > 0) {
        std::cerr << failed << " test(s) failed\n";
        return 1;
    }

    std::cout << "all tests passed\n";
    return 0;
}
