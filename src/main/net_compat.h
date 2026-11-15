// The BSD socket calls the app's small local servers use (phone_pads.cpp, roster_room.cpp), on Windows too: Winsock
// under the POSIX names, plus the local network address phones and other computers open (docs/WINDOWS_VULKAN.md).
#pragma once
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <cstdlib>
#include <cstring>
#include <vector>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
using ssize_t = long long;
#ifndef SHUT_RDWR
#define SHUT_RDWR SD_BOTH
#endif
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#endif

#ifdef __PROSPERO__
namespace blitz {
    std::string ps5_local_address();  // platform_ps5.cpp: the network control library's answer
}
#endif

namespace blitz::net {
    // Winsock starts once per process; the other platforms have nothing to start.
    inline bool start() {
#ifdef _WIN32
        static const bool ok = [] {
            WSADATA data;
            return WSAStartup(MAKEWORD(2, 2), &data) == 0;
        }();
        return ok;
#else
        return true;
#endif
    }

    // A socket as an int, as the servers keep it (Winsock's handles are small numbers in practice).
    inline int open_tcp() {
        if (!start()) return -1;
#ifdef _WIN32
        SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        return s == INVALID_SOCKET ? -1 : (int)s;
#else
        return ::socket(AF_INET, SOCK_STREAM, 0);
#endif
    }

    inline void close_socket(int fd) {
#ifdef _WIN32
        ::closesocket((SOCKET)fd);
#else
        ::close(fd);
#endif
    }

    inline ssize_t send_some(int fd, const char* data, size_t size) {
#ifdef _WIN32
        return ::send((SOCKET)fd, data, (int)size, 0);
#else
        return ::send(fd, data, size, 0);
#endif
    }

    inline ssize_t recv_some(int fd, char* data, size_t size, int flags = 0) {
#ifdef _WIN32
        return ::recv((SOCKET)fd, data, (int)size, flags);
#else
        return ::recv(fd, data, size, flags);
#endif
    }

    // After a failed recv: only the timeout ran out (try again), rather than the connection failing.
    inline bool timed_out() {
#ifdef _WIN32
        int e = WSAGetLastError();
        return e == WSAETIMEDOUT || e == WSAEWOULDBLOCK || e == WSAEINTR;
#else
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
    }

    inline void shutdown_socket(int fd) {
#ifdef _WIN32
        ::shutdown((SOCKET)fd, SD_BOTH);
#else
        ::shutdown(fd, SHUT_RDWR);
#endif
    }

    inline int accept_one(int fd) {
#ifdef _WIN32
        SOCKET c = ::accept((SOCKET)fd, nullptr, nullptr);
        return c == INVALID_SOCKET ? -1 : (int)c;
#else
        return ::accept(fd, nullptr, nullptr);
#endif
    }

    // Send and receive timeouts in milliseconds (a timeval elsewhere, a DWORD on Windows).
    inline void set_timeouts(int fd, int ms) {
#ifdef _WIN32
        DWORD t = (DWORD)ms;
        setsockopt((SOCKET)fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof(t));
        setsockopt((SOCKET)fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&t, sizeof(t));
#else
        timeval t{ms / 1000, (ms % 1000) * 1000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof(t));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &t, sizeof(t));
#endif
    }

    // The receive timeout alone (a send keeps its own).
    inline void set_recv_timeout(int fd, int ms) {
#ifdef _WIN32
        DWORD t = (DWORD)ms;
        setsockopt((SOCKET)fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof(t));
#else
        timeval t{ms / 1000, (ms % 1000) * 1000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof(t));
#endif
    }

    inline void set_int_option(int fd, int level, int name, int value) {
#ifdef _WIN32
        setsockopt((SOCKET)fd, level, name, (const char*)&value, sizeof(value));
#else
        setsockopt(fd, level, name, &value, sizeof(value));
#endif
    }

    // A listening port this app can take again right after it closed one (a restart). On Windows SO_REUSEADDR means
    // something else, sharing a port another program is listening on, so there it's exclusive use instead.
    inline void reuse_port(int fd) {
#ifdef _WIN32
        set_int_option(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1);
#else
        set_int_option(fd, SOL_SOCKET, SO_REUSEADDR, 1);
#endif
    }

    // A connection that drops can't kill the app (SIGPIPE) and doesn't follow a restart into the new app.
    inline void harden(int fd) {
#if defined(SO_NOSIGPIPE)
        set_int_option(fd, SOL_SOCKET, SO_NOSIGPIPE, 1);
#endif
#ifndef _WIN32
        fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
    }

    // This machine's address on the local network ("192.168.1.20"), or "" when offline: the first IPv4 address
    // that's up, not loopback or link-local, preferring Wi-Fi/Ethernet.
    inline std::string local_address() {
        std::string ip;
#ifdef __PROSPERO__
        ip = blitz::ps5_local_address();
        if (!ip.empty()) return ip;
#endif
#ifdef _WIN32
        if (!start()) return ip;
        ULONG size = 16 * 1024;
        std::vector<unsigned char> buf(size);
        auto* list = (IP_ADAPTER_ADDRESSES*)buf.data();
        // Adapters with a default gateway first: Hyper-V, WSL and VPN adapters are up too, but phones can't reach them.
        ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS;
        if (GetAdaptersAddresses(AF_INET, flags, nullptr, list, &size) == ERROR_BUFFER_OVERFLOW) {
            buf.resize(size);
            list = (IP_ADAPTER_ADDRESSES*)buf.data();
        }
        if (GetAdaptersAddresses(AF_INET, flags, nullptr, list, &size) != NO_ERROR) return ip;
        for (auto* a = list; a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
                char s[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &((sockaddr_in*)u->Address.lpSockaddr)->sin_addr, s, sizeof(s));
                if (strncmp(s, "169.254.", 8) == 0) continue;
                bool preferred = a->FirstGatewayAddress != nullptr &&
                                 (a->IfType == IF_TYPE_ETHERNET_CSMACD || a->IfType == IF_TYPE_IEEE80211);
                if (ip.empty() || preferred) ip = s;
                if (preferred) return ip;
            }
        }
#else
        ifaddrs* list = nullptr;
        if (getifaddrs(&list) == 0) {
            for (ifaddrs* a = list; a; a = a->ifa_next) {
                if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || (a->ifa_flags & IFF_LOOPBACK) || !(a->ifa_flags & IFF_UP)) continue;
                char s[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &((sockaddr_in*)a->ifa_addr)->sin_addr, s, sizeof(s));
                if (strncmp(s, "169.254.", 8) == 0) continue;
                ip = s;
                if (strncmp(a->ifa_name, "en", 2) == 0 || strncmp(a->ifa_name, "eth", 3) == 0 || strncmp(a->ifa_name, "wlan", 4) == 0)
                    break;  // Wi-Fi or Ethernet first
            }
            freeifaddrs(list);
        }
#endif
        return ip;
    }
}
