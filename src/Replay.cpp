/**
 * @file Replay.cpp
 * @brief Implémente le conteneur binaire de replay STGF.
 *
 * Le fichier commence par le magic ASCII `STGF` suivi d'une version u32 big-endian. Chaque entrée
 * contient ensuite une longueur u32 big-endian et les octets complets d'une trame STGS, CRC inclus.
 * Le lecteur valide les bornes avant allocation afin de rejeter les captures tronquées ou hostiles.
 */

#include "stgs/Replay.hpp"

#include "stgs/ByteUtils.hpp"

#include <array>
#include <stdexcept>

namespace stgs
{
    namespace
    {

        constexpr std::array<std::uint8_t, U32WireSize> ReplayMagic{'S', 'T', 'G', 'F'};
        constexpr std::uint32_t ReplayFormatVersion = 1U;
        constexpr std::size_t ReplayLengthFieldSize = U32WireSize;

        ByteVector makeFileHeader()
        {
            ByteVector header(ReplayMagic.begin(), ReplayMagic.end());
            detail::appendU32BE(header, ReplayFormatVersion);
            return header;
        }

        const ByteVector FileHeader = makeFileHeader();

        /**
         * @brief Lit exactement `size` octets ou signale un fichier tronqué.
         *
         * `std::istream::read` peut atteindre EOF avant la taille demandée. Le caller n'a donc jamais
         * à raisonner sur un buffer partiellement initialisé : une lecture courte devient une exception.
         */
        void readExact(std::ifstream &in, std::uint8_t *data, std::size_t size)
        {
            in.read(reinterpret_cast<char *>(data), static_cast<std::streamsize>(size));
            if (in.gcount() != static_cast<std::streamsize>(size))
            {
                throw std::runtime_error("unexpected end of replay file");
            }
        }

    } // namespace

    FrameFileWriter::FrameFileWriter(const std::filesystem::path &path)
    {
        out_.open(path, std::ios::binary | std::ios::out | std::ios::trunc);
        if (!out_)
        {
            throw std::runtime_error("failed to open replay output file: " + path.string());
        }
        out_.write(reinterpret_cast<const char *>(FileHeader.data()),
                   static_cast<std::streamsize>(FileHeader.size()));
        if (!out_)
        {
            throw std::runtime_error("failed while writing STGF replay header");
        }
    }

    FrameFileWriter::~FrameFileWriter()
    {
        if (out_)
        {
            out_.flush();
        }
    }

    void FrameFileWriter::writeFrame(std::span<const std::uint8_t> frameBytes)
    {
        if (frameBytes.size() < MinFrameSize || frameBytes.size() > MaxFrameSize)
        {
            throw std::runtime_error("refusing to write replay frame outside STGS frame size bounds");
        }

        ByteVector length;
        length.reserve(ReplayLengthFieldSize);
        detail::appendU32BE(length, static_cast<std::uint32_t>(frameBytes.size()));
        out_.write(reinterpret_cast<const char *>(length.data()), static_cast<std::streamsize>(length.size()));
        out_.write(reinterpret_cast<const char *>(frameBytes.data()),
                   static_cast<std::streamsize>(frameBytes.size()));
        if (!out_)
        {
            throw std::runtime_error("failed while writing replay frame");
        }
    }

    void FrameFileWriter::flush()
    {
        out_.flush();
        if (!out_)
        {
            throw std::runtime_error("failed while flushing replay file");
        }
    }

    FrameFileReader::FrameFileReader(const std::filesystem::path &path)
    {
        in_.open(path, std::ios::binary | std::ios::in);
        if (!in_)
        {
            throw std::runtime_error("failed to open replay input file: " + path.string());
        }

        ByteVector header(FileHeader.size(), 0U);
        readExact(in_, header.data(), header.size());
        if (header != FileHeader)
        {
            throw std::runtime_error("invalid or unsupported STGF replay file header");
        }
    }

    /**
     * @brief Lit une entrée STGF en refusant les longueurs hors bornes avant allocation.
     * @return Trame brute suivante ou std::nullopt à la fin normale du fichier.
     * @throws std::runtime_error Si la longueur ou le contenu sont tronqués/incohérents.
     */
    std::optional<ByteVector> FrameFileReader::readNext()
    {
        std::array<std::uint8_t, ReplayLengthFieldSize> lengthBytes{};
        in_.read(reinterpret_cast<char *>(lengthBytes.data()),
                 static_cast<std::streamsize>(lengthBytes.size()));
        const auto bytesRead = in_.gcount();
        if (bytesRead == 0 && in_.eof())
        {
            return std::nullopt;
        }
        if (bytesRead != static_cast<std::streamsize>(lengthBytes.size()))
        {
            throw std::runtime_error("truncated frame length in replay file");
        }

        const auto length = detail::readU32BE(lengthBytes, 0U);
        if (length < MinFrameSize || length > MaxFrameSize)
        {
            throw std::runtime_error("invalid replay frame length");
        }

        ByteVector frame(length);
        readExact(in_, frame.data(), frame.size());
        return frame;
    }

} // namespace stgs
