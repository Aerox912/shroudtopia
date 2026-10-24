#include "inventory.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <set>
#include <string>

namespace shroudweave::server {
namespace {
using json = nlohmann::json;

bool module_id(std::string_view value) {
    if (value.empty() || value.size() > 64 || value.front() < 'a' || value.front() > 'z') return false;
    return std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}
bool module_file(std::string_view value) {
    if (value.empty() || value.size() > 120 || value.find("..") != std::string_view::npos ||
        value.size() <= 4 || value.substr(value.size() - 4) != ".dll") return false;
    if (!std::isalnum(static_cast<unsigned char>(value.front()))) return false;
    std::string base(value.substr(0, value.find('.')));
    std::transform(base.begin(), base.end(), base.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (base == "con" || base == "prn" || base == "aux" || base == "nul" ||
        (base.size() == 4 && (base.substr(0, 3) == "com" || base.substr(0, 3) == "lpt") &&
         base[3] >= '1' && base[3] <= '9')) return false;
    return std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    });
}
bool digest(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}
std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}
}

std::optional<Inventory> parse_inventory(std::string_view text, std::string* error) {
    const auto fail = [error](const char* reason) -> std::optional<Inventory> {
        if (error) *error = reason;
        return std::nullopt;
    };
    if (error) error->clear();
    if (text.empty() || text.size() > 65536) return fail("inventory size invalid");
    try {
        const auto document = json::parse(text.begin(), text.end());
        if (document.contains("proxyChain")) return fail("external proxyChain retired; use native chainloader.ini");
        if (!document.is_object() || document.size() != 3 ||
            !document.contains("schema") || !document["schema"].is_number_integer() ||
            document["schema"] != 1 || !document.contains("modules") ||
            !document["modules"].is_array() || !document.contains("requiredModules") ||
            !document["requiredModules"].is_array()) return fail("inventory schema invalid");
        const auto& required_list = document["requiredModules"];
        if (required_list.size() > 32) return fail("required module count invalid");
        std::set<std::string> required_ids;
        for (const auto& id : required_list) {
            if (!id.is_string()) return fail("required module identifier invalid");
            const auto name = id.get<std::string>();
            if (!module_id(name) || !required_ids.insert(name).second)
                return fail("required module identifier invalid or duplicated");
        }
        const auto& modules = document["modules"];
        if (modules.size() > 32) return fail("module count invalid");
        Inventory inventory;
        inventory.modules.reserve(modules.size());
        std::set<std::string> ids;
        std::set<std::string> files;
        for (const auto& item : modules) {
            if (!item.is_object() || item.size() != 4 ||
                !item.contains("id") || !item["id"].is_string() ||
                !item.contains("file") || !item["file"].is_string() ||
                !item.contains("sha256") || !item["sha256"].is_string() ||
                !item.contains("required") || !item["required"].is_boolean())
                return fail("module fields invalid");
            ModuleSpec spec{item["id"].get<std::string>(), item["file"].get<std::string>(),
                            item["sha256"].get<std::string>(), item["required"].get<bool>()};
            if (!module_id(spec.id) || !module_file(spec.file) || !digest(spec.sha256))
                return fail("module identifier, filename or digest invalid");
            if (spec.required != required_ids.contains(spec.id))
                return fail("module required flag and declaration disagree");
            spec.file = lowercase(std::move(spec.file));
            spec.sha256 = lowercase(std::move(spec.sha256));
            if (!ids.insert(spec.id).second || !files.insert(spec.file).second)
                return fail("duplicate module identifier or filename");
            inventory.modules.push_back(std::move(spec));
        }
        for (const auto& id : required_ids) {
            if (!ids.contains(id)) return fail("required module missing");
        }
        return inventory;
    } catch (const json::exception&) {
        return fail("inventory JSON invalid");
    }
}
}
