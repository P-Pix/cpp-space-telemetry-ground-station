#pragma once

#include "stgs/TelemetryFrame.hpp"

#include <filesystem>
#include <fstream>
#include <optional>
#include <span>

namespace stgs {

class FrameFileWriter {
public:
    explicit FrameFileWriter(const std::filesystem::path& path);
    ~FrameFileWriter();

    FrameFileWriter(const FrameFileWriter&) = delete;
    FrameFileWriter& operator=(const FrameFileWriter&) = delete;

    void writeFrame(std::span<const std::uint8_t> frameBytes);

private:
    std::ofstream out_;
};

class FrameFileReader {
public:
    explicit FrameFileReader(const std::filesystem::path& path);

    FrameFileReader(const FrameFileReader&) = delete;
    FrameFileReader& operator=(const FrameFileReader&) = delete;

    std::optional<ByteVector> readNext();

private:
    std::ifstream in_;
};

} // namespace stgs
