#include "viewport_screenshot.h"

#include <algorithm>
#include <array>
#include <vector>

#include "runtime/core/error/error.h"
#include "runtime/platform/fileio/fileio.h"

namespace Incubator::Editor
{
    namespace
    {
        void appendBe32(std::vector<uint8_t>& out, uint32_t value)
        {
            out.push_back(static_cast<uint8_t>((value >> 24) & 0xFFu));
            out.push_back(static_cast<uint8_t>((value >> 16) & 0xFFu));
            out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
            out.push_back(static_cast<uint8_t>(value & 0xFFu));
        }

        uint32_t crc32Update(uint32_t crc, std::span<const uint8_t> bytes) noexcept
        {
            crc = ~crc;
            for (const uint8_t byte : bytes)
            {
                crc ^= byte;
                for (int bit = 0; bit < 8; ++bit)
                    crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
            }
            return ~crc;
        }

        void appendPngChunk(std::vector<uint8_t>& out, const char (&type)[5], std::span<const uint8_t> data)
        {
            appendBe32(out, static_cast<uint32_t>(data.size()));
            const std::array<uint8_t, 4> typeBytes{static_cast<uint8_t>(type[0]), static_cast<uint8_t>(type[1]),
                                                   static_cast<uint8_t>(type[2]), static_cast<uint8_t>(type[3])};
            out.insert(out.end(), typeBytes.begin(), typeBytes.end());
            out.insert(out.end(), data.begin(), data.end());
            uint32_t crc = crc32Update(0u, typeBytes);
            crc = crc32Update(crc, data);
            appendBe32(out, crc);
        }
    }  // namespace

    void writeRgba8Png(FileIO::FileIO& fileIO, const std::filesystem::path& path, uint32_t width, uint32_t height,
                       std::span<const uint8_t> rgba8)
    {
        if (path.empty())
            throw Error::Exception(Error::Code::IO, "Screenshot PNG output path must not be empty");
        if (width == 0u || height == 0u)
            throw Error::Exception(Error::Code::CONFIG, "Screenshot PNG extent must be positive");
        const uint64_t expectedPixels = static_cast<uint64_t>(width) * height * 4ull;
        if (rgba8.size() != expectedPixels)
            throw Error::Exception(Error::Code::CONFIG, "Screenshot RGBA8 payload size does not match extent");

        const size_t rowBytes = static_cast<size_t>(width) * 4u;
        const size_t rawScanlineBytes = static_cast<size_t>(height) * (rowBytes + 1u);

        // 先组装 PNG 每行首字节 filter=0 载荷，并同步累加 zlib Adler-32。
        std::vector<uint8_t> scanlines;
        scanlines.reserve(rawScanlineBytes);
        uint32_t adlerS1 = 1u;
        uint32_t adlerS2 = 0u;
        constexpr uint32_t ADLER_MOD = 65521u;
        for (uint32_t y = 0; y < height; ++y)
        {
            scanlines.push_back(0u);
            adlerS2 = (adlerS2 + adlerS1) % ADLER_MOD;
            const uint8_t* rowPtr = rgba8.data() + static_cast<size_t>(y) * rowBytes;
            scanlines.insert(scanlines.end(), rowPtr, rowPtr + rowBytes);
            for (size_t x = 0; x < rowBytes; ++x)
            {
                adlerS1 += rowPtr[x];
                if (adlerS1 >= ADLER_MOD)
                    adlerS1 -= ADLER_MOD;
                adlerS2 += adlerS1;
                if (adlerS2 >= ADLER_MOD)
                    adlerS2 -= ADLER_MOD;
            }
        }
        const uint32_t adler32 = (adlerS2 << 16) | adlerS1;

        // 构造标准 zlib/DEFLATE 无压缩存储块流，避免引入外部压缩库依赖。
        std::vector<uint8_t> idatPayload;
        idatPayload.reserve(scanlines.size() + (scanlines.size() / 65535u + 1u) * 5u + 6u);
        idatPayload.push_back(0x78u);
        idatPayload.push_back(0x01u);
        size_t offset = 0;
        while (offset < scanlines.size())
        {
            const uint16_t chunkLen = static_cast<uint16_t>(std::min<size_t>(65535u, scanlines.size() - offset));
            const bool isFinal = (offset + chunkLen) >= scanlines.size();
            idatPayload.push_back(isFinal ? 0x01u : 0x00u);
            idatPayload.push_back(static_cast<uint8_t>(chunkLen & 0xFFu));
            idatPayload.push_back(static_cast<uint8_t>((chunkLen >> 8) & 0xFFu));
            const uint16_t nlen = static_cast<uint16_t>(~chunkLen);
            idatPayload.push_back(static_cast<uint8_t>(nlen & 0xFFu));
            idatPayload.push_back(static_cast<uint8_t>((nlen >> 8) & 0xFFu));
            idatPayload.insert(idatPayload.end(), scanlines.begin() + static_cast<ptrdiff_t>(offset),
                               scanlines.begin() + static_cast<ptrdiff_t>(offset + chunkLen));
            offset += chunkLen;
        }
        appendBe32(idatPayload, adler32);

        std::vector<uint8_t> png;
        constexpr std::array<uint8_t, 8> PNG_SIGNATURE{137u, 80u, 78u, 71u, 13u, 10u, 26u, 10u};
        png.insert(png.end(), PNG_SIGNATURE.begin(), PNG_SIGNATURE.end());

        std::vector<uint8_t> ihdr;
        ihdr.reserve(13);
        appendBe32(ihdr, width);
        appendBe32(ihdr, height);
        ihdr.push_back(8u);   // bit depth
        ihdr.push_back(6u);   // color type RGBA
        ihdr.push_back(0u);   // compression DEFLATE
        ihdr.push_back(0u);   // filter
        ihdr.push_back(0u);   // interlace none
        appendPngChunk(png, "IHDR", ihdr);
        appendPngChunk(png, "IDAT", idatPayload);
        appendPngChunk(png, "IEND", {});

        auto handle =
            fileIO.open(path, FileIO::OpenMode::Write | FileIO::OpenMode::Create | FileIO::OpenMode::Truncate);
        if (!handle)
            throw Error::Exception(Error::Code::IO, fmt::format("Cannot open screenshot file '{}' for writing", path.string()));
        size_t written = 0;
        while (written < png.size())
        {
            const size_t n = handle->write(std::span<const uint8_t>(png.data() + written, png.size() - written));
            if (n == 0u)
                throw Error::Exception(Error::Code::IO, "Short write while writing PNG screenshot");
            written += n;
        }
        handle->flush();
    }
}  // namespace Incubator::Editor
