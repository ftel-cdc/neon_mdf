/**
 * @file test_common.h
 * @brief Minimal host test harness for the camera HAL tests. One translation unit per test program.
 */

#ifndef __TEST_COMMON_H__
#define __TEST_COMMON_H__

#include <stdio.h>
#include <string.h>

static int g_checks;
static int g_failures;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        g_checks++;                                                                                \
        if (!(condition)) {                                                                        \
            g_failures++;                                                                          \
            printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                        \
        }                                                                                          \
    } while (0)

#define CHECK_EQ(actual, expected)                                                                 \
    do {                                                                                           \
        unsigned long long a_ = (unsigned long long)(actual);                                      \
        unsigned long long e_ = (unsigned long long)(expected);                                    \
        g_checks++;                                                                                \
        if (a_ != e_) {                                                                            \
            g_failures++;                                                                          \
            printf("    FAIL %s:%d: %s == %s (got %llu, expected %llu)\n", __FILE__, __LINE__,    \
                   #actual, #expected, a_, e_);                                                    \
        }                                                                                          \
    } while (0)

#define RUN(test)                                                                                  \
    do {                                                                                           \
        int before_ = g_failures;                                                                  \
        test();                                                                                    \
        printf("%s %s\n", (g_failures == before_) ? "PASS" : "FAIL", #test);                       \
    } while (0)

#define TEST_REPORT()                                                                              \
    (printf("%d checks, %d failures\n", g_checks, g_failures), (g_failures == 0) ? 0 : 1)

#endif /* __TEST_COMMON_H__ */
