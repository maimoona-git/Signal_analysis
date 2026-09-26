#pragma once

// Minimal dependency-free test harness. Each test binary is a plain
// executable that returns non-zero on failure, so ctest can run it natively
// or under node (the Emscripten toolchain's CMAKE_CROSSCOMPILING_EMULATOR)
// without any extra framework having to be cross-compiled to WASM.

#include <cmath>
#include <cstdio>
#include <exception>
#include <functional>
#include <string>
#include <vector>

namespace sigtest {

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

struct Failure {
    std::string message;
};

inline int run_all() {
    int failed = 0;
    for (const auto& t : registry()) {
        try {
            t.fn();
            std::printf("[ PASS ] %s\n", t.name);
        } catch (const Failure& f) {
            ++failed;
            std::printf("[ FAIL ] %s\n         %s\n", t.name, f.message.c_str());
        } catch (const std::exception& e) {
            ++failed;
            std::printf("[ FAIL ] %s\n         unexpected exception: %s\n", t.name, e.what());
        }
    }
    std::printf("%zu tests, %d failed\n", registry().size(), failed);
    return failed == 0 ? 0 : 1;
}

}  // namespace sigtest

#define SIG_CONCAT_INNER(a, b) a##b
#define SIG_CONCAT(a, b) SIG_CONCAT_INNER(a, b)

#define TEST_CASE(name)                                                             \
    static void SIG_CONCAT(sig_test_fn_, __LINE__)();                               \
    static ::sigtest::Registrar SIG_CONCAT(sig_test_reg_, __LINE__)(                \
        name, &SIG_CONCAT(sig_test_fn_, __LINE__));                                 \
    static void SIG_CONCAT(sig_test_fn_, __LINE__)()

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond))                                                                \
            throw ::sigtest::Failure{std::string(__FILE__) + ":" +                  \
                                     std::to_string(__LINE__) + ": CHECK(" #cond ")"}; \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                       \
    do {                                                                            \
        const double sig_a_ = (a), sig_b_ = (b);                                    \
        if (!(std::fabs(sig_a_ - sig_b_) <= (tol)))                                 \
            throw ::sigtest::Failure{std::string(__FILE__) + ":" +                  \
                                     std::to_string(__LINE__) + ": " #a " = " +     \
                                     std::to_string(sig_a_) + ", expected " #b " = " + \
                                     std::to_string(sig_b_)};                       \
    } while (0)

// Integer equality with both values in the failure message (handy for golden
// hashes, where the actual value is what you need to see).
#define CHECK_EQ(a, b)                                                              \
    do {                                                                            \
        const long long sig_a_ = static_cast<long long>(a);                         \
        const long long sig_b_ = static_cast<long long>(b);                         \
        if (sig_a_ != sig_b_)                                                       \
            throw ::sigtest::Failure{std::string(__FILE__) + ":" +                  \
                                     std::to_string(__LINE__) + ": " #a " = " +     \
                                     std::to_string(sig_a_) + ", expected " #b " = " + \
                                     std::to_string(sig_b_)};                       \
    } while (0)

// The statement must throw std::exception (sigcore reports bad input this way).
#define CHECK_THROWS(stmt)                                                          \
    do {                                                                            \
        bool sig_threw_ = false;                                                    \
        try { stmt; } catch (const std::exception&) { sig_threw_ = true; }          \
        if (!sig_threw_)                                                            \
            throw ::sigtest::Failure{std::string(__FILE__) + ":" +                  \
                                     std::to_string(__LINE__) + ": expected throw from " #stmt}; \
    } while (0)

#define TEST_MAIN() \
    int main() { return ::sigtest::run_all(); }
