#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace shroudweave::server {
struct ModuleSpec {
    std::string id;
    std::string file;
    std::string sha256;
    bool required;
};
struct Inventory {
    std::vector<ModuleSpec> modules;

};
std::optional<Inventory> parse_inventory(std::string_view text, std::string* error = nullptr);
}
