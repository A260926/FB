#include "Downloader.hpp"

#include <MemPDB/MemPDB.hpp>

#include <algorithm>
#include <cctype>
#include <memory>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#if defined(_MSC_VER)
// MinGW/Clang GNU drivers rely on CMake's target_link_libraries(winhttp).
#pragma comment(lib, "winhttp.lib")
#endif
#else
#include <curl/curl.h>
#endif

namespace MemPDB::detail
{
    namespace
    {
#if defined(_WIN32)
        struct HINTERNETDeleter
        {
            void operator()(HINTERNET h) const noexcept
            {
                if (h) WinHttpCloseHandle(h);
            }
        };
        using UniqueHINTERNET = std::unique_ptr<void, HINTERNETDeleter>;

        std::wstring ToWide(std::string_view sv)
        {
            if (sv.empty()) return {};
            const int len = MultiByteToWideChar(
                CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()), nullptr, 0);
            if (len <= 0) throw Error("Downloader: UTF-8 to wide conversion failed");
            std::wstring out(static_cast<std::size_t>(len), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()),
                                out.data(), len);
            return out;
        }

        std::string WinErrorMsg(DWORD code)
        {
            char buf[256]{};
            FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                           nullptr, code, 0, buf, sizeof(buf), nullptr);
            return std::string(buf);
        }
#else
        struct CurlGlobal
        {
            CurlGlobal()
            {
                if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0)
                    throw Error("Downloader: curl_global_init failed");
            }
            ~CurlGlobal() { curl_global_cleanup(); }
        };

        void EnsureCurl()
        {
            static CurlGlobal g;
        }

        size_t WriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
        {
            auto* out = static_cast<std::vector<std::byte>*>(userdata);
            const size_t n = size * nmemb;
            const auto* bytes = reinterpret_cast<const std::byte*>(ptr);
            out->insert(out->end(), bytes, bytes + n);
            return n;
        }
