/**
 * @file TestSupport.hpp
 * @brief Fournit le mini-harnais commun aux tests STGS sans framework externe.
 */
#pragma once

#include "stgs/TelemetryFrame.hpp"

#include <cmath>
#include <cstdint>
#include <exception>
#include <functional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#define ASSERT_TRUE(expr)                                                        \
    do                                                                           \
    {                                                                            \
        if (!(expr))                                                             \
        {                                                                        \
            throw std::runtime_error(std::string("assertion failed: ") + #expr); \
        }                                                                        \
    } while (false)

#define ASSERT_EQ(a, b)                                                                     \
    do                                                                                      \
    {                                                                                       \
        const auto lhs = (a);                                                               \
        const auto rhs = (b);                                                               \
        if (!(lhs == rhs))                                                                  \
        {                                                                                   \
            throw std::runtime_error(std::string("assertion failed: ") + #a + " == " + #b); \
        }                                                                                   \
    } while (false)

namespace stgs::test
{

    using TestRegistry = std::vector<std::pair<std::string, std::function<void()>>>;

    inline void assertNear(double actual, double expected, double tolerance, const char *label)
    {
        if (std::fabs(actual - expected) > tolerance)
        {
            throw std::runtime_error(std::string("assertion failed: ") + label);
        }
    }

    template <typename Fn>
    void assertThrows(Fn &&fn)
    {
        bool thrown = false;
        try
        {
            fn();
        }
        catch (const std::exception &)
        {
            thrown = true;
        }
        ASSERT_TRUE(thrown);
    }

    TelemetryFrame sampleFrame();
    std::uint16_t ephemeralPort(int socketType);
    int connectLoopback(std::uint16_t port);
    void sendExact(int fd, std::span<const std::uint8_t> bytes);
    void rethrowThreadError(const std::exception_ptr &error);

    void registerCodecTests(TestRegistry &tests);
    void registerApplicationSignalTests(TestRegistry &tests);
    void registerReplayHealthTests(TestRegistry &tests);
    void registerNetworkTests(TestRegistry &tests);

} // namespace stgs::test
