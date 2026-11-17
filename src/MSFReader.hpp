#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace MemPDB::detail
{
    class MSFReader
    {
    public:
        explicit MSFReader(std::span<const std::byte> data);

        uint32_t StreamCount() const noexcept;

        std::optional<std::span<const std::byte>> TryGetStreamView(uint32_t index) const noexcept;
        std::vector<std::byte>                    GetStream(uint32_t index) const;

    private:
        struct StreamEntry
        {
            uint32_t Size;
            uint32_t BlockOffset;
            uint32_t BlockCount;
        };

        std::span<const std::byte>  raw_;
        uint32_t                    blockSize_;
        std::vector<uint32_t>       blockMap_;
        std::vector<StreamEntry>    streams_;

        std::vector<std::byte> ReadBlocks(uint32_t blockOffset,
                                          uint32_t blockCount,
                                          uint32_t byteCount) const;
    };
}
