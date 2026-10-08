#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libScePosixForWebKit/GuestResolver.hpp"
#include <array>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <source_location>
#include <barrier>
#include <string>
#include <thread>

extern "C" {
GuestResolver::HostEntry* APS5_VABI gethostbyname_nid_postfix(const char*);
int* APS5_VABI sceNetErrnoLoc();
int APS5_VABI getaddrinfo_nid_postfix(const char*, const char*, const GuestResolver::AddressInfo*, GuestResolver::AddressInfo**);
void APS5_VABI freeaddrinfo_nid_postfix(GuestResolver::AddressInfo*);
int APS5_VABI getnameinfo_nid_postfix(const void*, std::uint32_t, char*, std::uint32_t, char*, std::uint32_t, int);
const char* APS5_VABI gai_strerror_nid_postfix(int);
}
void Require(bool condition, std::source_location location = std::source_location::current()) {
    if (!condition) {
        std::fprintf(stderr, "Resolver check failed at line %u\n", location.line());
        std::abort();
    }
}
int main() {
    auto* legacy = gethostbyname_nid_postfix("127.0.0.1");
    Require(legacy && legacy->name && std::strcmp(legacy->name, "127.0.0.1") == 0);
    Require(legacy->family == 2 && legacy->length == 4);
    const std::array<unsigned char, 4> loopback{127, 0, 0, 1};
    Require(legacy->addresses && legacy->addresses[0]);
    Require(std::memcmp(legacy->addresses[0], loopback.data(), loopback.size()) == 0);
    Require(legacy->addresses[1] == nullptr && legacy->aliases && legacy->aliases[0] == nullptr);
    std::barrier ready(2);
    std::thread worker([&] {
        auto* entry = gethostbyname_nid_postfix("127.0.0.2");
        ready.arrive_and_wait();
        const std::array<unsigned char, 4> expected{127, 0, 0, 2};
        Require(entry && std::strcmp(entry->name, "127.0.0.2") == 0);
        Require(std::memcmp(entry->addresses[0], expected.data(), expected.size()) == 0);
    });
    legacy = gethostbyname_nid_postfix("127.0.0.1");
    ready.arrive_and_wait();
    Require(std::strcmp(legacy->name, "127.0.0.1") == 0);
    Require(std::memcmp(legacy->addresses[0], loopback.data(), loopback.size()) == 0);
    worker.join();
    *sceNetErrnoLoc() = 77;
    Require(gethostbyname_nid_postfix(std::string(1024, 'a').c_str()) == nullptr);
    Require(*sceNetErrnoLoc() >= 1 && *sceNetErrnoLoc() <= 4);
    std::array<unsigned char, 16> v4{16, 2, 0x6d, 0x06, 127, 0, 0, 1};
    char host[128]{}, service[32]{};
    Require(getnameinfo_nid_postfix(v4.data(), v4.size(), host, sizeof(host), service, sizeof(service), 10) == 0);
    Require(std::strcmp(host, "127.0.0.1") == 0 && std::strcmp(service, "27910") == 0);
    std::array<unsigned char, 28> v6{28, 28};
    v6[23] = 1;
    Require(getnameinfo_nid_postfix(v6.data(), v6.size(), host, sizeof(host), nullptr, 0, 10) == 0);
    Require(std::strcmp(host, "::1") == 0);
    Require(getnameinfo_nid_postfix(v4.data(), 2, host, sizeof(host), nullptr, 0, 10) == 5);
    Require(getnameinfo_nid_postfix(v4.data(), v4.size(), host, sizeof(host), nullptr, 0, 32) == 3);
    Require(getnameinfo_nid_postfix(v4.data(), v4.size(), host, 1, nullptr, 0, 10) == 14);
    Require(std::strlen(gai_strerror_nid_postfix(14)) > 0);
    Require(std::strstr(gai_strerror_nid_postfix(-1), "Unknown") != nullptr);
    GuestResolver::AddressInfo hints{};
    hints.flags = 12; // numeric host and service: no external DNS dependency
    hints.socketType = 1;
    for (const auto* numeric : {"127.0.0.1", "::1"}) {
        GuestResolver::AddressInfo* result = nullptr;
        Require(getaddrinfo_nid_postfix(numeric, "27910", &hints, &result) == 0);
        Require(result != nullptr);
        for (auto* entry = result; entry; entry = entry->next) {
            Require(entry->family == 2 || entry->family == 28);
            Require(entry->socketType == 1);
            Require(getnameinfo_nid_postfix(entry->address, entry->addressLength,
                host, sizeof(host), service, sizeof(service), 10) == 0);
            Require(std::strcmp(host, numeric) == 0 && std::strcmp(service, "27910") == 0);
        }
        freeaddrinfo_nid_postfix(result);
    }
    GuestResolver::AddressInfo* result = reinterpret_cast<GuestResolver::AddressInfo*>(1);
    hints.family = 1234;
    Require(getaddrinfo_nid_postfix("127.0.0.1", "1", &hints, &result) == 5 && !result);
    hints.family = 0;
    hints.flags = 0x200;
    Require(getaddrinfo_nid_postfix("127.0.0.1", "1", &hints, &result) == 3 && !result);
    hints.flags = 12;
    Require(getaddrinfo_nid_postfix("invalid-numeric-address", "1", &hints, &result) == 8 && !result);
    freeaddrinfo_nid_postfix(nullptr);
}
