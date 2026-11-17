#pragma once

#include <cstddef>
#include <cstring>
#include <span>
#include <string_view>
#include <type_traits>

#include <MemPDB/MemPDB.hpp>

namespace MemPDB::detail
{
    class BinaryReader
    {
    public:
        explicit BinaryReader(std::span<const std::byte> data) noexcept
            : data_(data), pos_(0) {}

        template<typename T>
        T Read()
        {
            static_assert(std::is_trivially_copyable_v<T>);
            if (pos_ + sizeof(T) > data_.size())
                throw Error("BinaryReader: read out of bounds");
            T val;
            std::memcpy(&val, data_.data() + pos_, sizeof(T));
            pos_ += sizeof(T);
            return val;
        }

        template<typename T>
        T Peek() const
        {
            static_assert(std::is_trivially_copyable_v<T>);
            if (pos_ + sizeof(T) > data_.size())
                throw Error("BinaryReader: peek out of bounds");
            T val;
            std::memcpy(&val, data_.data() + pos_, sizeof(T));
            return val;
        }

        std::span<const std::byte> ReadBytes(std::size_t n)
        {
            if (pos_ + n > data_.size())
                throw Error("BinaryReader: ReadBytes out of bounds");
            auto s = data_.subspan(pos_, n);
            pos_ += n;
            return s;
        }

        std::string_view ReadCString()
        {
            const auto* base = reinterpret_cast<const char*>(data_.data());
            std::size_t start = pos_;
            while (pos_ < data_.size())
            {
                if (static_cast<char>(data_[pos_]) == '\0')
                {
                    std::string_view sv(base + start, pos_ - start);
                    ++pos_;
                    return sv;
                }
                ++pos_;
            }
            throw Error("BinaryReader: unterminated string");
        }

        // Like ReadCString but never throws: an unterminated string yields the
        // remaining bytes. Used by tolerant type/field-list decoding.
        std::string_view ReadCStringSafe() noexcept
        {
            const auto* base = reinterpret_cast<const char*>(data_.data());
            const std::size_t start = pos_;
            while (pos_ < data_.size() && static_cast<char>(data_[pos_]) != '\0')
                ++pos_;
            std::string_view sv(base + start, pos_ - start);
            if (pos_ < data_.size()) ++pos_; // consume terminator if present
            return sv;
        }

        void Skip(std::size_t n)
        {
            if (pos_ + n > data_.size())
                throw Error("BinaryReader: skip out of bounds");
            pos_ += n;
        }

        void Seek(std::size_t offset)
        {
            if (offset > data_.size())
                throw Error("BinaryReader: seek out of bounds");
            pos_ = offset;
        }

        std::size_t Offset()    const noexcept { return pos_; }
        std::size_t Remaining() const noexcept { return data_.size() - pos_; }
        bool        AtEnd()     const noexcept { return pos_ >= data_.size(); }

        std::span<const std::byte> View() const noexcept { return data_; }

        std::span<const std::byte> Subspan(std::size_t offset, std::size_t len) const
        {
            if (offset + len > data_.size())
                throw Error("BinaryReader: subspan out of bounds");
            return data_.subspan(offset, len);
        }

    private:
        std::span<const std::byte> data_;
        std::size_t                pos_;
    };
}
