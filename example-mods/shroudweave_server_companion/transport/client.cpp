#include "steam_channel.hpp"
#include "shared_settings.hpp"
#define SHROUDWEAVE_CLIENT_BUILD
#include "shared_services_v1.h"
#include <atomic>
#include <deque>
#include <cstring>
namespace {
shroudweave::server::SteamChannel channel;
std::atomic<std::uint64_t> server{};
std::deque<std::string> replies;
}
extern "C" __declspec(dllexport) uint32_t ShroudweaveClient_ConnectV1(uint64_t peer) {
    server.store(0); replies.clear();
    if (!peer) return 0;
    HMODULE pinned{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(ShroudweaveClient_ConnectV1), &pinned)) return 1;
    try {
        if (!channel.initialize(false, [](std::uint64_t sender) { return sender && sender == server.load(); })) return 1;
        // Drain old channel packets before allowing the new connection.
        channel.poll([](std::uint64_t, std::string_view){});
        server.store(peer); return 0;
    } catch (...) { return 1; }
}
extern "C" __declspec(dllexport) uint32_t ShroudweaveClient_SendV1(const char* bytes, uint32_t length) {
    if (!bytes || !length || length > 49152) return 2;
    const auto peer = server.load(); if (!peer) return 1;
    try {
        if (!shroudweave::server::parse_settings_json({bytes,length})) return 2;
        return channel.send(peer,{bytes,length}) ? 0 : 3;
    } catch (...) { return 3; }
}
extern "C" __declspec(dllexport) uint32_t ShroudweaveClient_ReceiveV1(char* output, uint32_t capacity, uint32_t* length) {
    if (!length) return 2; *length=0;
    if (!output || capacity<65536) return 2;
    try {
        channel.poll([](std::uint64_t sender,std::string_view body) {
            if (sender && sender == server.load() && replies.size()<16) replies.emplace_back(body);
        });
        if (replies.empty()) return 1;
        *length=static_cast<uint32_t>(replies.front().size());
        std::memcpy(output,replies.front().data(),*length); replies.pop_front(); return 0;
    } catch (...) { return 3; }
}
