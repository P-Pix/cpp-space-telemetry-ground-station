/**
 * @file TerminalUi.cpp
 * @brief Implémente un rendu ANSI/Unicode optionnel pour la démonstration en terminal.
 */

#include "stgs/TerminalUi.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <syncstream>
#include <sys/ioctl.h>
#include <unistd.h>

namespace stgs {
namespace {

// Codes SGR normalisés par ECMA-48 / ISO 6429. Ils ne représentent pas des valeurs métier.
constexpr std::string_view SgrReset = "0";
constexpr std::string_view SgrBold = "1";
constexpr std::string_view SgrDim = "2";
constexpr std::string_view SgrRed = "31";
constexpr std::string_view SgrGreen = "32";
constexpr std::string_view SgrYellow = "33";
constexpr std::string_view SgrMagenta = "35";
constexpr std::string_view SgrCyan = "36";

// Paramètres de mise en page du terminal. Ils n'ont aucun rôle protocolaire ou métier.
inline constexpr std::size_t FallbackTerminalWidth = 100U;
inline constexpr std::size_t SectionDecorationReserve = 12U;
inline constexpr std::size_t MinimumSectionRuleLength = 20U;
inline constexpr std::size_t MaximumSectionRuleLength = 72U;
inline constexpr std::size_t SignalLabelReserve = 28U;
inline constexpr std::size_t SignalFallbackColumns = 40U;
inline constexpr std::size_t MinimumSignalColumns = 24U;
inline constexpr std::size_t MaximumSignalColumns = 96U;
inline constexpr std::size_t SparklineLevelCount = 8U;
inline constexpr double FlatSignalNormalizedLevel = 0.5;
inline constexpr unsigned char AsciiEscape = 0x1BU;
inline constexpr unsigned char AsciiControlUpperExclusive = 0x20U;
inline constexpr unsigned char AsciiDelete = 0x7FU;
inline constexpr int EscapedByteHexWidth = 2;
inline constexpr int DefaultMetricPrecision = 3;
inline constexpr int SignalMetadataPrecision = 2;

bool terminalLooksUnicodeCapable() {
    const char* locale = std::getenv("LC_ALL");
    if (locale == nullptr || *locale == '\0') {
        locale = std::getenv("LC_CTYPE");
    }
    if (locale == nullptr || *locale == '\0') {
        locale = std::getenv("LANG");
    }
    if (locale == nullptr) {
        return true;
    }
    const std::string value(locale);
    return value.find("UTF-8") != std::string::npos || value.find("utf8") != std::string::npos ||
           value.find("UTF8") != std::string::npos;
}

std::string formatNumber(double value, int precision = DefaultMetricPrecision) {
    std::ostringstream out;
    if (std::isinf(value)) {
        return value > 0.0 ? "+inf" : "-inf";
    }
    out << std::fixed << std::setprecision(precision) << value;
    return out.str();
}

} // namespace

TerminalUi::TerminalUi(bool allowColor) {
    interactive_ = ::isatty(STDOUT_FILENO) == 1;
    const char* term = std::getenv("TERM");
    const bool dumbTerminal = term != nullptr && std::string_view(term) == "dumb";
    const bool noColor = std::getenv("NO_COLOR") != nullptr;
    colorsEnabled_ = interactive_ && allowColor && !dumbTerminal && !noColor;
    unicodeEnabled_ = interactive_ && terminalLooksUnicodeCapable();
}

std::size_t TerminalUi::width() const noexcept {
    winsize size{};
    if (interactive_ && ::ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0U) {
        return static_cast<std::size_t>(size.ws_col);
    }
    return FallbackTerminalWidth;
}

std::string TerminalUi::style(std::string_view text, std::string_view sgrCode) const {
    if (!colorsEnabled_) {
        return std::string(text);
    }
    return "\x1b[" + std::string(sgrCode) + "m" + std::string(text) + "\x1b[" + std::string(SgrReset) + "m";
}

void TerminalUi::section(std::string_view title) const {
    std::osyncstream out(std::cout);
    const std::size_t available = width() > SectionDecorationReserve
                                      ? width() - SectionDecorationReserve
                                      : MinimumSectionRuleLength;
    const std::size_t ruleLength = std::min<std::size_t>(available, MaximumSectionRuleLength);
    const char rule = '-';
    out << '\n' << style("[ " + std::string(title) + " ]", SgrBold) << '\n'
        << style(std::string(ruleLength, rule), SgrDim) << '\n';
}

void TerminalUi::keyValue(std::string_view key, std::string_view value) const {
    std::osyncstream out(std::cout);
    out << "  " << style(std::string(key), SgrCyan) << ": " << value << '\n';
}

void TerminalUi::statusLine(TerminalStatus status,
                            std::string_view subject,
                            std::string_view detail) const {
    std::string_view label = "INFO";
    std::string_view color = SgrCyan;
    switch (status) {
    case TerminalStatus::Info:
        break;
    case TerminalStatus::Success:
        label = "OK";
        color = SgrGreen;
        break;
    case TerminalStatus::Warning:
        label = "WARN";
        color = SgrYellow;
        break;
    case TerminalStatus::Error:
        label = "ERR";
        color = SgrRed;
        break;
    }

    std::osyncstream out(std::cout);
    out << "  " << style("[" + std::string(label) + "]", color)
        << " " << subject;
    if (!detail.empty()) {
        out << " - " << detail;
    }
    out << '\n';
}

std::string TerminalUi::sanitizeRemoteText(std::string_view text) {
    std::ostringstream out;
    for (const char raw : text) {
        const auto ch = static_cast<unsigned char>(raw);
        switch (ch) {
        case '\n':
            out << "\\n";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\t':
            out << "\\t";
            break;
        case AsciiEscape: // ESC : neutralise explicitement les séquences de contrôle ANSI distantes.
            out << "\\x1B";
            break;
        default:
            if (ch < AsciiControlUpperExclusive || ch == AsciiDelete) {
                out << "\\x" << std::uppercase << std::hex << std::setw(EscapedByteHexWidth) << std::setfill('0')
                    << static_cast<unsigned int>(ch) << std::dec << std::nouppercase;
            } else {
                out << static_cast<char>(ch);
            }
            break;
        }
    }
    return out.str();
}

void TerminalUi::receivedMessage(std::uint16_t satelliteId,
                                 std::uint32_t sequence,
                                 std::string_view text) const {
    std::osyncstream out(std::cout);
    out << style("[MESSAGE]", SgrMagenta)
        << " sat=" << satelliteId
        << " seq=" << sequence
        << " text=\"" << sanitizeRemoteText(text) << "\"\n";
}

std::string TerminalUi::sparkline(std::span<const float> samples, std::size_t columns, bool unicode) {
    if (samples.empty() || columns == 0U) {
        return {};
    }

    const auto [minIt, maxIt] = std::minmax_element(samples.begin(), samples.end());
    const double minimum = static_cast<double>(*minIt);
    const double maximum = static_cast<double>(*maxIt);
    const double range = maximum - minimum;

    const std::array<std::string_view, SparklineLevelCount> unicodeLevels{"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    constexpr std::string_view asciiLevels = "._-~=+*#";

    std::string out;
    const std::size_t outputColumns = std::min(columns, samples.size());
    for (std::size_t column = 0; column < outputColumns; ++column) {
        const std::size_t begin = column * samples.size() / outputColumns;
        const std::size_t end = std::max(begin + 1U, (column + 1U) * samples.size() / outputColumns);
        double average = 0.0;
        for (std::size_t i = begin; i < end; ++i) {
            average += static_cast<double>(samples[i]);
        }
        average /= static_cast<double>(end - begin);
        const double normalized = range <= std::numeric_limits<double>::epsilon()
                                      ? FlatSignalNormalizedLevel
                                      : std::clamp((average - minimum) / range, 0.0, 1.0);
        const auto level = static_cast<std::size_t>(
            std::lround(normalized * static_cast<double>(SparklineLevelCount - 1U)));
        if (unicode) {
            out += unicodeLevels[level];
        } else {
            out.push_back(asciiLevels[level]);
        }
    }
    return out;
}

void TerminalUi::receivedSignal(std::uint16_t satelliteId,
                                std::uint16_t sampleRateHz,
                                float frequencyHz,
                                float nominalAmplitude,
                                std::span<const float> raw,
                                std::span<const float> filtered,
                                const SignalMetrics& metrics,
                                SignalFilterMode filterMode) const {
    const std::size_t columns = std::clamp<std::size_t>(
        width() > SignalLabelReserve ? width() - SignalLabelReserve : SignalFallbackColumns,
        MinimumSignalColumns,
        MaximumSignalColumns);
    std::osyncstream out(std::cout);
    out << style("[SIGNAL]", SgrYellow)
        << " sat=" << satelliteId
        << " samples=" << raw.size()
        << " fs=" << sampleRateHz << "Hz"
        << " f=" << formatNumber(frequencyHz, SignalMetadataPrecision) << "Hz"
        << " nominal_amp=" << formatNumber(nominalAmplitude, SignalMetadataPrecision)
        << " filter=" << signalFilterModeToString(filterMode) << '\n';
    out << "  " << style("raw     ", SgrDim) << sparkline(raw, columns, unicodeEnabled_) << '\n';
    out << "  " << style("filtered", SgrGreen) << ' ' << sparkline(filtered, columns, unicodeEnabled_) << '\n';
    out << "  metrics: raw_rms=" << formatNumber(metrics.rawRms)
        << " filtered_rms=" << formatNumber(metrics.filteredRms)
        << " residual_noise_rms=" << formatNumber(metrics.residualNoiseRms)
        << " est_amp=" << formatNumber(metrics.estimatedAmplitude)
        << " est_snr_db=" << formatNumber(metrics.estimatedSnrDb, SignalMetadataPrecision) << '\n';
}

} // namespace stgs
