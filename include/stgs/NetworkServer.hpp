#pragma once

#include "stgs/FrameCodec.hpp"
#include "stgs/Logger.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace stgs {

enum class Transport {
    Udp,
    Tcp
};

struct NetworkConfig {
    Transport transport = Transport::Udp;
    std::string bindAddress = "0.0.0.0";
    std::uint16_t port = 9000;
    int pollTimeoutMs = 250;
};

class NetworkServer {
public:
    using FrameCallback = std::function<void(ByteVector)>;

    NetworkServer(NetworkConfig config, Logger& logger);
    ~NetworkServer();

    NetworkServer(const NetworkServer&) = delete;
    NetworkServer& operator=(const NetworkServer&) = delete;

    void run(FrameCallback callback, const std::atomic_bool& running);
    void stop();

private:
    void runUdp(FrameCallback& callback, const std::atomic_bool& running);
    void runTcp(FrameCallback& callback, const std::atomic_bool& running);
    int createBoundSocket(int socketType) const;
    void closeServerSocket() noexcept;

    NetworkConfig config_;
    Logger& logger_;
    int serverFd_ = -1;
    std::atomic_bool stopRequested_{false};
};

std::string transportToString(Transport transport);

} // namespace stgs
