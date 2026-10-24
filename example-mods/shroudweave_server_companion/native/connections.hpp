#pragma once
#include "connection_registry.hpp"
#include <windows.h>
#include <functional>
namespace shroudweave::server {
bool install_connections(HMODULE game);
void with_connections(const std::function<void(ConnectionRegistry&)>&);
}
