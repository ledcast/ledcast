#pragma once

#include <cstdio>

// Минимальный набор проверок: внешний фреймворк ради двух файлов тестов
// тянуть незачем, а ctest хватает ненулевого кода возврата.

namespace hosttest {

inline int failures = 0;
inline int checks = 0;

inline void record(bool ok, const char* expression, const char* file, int line) {
    ++checks;
    if (ok) {
        return;
    }
    ++failures;
    std::printf("FAIL %s:%d: %s\n", file, line, expression);
}

inline void record_eq(long long actual, long long expected, const char* expression,
                      const char* file, int line) {
    ++checks;
    if (actual == expected) {
        return;
    }
    ++failures;
    std::printf("FAIL %s:%d: %s — получено %lld, ожидалось %lld\n", file, line, expression, actual,
                expected);
}

inline int report(const char* suite) {
    std::printf("%s: проверок %d, провалов %d\n", suite, checks, failures);
    return failures == 0 ? 0 : 1;
}

}  // namespace hosttest

#define CHECK(expr) hosttest::record((expr), #expr, __FILE__, __LINE__)
#define CHECK_EQ(actual, expected) \
    hosttest::record_eq(static_cast<long long>(actual), static_cast<long long>(expected), \
                        #actual " == " #expected, __FILE__, __LINE__)
