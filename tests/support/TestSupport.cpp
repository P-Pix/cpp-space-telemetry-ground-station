/**
 * @file TestSupport.cpp
 * @brief Implémente les fixtures et helpers réseau loopback partagés par les tests.
 */
#include "TestSupport.hpp"

#include <arpa/inet.h>
#include <cstddef>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace stgs::test
{

    TelemetryFrame sampleFrame()
    {
        TelemetryFrame frame;
        frame.satelliteId = 1337U;
        frame.timestampMs = 1'712'345'678'901ULL;
        frame.temperatureC = 18.75F;
        frame.batteryPercent = 87U;
        frame.status = Status::Warning;
        frame.payload = {0xDEU, 0xADU, 0xBEU, 0xEFU, 0x01U};
        return frame;
    }

    std::uint16_t ephemeralPort(int socketType)
    {
        const int fd = ::socket(AF_INET, socketType, 0);
        if (fd < 0)
        {
            throw std::runtime_error("failed to create ephemeral test socket");
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(0U);
        if (::bind(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) < 0)
        {
            ::close(fd);
            throw std::runtime_error("failed to bind ephemeral test socket");
        }
        socklen_t length = sizeof(address);
        if (::getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length) < 0)
        {
            ::close(fd);
            throw std::runtime_error("failed to inspect ephemeral test socket");
        }
        const auto port = ntohs(address.sin_port);
        ::close(fd);
        return port;
    }

    int connectLoopback(std::uint16_t port)
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
        {
            throw std::runtime_error("test socket() failed");
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        if (::connect(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) < 0)
        {
            ::close(fd);
            throw std::runtime_error("test connect() failed");
        }
        return fd;
    }

    void sendExact(int fd, std::span<const std::uint8_t> bytes)
    {
        std::size_t offset = 0U;
        while (offset < bytes.size())
        {
            const ssize_t count = ::send(fd,
                                         bytes.data() + static_cast<std::ptrdiff_t>(offset),
                                         bytes.size() - offset,
                                         MSG_NOSIGNAL);
            if (count <= 0)
            {
                throw std::runtime_error("test send() failed");
            }
            offset += static_cast<std::size_t>(count);
        }
    }

    void rethrowThreadError(const std::exception_ptr &error)
    {
        if (error != nullptr)
        {
            std::rethrow_exception(error);
        }
    }

} // namespace stgs::test
