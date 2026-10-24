# Shared Shroudweave settings store

Source-only reuse from Nick's private Shroudweave Loader checkout, copied on
23 October 2026. The standalone companion compiles these files directly and
does not require a Loader DLL, launcher or bootstrap. Existing `esm::settings`
identifiers and source ownership are preserved.

Source checkout HEAD: `f101d6359ff05ead9166e6c0b670ada7127ef7c6`.
The byte hashes identify the actual copied working files, including any local
changes; HEAD alone is not their source identity.

| Original path | SHA-256 |
| --- | --- |
| `src/host/settings_store.cpp` | `a9baca5391e47e1065ba23e1288450694dc808b1e853685bccc9b866208f4df4` |
| `include/esm/settings_store.hpp` | `a745dcce791d4a4b92b32832f0eda736ba6b01e304d8ebc48be7783e0baf1aeb` |

Both files are copied unchanged. They use the fork's existing nlohmann/json
dependency. The source checkout has no root LICENSE file; this private source
reuse does not establish permission to publish it under a new upstream license.
