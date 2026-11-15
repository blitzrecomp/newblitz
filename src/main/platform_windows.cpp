// Windows host services: the functions src/main/macos_support.mm provides on a Mac (docs/WINDOWS_VULKAN.md).
#define WIN32_LEAN_AND_MEAN
#include <filesystem>
#include "blitz_host.h"
#include <windows.h>
#include <winhttp.h>
#include <string>
#include <vector>
#pragma comment(lib, "winhttp.lib")

namespace {
    std::wstring widen(const std::string& s) {
        if (s.empty()) return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
        return w;
    }

    // Closes a WinHTTP handle when it goes out of scope.
    struct Handle {
        HINTERNET h = nullptr;
        ~Handle() { if (h) WinHttpCloseHandle(h); }
    };
}

namespace blitz {
    // macOS's press-and-hold accent picker; Windows has none.
    void disable_press_and_hold() {}

    // The Cocoa window's number (the Mac's screenshots); capture.cpp uses the HWND on Windows.
    int window_number(void*) { return 0; }

    // The macOS app menu; a Windows window has none.
    void install_app_menu(const std::string&) {}

    // A blocking HTTP GET (call it off the main thread): true with the body when the server answers 200.
    // Rosters at launch (rosters_live.cpp). WinHTTP, with Windows' own certificate store for HTTPS.
    bool http_get(const std::string& url, const std::string& bearer, const std::string& accept, std::string& body, int& status) {
        body.clear();
        status = 0;
        std::wstring wurl = widen(url);
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        wchar_t host[256], path[2048];
        parts.lpszHostName = host;
        parts.dwHostNameLength = (DWORD)std::size(host);
        parts.lpszUrlPath = path;
        parts.dwUrlPathLength = (DWORD)std::size(path);
        parts.dwExtraInfoLength = 1;  // the query stays on the path
        if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) return false;
        std::wstring object(parts.lpszUrlPath, parts.dwUrlPathLength);
        if (parts.lpszExtraInfo && parts.dwExtraInfoLength) object.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);

        // BRANDING: HTTP User-Agent
        Handle session{WinHttpOpen(L"BlitzRecomp", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
        if (!session.h) return false;
        WinHttpSetTimeouts(session.h, 15000, 15000, 15000, 15000);
        Handle connection{WinHttpConnect(session.h, std::wstring(parts.lpszHostName, parts.dwHostNameLength).c_str(), parts.nPort, 0)};
        if (!connection.h) return false;
        DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
        Handle request{WinHttpOpenRequest(connection.h, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags)};
        if (!request.h) return false;
        std::wstring headers;
        if (!accept.empty()) headers += L"Accept: " + widen(accept) + L"\r\n";
        if (!bearer.empty()) headers += L"Authorization: Bearer " + widen(bearer) + L"\r\n";
        if (!WinHttpSendRequest(request.h, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), (DWORD)-1L,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request.h, nullptr))
            return false;
        DWORD code = 0, size = sizeof(code);
        WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &size,
                            WINHTTP_NO_HEADER_INDEX);
        status = (int)code;
        std::string got;
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request.h, &available) || available == 0) break;
            std::vector<char> buf(available);
            DWORD read = 0;
            if (!WinHttpReadData(request.h, buf.data(), available, &read) || read == 0) break;
            got.append(buf.data(), read);
        }
        if (code != 200) return false;
        body = std::move(got);
        return true;
    }
}

namespace blitz {
    // A computer has file dialogs: no drop folder.
    std::filesystem::path rom_drop_folder() { return {}; }
}

namespace blitz {
    void window_created(SDL_Window*) {}
}

namespace blitz {
    void early_init() {}
}
