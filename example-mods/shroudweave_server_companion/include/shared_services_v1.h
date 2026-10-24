#pragma once
#include <stdint.h>

#if defined(SHROUDWEAVE_CLIENT_BUILD)
#define SW_CLIENT __declspec(dllexport)
#else
#define SW_CLIENT
#endif

// Client transport ABI 1. UTF-8, explicit lengths, no ownership crosses the
// DLL boundary. Return 0=ok, 1=unavailable/no message, 2=invalid/capacity,
// 3=transport failure. The client module must be pinned and called from one
// owner thread. Connect(0) revokes the destination and discards buffered
// replies. The configured peer must be the intended game server; this API does
// not discover it from logs.
#ifdef __cplusplus
extern "C" {
#endif
SW_CLIENT uint32_t ShroudweaveClient_ConnectV1(uint64_t server_steam);
SW_CLIENT uint32_t ShroudweaveClient_SendV1(const char* json, uint32_t length);
SW_CLIENT uint32_t ShroudweaveClient_ReceiveV1(char* output, uint32_t capacity, uint32_t* length);
#ifdef __cplusplus
}
#endif