#endif
    }

    ParsedURL ParseURL(std::string_view url)
    {
        ParsedURL result{};

        const auto schemeEnd = url.find("://");
        if (schemeEnd == std::string_view::npos)
            throw Error("Downloader: URL missing scheme");

        result.scheme = std::string(url.substr(0, schemeEnd));
        for (auto& c : result.scheme) c = static_cast<char>(std::tolower(c));

        if (result.scheme != "http" && result.scheme != "https")
            throw Error("Downloader: unsupported scheme '" + result.scheme + "'");

        result.port = (result.scheme == "https") ? 443 : 80;

        std::string_view rest = url.substr(schemeEnd + 3);

        const auto pathStart = rest.find('/');
        std::string_view hostPort = (pathStart == std::string_view::npos)
            ? rest : rest.substr(0, pathStart);

        if (pathStart != std::string_view::npos)
            result.path = std::string(rest.substr(pathStart));
        else
            result.path = "/";

        const auto colon = hostPort.rfind(':');
        if (colon != std::string_view::npos)
        {
            result.host = std::string(hostPort.substr(0, colon));
            const auto portStr = hostPort.substr(colon + 1);
            int p = 0;
            for (char c : portStr)
            {
                if (c < '0' || c > '9')
                    throw Error("Downloader: invalid port in URL");
                p = p * 10 + (c - '0');
            }
            result.port = static_cast<uint16_t>(p);
        }
        else
        {
            result.host = std::string(hostPort);
        }

        if (result.host.empty())
            throw Error("Downloader: empty host in URL");

        return result;
    }

    std::vector<std::byte> Download(std::string_view url)
    {
#if defined(_WIN32)
        const ParsedURL parsed = ParseURL(url);

        UniqueHINTERNET session(WinHttpOpen(
            L"MemPDB/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0));

        if (!session)
            throw Error("Downloader: WinHttpOpen failed: " + WinErrorMsg(GetLastError()));

        constexpr DWORD kTimeoutMs = 30000;
        WinHttpSetTimeouts(session.get(), kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);

        const std::wstring wHost = ToWide(parsed.host);
        UniqueHINTERNET connection(WinHttpConnect(
            session.get(), wHost.c_str(),
            static_cast<INTERNET_PORT>(parsed.port), 0));

        if (!connection)
            throw Error("Downloader: WinHttpConnect failed: " + WinErrorMsg(GetLastError()));

        const DWORD openFlags =
            (parsed.scheme == "https") ? WINHTTP_FLAG_SECURE : 0;

        const std::wstring wPath = ToWide(parsed.path);
        UniqueHINTERNET request(WinHttpOpenRequest(
            connection.get(),
            L"GET",
            wPath.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            openFlags));

        if (!request)
            throw Error("Downloader: WinHttpOpenRequest failed: " + WinErrorMsg(GetLastError()));

        DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
        WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY,
                         &redirectPolicy, sizeof(redirectPolicy));

        if (!WinHttpSendRequest(request.get(),
                                WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
            throw Error("Downloader: WinHttpSendRequest failed: " + WinErrorMsg(GetLastError()));

        if (!WinHttpReceiveResponse(request.get(), nullptr))
            throw Error("Downloader: WinHttpReceiveResponse failed: " + WinErrorMsg(GetLastError()));

        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        if (!WinHttpQueryHeaders(request.get(),
                                 WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX,
                                 &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX))
            throw Error("Downloader: failed to query HTTP status");

        if (statusCode != 200)
            throw Error("Downloader: HTTP " + std::to_string(statusCode) + " for " + std::string(url));

        std::vector<std::byte> result;
        {
            DWORD contentLen = 0;
            DWORD clSize     = sizeof(contentLen);
            if (WinHttpQueryHeaders(request.get(),
                                    WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX,
                                    &contentLen, &clSize, WINHTTP_NO_HEADER_INDEX))
            {
                result.reserve(contentLen);
            }
            else
            {
                result.reserve(4 * 1024 * 1024);
            }
        }

        constexpr DWORD kChunk = 64 * 1024;
        std::vector<std::byte> chunk(kChunk);

        for (;;)
        {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request.get(), &available))
                throw Error("Downloader: WinHttpQueryDataAvailable failed");

            if (available == 0) break;

            DWORD toRead = std::min(available, kChunk);
            DWORD bytesRead = 0;
            if (!WinHttpReadData(request.get(), chunk.data(), toRead, &bytesRead))
                throw Error("Downloader: WinHttpReadData failed");

            if (bytesRead == 0) break;

            result.insert(result.end(), chunk.begin(),
                          chunk.begin() + bytesRead);
        }

        if (result.empty())
            throw Error("Downloader: empty response for " + std::string(url));

        return result;
#else
        EnsureCurl();

        // Validate URL shape early so callers get the same errors as on Windows.
        (void)ParseURL(url);

        CURL* curl = curl_easy_init();
        if (!curl)
            throw Error("Downloader: curl_easy_init failed");

        struct CurlDeleter
        {
            void operator()(CURL* h) const noexcept
            {
                if (h) curl_easy_cleanup(h);
            }
        };
        std::unique_ptr<CURL, CurlDeleter> easy(curl);

        const std::string urlStr(url);
        std::vector<std::byte> result;
        result.reserve(4 * 1024 * 1024);

        curl_easy_setopt(easy.get(), CURLOPT_URL, urlStr.c_str());
        curl_easy_setopt(easy.get(), CURLOPT_USERAGENT, "MemPDB/1.0");
        curl_easy_setopt(easy.get(), CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(easy.get(), CURLOPT_MAXREDIRS, 10L);
        curl_easy_setopt(easy.get(), CURLOPT_CONNECTTIMEOUT, 30L);
        curl_easy_setopt(easy.get(), CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(easy.get(), CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(easy.get(), CURLOPT_WRITEDATA, &result);
        curl_easy_setopt(easy.get(), CURLOPT_FAILONERROR, 0L);

        const CURLcode rc = curl_easy_perform(easy.get());
        if (rc != CURLE_OK)
            throw Error(std::string("Downloader: ") + curl_easy_strerror(rc)
                        + " for " + urlStr);

        long statusCode = 0;
        curl_easy_getinfo(easy.get(), CURLINFO_RESPONSE_CODE, &statusCode);
        if (statusCode != 200)
            throw Error("Downloader: HTTP " + std::to_string(statusCode) + " for " + urlStr);

        if (result.empty())
            throw Error("Downloader: empty response for " + urlStr);

        return result;
#endif
    }
}
