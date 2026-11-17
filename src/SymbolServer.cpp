#include <MemPDB/MemPDB.hpp>
#include "Downloader.hpp"

#include <algorithm>

namespace MemPDB
{
    SymbolServer::SymbolServer(Config config)
        : config_(std::move(config))
    {
        while (!config_.SymbolServer.empty() &&
               config_.SymbolServer.back() == '/')
        {
            config_.SymbolServer.pop_back();
        }
    }

    std::vector<std::byte> SymbolServer::Download(std::string_view pdbName,
                                                   std::string_view guidAge) const
    {
        std::string url;
        url.reserve(config_.SymbolServer.size() + 1 +
                    pdbName.size() + 1 +
                    guidAge.size() + 1 +
                    pdbName.size());

        url  = config_.SymbolServer;
        url += '/';
        url += pdbName;
        url += '/';
        url += guidAge;
        url += '/';
        url += pdbName;

        return detail::Download(url);
    }
}
