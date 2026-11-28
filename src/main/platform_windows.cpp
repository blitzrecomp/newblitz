// Windows host services: the functions src/main/macos_support.mm provides on a Mac (docs/WINDOWS_VULKAN.md).
#define WIN32_LEAN_AND_MEAN
#include <filesystem>
#include "blitz_host.h"
#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "crypt32.lib")

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
    // A blocking HTTP POST (call it off the main thread): true on a 2xx answer. Crash reports (crash_report.cpp).
    bool http_post(const std::string& url, const std::string& content_type, const std::string& body, int& status) {
        status = 0;
        std::wstring wurl = widen(url);
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        wchar_t host[256], path[2048];
        parts.lpszHostName = host;
        parts.dwHostNameLength = (DWORD)std::size(host);
        parts.lpszUrlPath = path;
        parts.dwUrlPathLength = (DWORD)std::size(path);
        parts.dwExtraInfoLength = 1;
        if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) return false;
        std::wstring object(parts.lpszUrlPath, parts.dwUrlPathLength);
        if (parts.lpszExtraInfo && parts.dwExtraInfoLength) object.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        // BRANDING: HTTP User-Agent
        Handle session{WinHttpOpen(L"BlitzRecomp", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
        if (!session.h) return false;
        WinHttpSetTimeouts(session.h, 20000, 20000, 20000, 20000);
        Handle connection{WinHttpConnect(session.h, std::wstring(parts.lpszHostName, parts.dwHostNameLength).c_str(), parts.nPort, 0)};
        if (!connection.h) return false;
        DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
        Handle request{WinHttpOpenRequest(connection.h, L"POST", object.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags)};
        if (!request.h) return false;
        std::wstring headers = L"Content-Type: " + widen(content_type) + L"\r\n";
        if (!WinHttpSendRequest(request.h, headers.c_str(), (DWORD)-1L, (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0) ||
            !WinHttpReceiveResponse(request.h, nullptr))
            return false;
        DWORD code = 0, size = sizeof(code);
        WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &size,
                            WINHTTP_NO_HEADER_INDEX);
        status = (int)code;
        return code >= 200 && code < 300;
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

// Secure storage (Cloud Sync's code): DPAPI. The value sealed with CryptProtectData for this Windows user, in
// %LOCALAPPDATA%\NewBlitz\secrets\<name>.dpapi: another user, or a copy of the file on another PC, can't open it.
namespace {
    std::filesystem::path secret_file(const std::string& name) {
        const wchar_t* base = _wgetenv(L"LOCALAPPDATA");
        if (base == nullptr || *base == 0) return {};
        // BRANDING: the folder under LOCALAPPDATA
        return std::filesystem::path(base) / L"NewBlitz" / L"secrets" / (widen(name) + L".dpapi");
    }
}

namespace blitz {
    bool secure_put(const std::string& name, const std::string& value) {
        std::filesystem::path p = secret_file(name);
        if (p.empty()) return false;
        DATA_BLOB in{(DWORD)value.size(), (BYTE*)value.data()}, out{};
        if (!CryptProtectData(&in, L"New Blitz", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
        bool ok = false;
        if (FILE* f = _wfopen(p.c_str(), L"wb")) {
            ok = fwrite(out.pbData, 1, out.cbData, f) == out.cbData;
            ok = fclose(f) == 0 && ok;
        }
        LocalFree(out.pbData);
        return ok;
    }

    bool secure_get(const std::string& name, std::string& value) {
        value.clear();
        std::filesystem::path p = secret_file(name);
        std::vector<BYTE> sealed;
        if (FILE* f = p.empty() ? nullptr : _wfopen(p.c_str(), L"rb")) {
            BYTE buf[4096];
            for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) sealed.insert(sealed.end(), buf, buf + n);
            fclose(f);
        }
        if (sealed.empty()) return false;
        DATA_BLOB in{(DWORD)sealed.size(), sealed.data()}, out{};
        if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
        value.assign((const char*)out.pbData, out.cbData);
        SecureZeroMemory(out.pbData, out.cbData);
        LocalFree(out.pbData);
        return !value.empty();
    }

    void secure_erase(const std::string& name) {
        std::filesystem::path p = secret_file(name);
        std::error_code ec;
        if (!p.empty()) std::filesystem::remove(p, ec);
    }

    const char* secure_kind() { return "Windows' DPAPI"; }
}
