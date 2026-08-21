/**
 * @file port_check.cpp
 * @brief Fournit un diagnostic CLI borné des listeners TCP présents sur le loopback.
 *
 * L'outil parcourt une petite plage locale, tente une connexion non bloquante sur chaque port,
 * mesure la latence et poursuit systématiquement jusqu'au dernier candidat afin de produire un
 * rapport complet. OPEN signifie "listener TCP joignable" et non "service STGS authentifié".
 */

#include "stgs/CliParsing.hpp"
#include "stgs/PortDiagnostics.hpp"
#include "stgs/TerminalUi.hpp"

#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace
{

    // Plage courte choisie uniquement pour la démonstration locale ; elle reste surchargeable en CLI.
    inline constexpr std::uint16_t DefaultDiagnosticFirstPort = 9000U;
    inline constexpr std::uint16_t DefaultDiagnosticLastPort = 9010U;

    struct Options
    {
        std::string host = "127.0.0.1";
        stgs::PortRange ports{DefaultDiagnosticFirstPort, DefaultDiagnosticLastPort};
        std::chrono::milliseconds timeout{stgs::DefaultPortProbeTimeout};
        bool color = true;
    };

    void printUsage()
    {
        std::cout << R"(STGS local TCP port diagnostics

Usage:
  stgs_port_check [--host 127.0.0.1] [--ports 9000:9010] [--timeout-ms 200]

Options:
  --host <ipv4>          Loopback target only. Default 127.0.0.1.
  --ports <a:b>          Inclusive range, at most 256 ports. Default 9000:9010.
  --timeout-ms <ms>      Timeout per port, 1..60000. Default 200.
  --no-color             Disable ANSI colors.
  --help                 Show this help.
)";
    }

    std::string requireValue(int &index, int argc, char **argv)
    {
        if (index + 1 >= argc)
        {
            throw std::runtime_error(std::string("missing value after ") + argv[index]);
        }
        ++index;
        return argv[index];
    }

    /**
     * @brief Parse un diagnostic borné et refuse toute cible hors loopback.
     *
     * La restriction est appliquée avant le premier socket probe afin que cet outil reste un
     * diagnostic de démonstration locale et ne puisse pas dériver en balayage réseau généraliste.
     */
    Options parseArgs(int argc, char **argv)
    {
        Options options;
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];
            if (arg == "--help" || arg == "-h")
            {
                printUsage();
                std::exit(0);
            }
            if (arg == "--host")
            {
                options.host = requireValue(i, argc, argv);
            }
            else if (arg == "--ports")
            {
                options.ports = stgs::parsePortRange(requireValue(i, argc, argv), "--ports");
            }
            else if (arg == "--timeout-ms")
            {
                const auto timeout = stgs::parseUnsigned<unsigned int>(
                    requireValue(i, argc, argv), "--timeout-ms", 1U,
                    static_cast<unsigned int>(stgs::MaximumPortProbeTimeout.count()));
                options.timeout = std::chrono::milliseconds(timeout);
            }
            else if (arg == "--no-color")
            {
                options.color = false;
            }
            else
            {
                throw std::runtime_error("unknown option: " + arg);
            }
        }
        if (!stgs::isLoopbackIpv4(options.host))
        {
            throw std::runtime_error("--host is intentionally restricted to IPv4 loopback");
        }
        if (options.ports.count() > stgs::MaxDiagnosticPortCount)
        {
            throw std::runtime_error("--ports exceeds MaxDiagnosticPortCount");
        }
        return options;
    }

} // namespace

int main(int argc, char **argv)
{
    try
    {
        const auto options = parseArgs(argc, argv);
        stgs::TerminalUi ui(options.color);
        ui.section("STGS PORT CHECK");
        ui.keyValue("target", options.host);
        ui.keyValue("range", std::to_string(options.ports.first) + ":" + std::to_string(options.ports.last));
        ui.keyValue("timeout", std::to_string(options.timeout.count()) + " ms / port");

        const auto results = stgs::scanTcpPorts(options.host, options.ports, options.timeout);
        std::size_t openCount = 0U;
        ui.section("REPORT");
        for (const auto &result : results)
        {
            if (result.state == stgs::TcpPortState::Open)
            {
                ++openCount;
            }
            std::ostringstream latency;
            latency << std::fixed << std::setprecision(2) << result.latencyMs << " ms";
            stgs::TerminalStatus visualStatus = stgs::TerminalStatus::Info;
            switch (result.state)
            {
            case stgs::TcpPortState::Open:
                visualStatus = stgs::TerminalStatus::Success;
                break;
            case stgs::TcpPortState::Closed:
                visualStatus = stgs::TerminalStatus::Info;
                break;
            case stgs::TcpPortState::Timeout:
                visualStatus = stgs::TerminalStatus::Warning;
                break;
            case stgs::TcpPortState::Error:
                visualStatus = stgs::TerminalStatus::Error;
                break;
            }
            ui.statusLine(visualStatus,
                          "port " + std::to_string(result.port) + " / " +
                              stgs::tcpPortStateToString(result.state),
                          latency.str() + " / " + result.detail);
        }

        ui.section("SUMMARY");
        ui.keyValue("ports tested", std::to_string(results.size()));
        ui.keyValue("TCP listeners", std::to_string(openCount));
        ui.keyValue("interpretation", "OPEN = TCP listener reachable; service identity is not inferred");
        return 0;
    }
    catch (const std::exception &ex)
    {
        std::cerr << "error: " << ex.what() << '\n';
        return 1;
    }
}
