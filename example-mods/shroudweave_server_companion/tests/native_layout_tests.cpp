#include "native_guards.hpp"
#include "../native/connection_profile.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

void check(bool value, const char* description) {
    if (!value) { std::printf("FAIL: %s\n", description); std::exit(1); }
}

int main(int argc, char** argv) {
    check(argc == 2, "dedicated fixture path required");
    std::ifstream file(argv[1], std::ios::binary);
    check(bool(file), "fixture available");
    const std::vector<unsigned char> raw(std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{});
    check(raw.size() > sizeof(IMAGE_DOS_HEADER), "fixture has DOS header");
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(raw.data());
    check(dos->e_magic == IMAGE_DOS_SIGNATURE && dos->e_lfanew > 0 &&
          static_cast<std::size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) <= raw.size(), "fixture has PE header");
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(raw.data() + dos->e_lfanew);
    check(nt->Signature == IMAGE_NT_SIGNATURE && nt->OptionalHeader.SizeOfImage <= 64 * 1024 * 1024 &&
          nt->OptionalHeader.SizeOfHeaders <= raw.size(), "fixture image bounds");
    std::vector<unsigned char> mapped(nt->OptionalHeader.SizeOfImage);
    std::memcpy(mapped.data(), raw.data(), nt->OptionalHeader.SizeOfHeaders);
    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto& s = section[i];
        check(static_cast<std::size_t>(s.PointerToRawData) + s.SizeOfRawData <= raw.size() &&
              static_cast<std::size_t>(s.VirtualAddress) + s.SizeOfRawData <= mapped.size(), "section bounds");
        std::memcpy(mapped.data() + s.VirtualAddress, raw.data() + s.PointerToRawData, s.SizeOfRawData);
    }
    check(esm::admission::is_supported_server_image(mapped.data()), "current retained native layout accepted");
    check(shroudweave::server::connection_profile_matches(mapped.data()), "current connection profile accepted");
    for (const auto rva : {0x9d60f0u,0x9d6b71u,0x9c5060u,0x9c4e00u,0x9c91d0u}) {
        mapped[rva] ^= 1;
        check(!shroudweave::server::connection_profile_matches(mapped.data()), "changed connection code rejected");
        mapped[rva] ^= 1;
    }
    for (const auto rva : {esm::admission::registration_rva, esm::admission::producer_rva,
            esm::admission::auth_handler_rva, esm::admission::dispatch_call_rva, 0x9cf205u}) {
        mapped[rva] ^= 1;
        check(!esm::admission::is_supported_server_image(mapped.data()), "native layout mutation refused");
        mapped[rva] ^= 1;
    }
    check(esm::admission::is_supported_server_image(mapped.data()), "restored layout accepted");
    std::printf("PASS: retained 26083076 native guard and five mutations\n");
}
