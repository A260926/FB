#include "MSFReader.hpp"
#include "BinaryReader.hpp"
#include "PDBFormat.hpp"

#include <cstring>
#include <stdexcept>

namespace MemPDB::detail
{
    MSFReader::MSFReader(std::span<const std::byte> data)
        : raw_(data)
    {
        if (data.size() < sizeof(MSFSuperBlock))
            throw Error("MSF: file too small for superblock");

        MSFSuperBlock sb;
        std::memcpy(&sb, data.data(), sizeof(sb));

        if (std::memcmp(sb.Magic, kMSFMagic, 32) != 0)
            throw Error("MSF: invalid magic");

        if (sb.BlockSize == 0 || (sb.BlockSize & (sb.BlockSize - 1)) != 0)
            throw Error("MSF: invalid block size");

        blockSize_ = sb.BlockSize;

        const uint64_t totalSize = static_cast<uint64_t>(sb.NumBlocks) * sb.BlockSize;
        if (totalSize > data.size())
            throw Error("MSF: NumBlocks exceeds file size");

        auto blockAt = [&](uint32_t idx) -> std::span<const std::byte>
        {
            if (idx >= sb.NumBlocks)
                throw Error("MSF: block index out of range");
            const uint64_t off = static_cast<uint64_t>(idx) * sb.BlockSize;
            return data.subspan(static_cast<std::size_t>(off), sb.BlockSize);
        };

        const uint32_t numDirBlocks =
            (sb.NumDirectoryBytes + sb.BlockSize - 1) / sb.BlockSize;

        auto mapBlock = blockAt(sb.BlockMapAddr);
        if (numDirBlocks * sizeof(uint32_t) > mapBlock.size())
            throw Error("MSF: block map block too small");

        std::vector<uint32_t> dirBlockNums(numDirBlocks);
        std::memcpy(dirBlockNums.data(), mapBlock.data(),
                    numDirBlocks * sizeof(uint32_t));

        std::vector<std::byte> dirData(sb.NumDirectoryBytes);
        {
            std::size_t written = 0;
            for (uint32_t i = 0; i < numDirBlocks; ++i)
            {
                auto blk = blockAt(dirBlockNums[i]);
                const std::size_t chunk =
                    std::min(static_cast<std::size_t>(sb.BlockSize),
                             dirData.size() - written);
                std::memcpy(dirData.data() + written, blk.data(), chunk);
                written += chunk;
            }
        }

        BinaryReader dir(dirData);

        const uint32_t numStreams = dir.Read<uint32_t>();

        std::vector<uint32_t> sizes(numStreams);
        for (uint32_t i = 0; i < numStreams; ++i)
            sizes[i] = dir.Read<uint32_t>();

        streams_.reserve(numStreams);
        blockMap_.clear();

        for (uint32_t i = 0; i < numStreams; ++i)
        {
            StreamEntry e{};
            e.Size        = sizes[i];
            e.BlockOffset = static_cast<uint32_t>(blockMap_.size());

            if (e.Size != kStreamDeleted && e.Size > 0)
            {
                e.BlockCount =
                    (e.Size + sb.BlockSize - 1) / sb.BlockSize;

                for (uint32_t b = 0; b < e.BlockCount; ++b)
                    blockMap_.push_back(dir.Read<uint32_t>());
            }
            else
            {
                e.BlockCount = 0;
            }

            streams_.push_back(e);
        }
    }

    uint32_t MSFReader::StreamCount() const noexcept
    {
        return static_cast<uint32_t>(streams_.size());
    }

    std::optional<std::span<const std::byte>>
    MSFReader::TryGetStreamView(uint32_t index) const noexcept
    {
        if (index >= streams_.size()) return std::nullopt;
        const auto& e = streams_[index];
        if (e.Size == kStreamDeleted || e.Size == 0 || e.BlockCount == 0)
            return std::nullopt;

        const uint32_t firstBlock = blockMap_[e.BlockOffset];
        for (uint32_t i = 1; i < e.BlockCount; ++i)
        {
            if (blockMap_[e.BlockOffset + i] != firstBlock + i)
                return std::nullopt;
        }

        const uint64_t off = static_cast<uint64_t>(firstBlock) * blockSize_;
        if (off + e.Size > raw_.size()) return std::nullopt;

        return raw_.subspan(static_cast<std::size_t>(off), e.Size);
    }

    std::vector<std::byte> MSFReader::GetStream(uint32_t index) const
    {
        if (index >= streams_.size())
            throw Error("MSF: stream index out of range");

        const auto& e = streams_[index];
        if (e.Size == kStreamDeleted || e.Size == 0)
            return {};

        return ReadBlocks(e.BlockOffset, e.BlockCount, e.Size);
    }

    std::vector<std::byte> MSFReader::ReadBlocks(uint32_t blockOffset,
                                                  uint32_t blockCount,
                                                  uint32_t byteCount) const
    {
        std::vector<std::byte> out(byteCount);
        std::size_t written = 0;

        for (uint32_t i = 0; i < blockCount; ++i)
        {
            const uint32_t blockIdx = blockMap_[blockOffset + i];
            if (blockIdx >= raw_.size() / blockSize_)
                throw Error("MSF: stream block index out of range");

            const uint64_t off = static_cast<uint64_t>(blockIdx) * blockSize_;
            const std::size_t chunk =
                std::min(static_cast<std::size_t>(blockSize_),
                         static_cast<std::size_t>(byteCount) - written);

            std::memcpy(out.data() + written,
                        raw_.data() + static_cast<std::size_t>(off),
                        chunk);
            written += chunk;
        }

        return out;
    }
}
