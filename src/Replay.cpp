#include "stgs/Replay.hpp"

#include "stgs/ByteUtils.hpp"

#include <array>
#include <stdexcept>

namespace stgs {
namespace {

constexpr std::array<std::uint8_t, 8> FileHeader{'S', 'T', 'G', 'F', 0, 0, 0, 1};

void readExact(std::ifstream& in, std::uint8_t* data, std::size_t size) {
    in.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(size));
    if (in.gcount() != static_cast<std::streamsize>(size)) {
        throw std::runtime_error("unexpected end of replay file");
    }
}

} // namespace

FrameFileWriter::FrameFileWriter(const std::filesystem::path& path) {
    out_.open(path, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!out_) {
        throw std::runtime_error("failed to open replay output file: " + path.string());
    }
    out_.write(reinterpret_cast<const char*>(FileHeader.data()), static_cast<std::streamsize>(FileHeader.size()));
}

FrameFileWriter::~FrameFileWriter() {
    if (out_) {
        out_.flush();
    }
}

void FrameFileWriter::writeFrame(std::span<const std::uint8_t> frameBytes) {
    if (frameBytes.size() > MaxFrameSize) {
        throw std::runtime_error("refusing to write frame larger than MaxFrameSize");
    }
    ByteVector length;
    detail::appendU32BE(length, static_cast<std::uint32_t>(frameBytes.size()));
    out_.write(reinterpret_cast<const char*>(length.data()), static_cast<std::streamsize>(length.size()));
    out_.write(reinterpret_cast<const char*>(frameBytes.data()), static_cast<std::streamsize>(frameBytes.size()));
    if (!out_) {
        throw std::runtime_error("failed while writing replay frame");
    }
}

FrameFileReader::FrameFileReader(const std::filesystem::path& path) {
    in_.open(path, std::ios::binary | std::ios::in);
    if (!in_) {
        throw std::runtime_error("failed to open replay input file: " + path.string());
    }

    std::array<std::uint8_t, FileHeader.size()> header{};
    readExact(in_, header.data(), header.size());
    if (header != FileHeader) {
        throw std::runtime_error("invalid replay file header; expected STGF v1");
    }
}

std::optional<ByteVector> FrameFileReader::readNext() {
    std::array<std::uint8_t, 4> lenBytes{};
    in_.read(reinterpret_cast<char*>(lenBytes.data()), static_cast<std::streamsize>(lenBytes.size()));
    const auto bytesRead = in_.gcount();
    if (bytesRead == 0 && in_.eof()) {
        return std::nullopt;
    }
    if (bytesRead != static_cast<std::streamsize>(lenBytes.size())) {
        throw std::runtime_error("truncated frame length in replay file");
    }

    const auto length = detail::readU32BE(lenBytes, 0U);
    if (length < MinFrameSize || length > MaxFrameSize) {
        throw std::runtime_error("invalid replay frame length");
    }

    ByteVector frame(length);
    readExact(in_, frame.data(), frame.size());
    return frame;
}

} // namespace stgs
