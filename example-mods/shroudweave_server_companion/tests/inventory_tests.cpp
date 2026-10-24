#include "inventory.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>
using shroudweave::server::parse_inventory;

namespace {
void check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
}

int main() {
    constexpr const char* hash = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    constexpr const char* upper_hash = "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF";
    const auto module = [](const std::string& id, const std::string& file,
        const std::string& digest, bool required) {
        return std::string(R"({"id":")") + id + R"(","file":")" + file +
            R"(","sha256":")" + digest + R"(","required":)" +
            (required ? "true}" : "false}");
    };
    const auto inventory = [](const std::string& required, const std::string& modules) {
        return std::string(R"({"schema":1,"requiredModules":)") + required +
            R"(,"modules":)" + modules + "}";
    };

    std::string error;
    const auto generic = inventory(R"(["alpha"])",
        "[" + module("alpha", "Alpha.dll", upper_hash, true) + "," +
        module("beta", "beta.dll", hash, false) + "]");
    const auto valid = parse_inventory(generic, &error);
    check(valid.has_value() && error.empty(), "generic required and optional module set is accepted");
    check(valid->modules.size() == 2 && valid->modules[0].id == "alpha" &&
        valid->modules[0].required && valid->modules[1].id == "beta" &&
        !valid->modules[1].required, "module identity and required flags are retained");
    check(valid->modules[0].file == "alpha.dll" && valid->modules[0].sha256 == hash,
        "filenames and digests are normalized to lowercase");

    const auto empty = parse_inventory(inventory("[]", "[]"), &error);
    check(empty.has_value(), "empty generic module inventory is valid");
    const auto optional_only = parse_inventory(inventory("[]",
        "[" + module("alpha-2", "alpha-2.dll", hash, false) + "]"), &error);
    check(optional_only && optional_only->modules.size() == 1 &&
        optional_only->modules[0].id == "alpha-2" && !optional_only->modules[0].required,
        "optional-only generic inventory is valid");

    const std::string proxy = R"({"schema":1,"requiredModules":[],"modules":[],"proxyChain":{"sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"}})";
    const auto invalid = [&](const std::string& text, const char* reason) {
        check(!parse_inventory(text, &error), reason);
        check(!error.empty(), "invalid inventory reports a reason");
    };
    invalid("", "empty document is refused");
    invalid(R"({"schema":2,"requiredModules":[],"modules":[]})", "unsupported schema is refused");
    invalid(R"({"schema":1,"requiredModules":[],"modules":[])",
        "malformed JSON is refused");
    invalid(R"({"schema":1,"requiredModules":[],"modules":[],"extra":true})",
        "unknown top-level field is refused");
    invalid(R"({"schema":1,"requiredModules":["Alpha"],"modules":[]})",
        "invalid required module identifier is refused");
    invalid(inventory("[]", "[" + module("Alpha", "alpha.dll", hash, false) + "]"),
        "invalid module identifier is refused");
    invalid(inventory(R"(["alpha"])",
        "[" + module("alpha", "..\\evil.dll", hash, true) + "]"),
        "path traversal in module filename is refused");
    invalid(inventory(R"(["alpha"])",
        "[" + module("alpha", "CON.dll", hash, true) + "]"),
        "Windows device filename is refused");
    invalid(inventory(R"(["alpha"])",
        "[" + module("alpha", "alpha.dll", "bad", true) + "]"),
        "invalid module digest is refused");
    invalid(inventory(R"(["alpha"])",
        "[" + module("alpha", "alpha.dll", hash, false) + "]"),
        "required flag must match the required module list");
    invalid(inventory(R"(["alpha"])",
        "[" + module("beta", "beta.dll", hash, false) + "]"),
        "missing required module is refused");
    invalid(inventory(R"(["alpha","alpha"])",
        "[" + module("alpha", "alpha.dll", hash, true) + "]"),
        "duplicate required module identifier is refused");
    invalid(inventory(R"(["alpha"])",
        "[" + module("alpha", "alpha.dll", hash, true) + "," +
        module("alpha", "other.dll", hash, true) + "]"),
        "duplicate module identifier is refused");
    invalid(inventory(R"(["alpha"])",
        "[" + module("alpha", "alpha.dll", hash, true) + "," +
        module("beta", "alpha.dll", hash, false) + "]"),
        "duplicate module filename is refused");
    invalid(R"({"schema":1,"requiredModules":[],"modules":[{"id":"alpha","file":"alpha.dll","sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","required":false,"extra":true}]})",
        "unknown module field is refused");
    invalid(proxy, "retired external proxy-chain inventory is refused");

    std::puts("PASS: generic module inventory validation");
}
