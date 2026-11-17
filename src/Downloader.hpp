#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace MemPDB::detail
{
    struct ParsedURL
    {
        std::string scheme;
        std::string host;
        uint16_t    port;
        std::string path;
    };

    ParsedURL              ParseURL(std::string_view url);
    std::vector<std::byte> Download(std::string_view url);
}
