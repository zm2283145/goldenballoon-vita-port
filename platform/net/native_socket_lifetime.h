#ifndef MDKR_NATIVE_SOCKET_LIFETIME_H
#define MDKR_NATIVE_SOCKET_LIFETIME_H

#include "network_lifetime.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>

struct MdkrNativeSocketApi {
    static bool startup(uint16_t &version) noexcept {
        WSADATA data{};
        if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
        version = data.wVersion;
        return true;
    }
    static bool cleanup() noexcept { return ::WSACleanup() == 0; }
};
using MdkrNativeSocketOperations = MdkrWinsockReferencePolicy<MdkrNativeSocketApi>;
#else
struct MdkrNativeSocketOperations {
    static constexpr bool required = false;
    static bool start() noexcept { return true; }
    static void stop() noexcept {}
};
#endif

using MdkrNativeSocketLease = MdkrSharedNetworkLease<MdkrNativeSocketOperations>;

#endif
