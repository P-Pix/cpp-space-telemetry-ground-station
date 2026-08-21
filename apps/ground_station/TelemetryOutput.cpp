/**
 * @file TelemetryOutput.cpp
 * @brief Implémente l'export CSV/JSON de la station sol.
 */
#include "TelemetryOutput.hpp"

#include "stgs/FrameCodec.hpp"

#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace stgs::app::ground_station
{
    namespace
    {
        // JSON impose l'échappement des codes de contrôle C0 U+0000..U+001F sous forme \uXXXX.
        inline constexpr unsigned char JsonControlUpperExclusive = 0x20U;
        inline constexpr int JsonUnicodeEscapeHexWidth = 4;
    } // namespace

    TelemetryOutput::TelemetryOutput(const std::filesystem::path &path, OutputFormat format)
        : out_(path, std::ios::out | std::ios::trunc), format_(format)
    {
        if (!out_)
        {
            throw std::runtime_error("failed to open output file: " + path.string());
        }
        if (format_ == OutputFormat::Json)
        {
            writeJsonHeader();
        }
        else
        {
            writeCsvHeader();
        }
    }

    void TelemetryOutput::write(const TelemetryFrame &frame)
    {
        if (format_ == OutputFormat::Json)
        {
            writeFrameJson(frame);
        }
        else
        {
            writeFrameCsv(frame);
        }
        if (!out_)
        {
            throw std::runtime_error("failed while writing telemetry output");
        }
    }

    void TelemetryOutput::finish()
    {
        if (finished_)
        {
            return;
        }
        if (format_ == OutputFormat::Json)
        {
            out_ << "\n]\n";
        }
        out_.flush();
        if (!out_)
        {
            throw std::runtime_error("failed while flushing telemetry output");
        }
        finished_ = true;
    }

    std::string TelemetryOutput::csvEscape(const std::string &value)
    {
        std::string out = "\"";
        for (const char ch : value)
        {
            if (ch == '"')
            {
                out += "\"\"";
            }
            else
            {
                out += ch;
            }
        }
        out += '"';
        return out;
    }

    std::string TelemetryOutput::jsonEscape(const std::string &value)
    {
        std::ostringstream out;
        for (const char raw : value)
        {
            const auto ch = static_cast<unsigned char>(raw);
            switch (ch)
            {
            case '"':
                out << "\\\"";
                break;
            case '\\':
                out << "\\\\";
                break;
            case '\b':
                out << "\\b";
                break;
            case '\f':
                out << "\\f";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                if (ch < JsonControlUpperExclusive)
                {
                    out << "\\u" << std::hex << std::setw(JsonUnicodeEscapeHexWidth) << std::setfill('0')
                        << static_cast<unsigned int>(ch) << std::dec;
                }
                else
                {
                    out << static_cast<char>(ch);
                }
                break;
            }
        }
        return out.str();
    }

    void TelemetryOutput::writeCsvHeader()
    {
        out_ << "timestamp_ms,satellite_id,temperature_c,battery_percent,status,payload_len,payload_hex\n";
    }

    void TelemetryOutput::writeFrameCsv(const TelemetryFrame &frame)
    {
        out_ << frame.timestampMs << ','
             << frame.satelliteId << ','
             << std::setprecision(std::numeric_limits<float>::max_digits10) << frame.temperatureC << ','
             << static_cast<unsigned int>(frame.batteryPercent) << ','
             << statusToString(frame.status) << ','
             << frame.payload.size() << ','
             << csvEscape(payloadToHex(frame.payload, frame.payload.size())) << '\n';
    }

    void TelemetryOutput::writeJsonHeader()
    {
        out_ << "[\n";
    }

    void TelemetryOutput::writeFrameJson(const TelemetryFrame &frame)
    {
        if (!firstJsonFrame_)
        {
            out_ << ",\n";
        }
        out_ << "  {"
             << "\"timestamp_ms\":" << frame.timestampMs << ','
             << "\"satellite_id\":" << frame.satelliteId << ','
             << "\"temperature_c\":"
             << std::setprecision(std::numeric_limits<float>::max_digits10) << frame.temperatureC << ','
             << "\"battery_percent\":" << static_cast<unsigned int>(frame.batteryPercent) << ','
             << "\"status\":\"" << jsonEscape(statusToString(frame.status)) << "\","
             << "\"payload_len\":" << frame.payload.size() << ','
             << "\"payload_hex\":\"" << jsonEscape(payloadToHex(frame.payload, frame.payload.size())) << "\""
             << '}';
        firstJsonFrame_ = false;
    }

} // namespace stgs::app::ground_station
