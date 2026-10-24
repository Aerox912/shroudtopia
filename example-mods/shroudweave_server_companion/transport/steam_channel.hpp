#pragma once
#include <windows.h>
#include <steam_api_common.h>
#include <isteamnetworkingmessages.h>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>

namespace shroudweave::server {
// One shared channel for all companion services. No chat hook or private
// Steam initialization/callback pump. Uses the game's existing Steam context.
inline constexpr int shared_channel = 18358;
inline constexpr std::size_t shared_max_bytes = 65536;
class SteamChannel {
    ISteamNetworkingMessages* api_{};
    class Request final : public CCallbackBase {
    public:
        ISteamNetworkingMessages* api{};
        std::function<bool(std::uint64_t)> allowed;
        void server(bool value) { m_nCallbackFlags = value ? k_ECallbackFlagsGameServer : 0; }
        void Run(void* data) override {
            if (!data) return;
            const auto& request = *static_cast<SteamNetworkingMessagesSessionRequest_t*>(data);
            try {
                if (allowed(request.m_identityRemote.GetSteamID64())) api->AcceptSessionWithUser(request.m_identityRemote);
            } catch (...) { /* Never unwind through the game's Steam callback. */ }
        }
        void Run(void* data, bool failed, SteamAPICall_t) override { if (!failed) Run(data); }
        int GetCallbackSizeBytes() override { return sizeof(SteamNetworkingMessagesSessionRequest_t); }
    };
    Request* callback_{};
public:
    // Runtime and callback are pinned until process exit. Caller owns any
    // allowed callback capture for at least that lifetime.
    bool initialize(bool server, std::function<bool(std::uint64_t)> allowed) {
        if (api_) return true;
        auto module = GetModuleHandleW(L"steam_api64.dll");
        if (!module) return false;
        using Accessor = ISteamNetworkingMessages* (*)();
        using Register = void (*)(CCallbackBase*, int);
        const auto get = reinterpret_cast<Accessor>(GetProcAddress(module, server ?
            "SteamAPI_SteamGameServerNetworkingMessages_SteamAPI_v002" : "SteamAPI_SteamNetworkingMessages_SteamAPI_v002"));
        const auto reg = reinterpret_cast<Register>(GetProcAddress(module, "SteamAPI_RegisterCallback"));
        if (!get || !reg || !(api_ = get())) return false;
        callback_ = new Request;
        callback_->api = api_; callback_->allowed = std::move(allowed); callback_->server(server);
        reg(callback_, SteamNetworkingMessagesSessionRequest_t::k_iCallback);
        return true;
    }
    bool send(std::uint64_t peer, std::string_view bytes) {
        if (!api_ || !peer || bytes.empty() || bytes.size() > shared_max_bytes) return false;
        SteamNetworkingIdentity id{}; id.SetSteamID64(peer);
        return api_->SendMessageToUser(id, bytes.data(), static_cast<uint32>(bytes.size()),
            k_nSteamNetworkingSend_ReliableNoNagle | k_nSteamNetworkingSend_AutoRestartBrokenSession,
            shared_channel) == k_EResultOK;
    }
    void poll(const std::function<void(std::uint64_t, std::string_view)>& handler) {
        if (!api_) return;
        SteamNetworkingMessage_t* messages[8]{};
        const auto count = api_->ReceiveMessagesOnChannel(shared_channel, messages, 8);
        for (int i = 0; i < count && i < 8; ++i) {
            auto* m = messages[i]; if (!m) continue;
            try {
                if (m->m_nChannel == shared_channel && m->m_cbSize > 0 &&
                    m->m_cbSize <= static_cast<int>(shared_max_bytes) && m->m_pData)
                    handler(m->m_identityPeer.GetSteamID64(), {static_cast<const char*>(m->m_pData),
                        static_cast<std::size_t>(m->m_cbSize)});
            } catch (...) { m->Release(); throw; }
            m->Release();
        }
    }
};
}
