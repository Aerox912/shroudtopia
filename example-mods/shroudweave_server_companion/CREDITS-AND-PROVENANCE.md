# Source provenance and notices

This document covers only the generic companion and reusable host components present in this source tree. Mod-specific schemas, policies, native providers, and gameplay implementations belong in their owning mod repositories.

## Dedicated entry gate

The entry/dedicated_* gate, proxy, and thunk sources are adapted from the private Shroudweave Loader checkout at E:\Repos\esm-loader, commit f101d6359ff05ead9166e6c0b670ada7127ef7c6, on 23 October 2026. The source is adapted for this standalone Shroudtopia host. Internal Esm* implementation names remain as compatibility identifiers; this companion does not load a separate Loader binary. The associated synthetic gate fixtures are offline tests only.

## Connection identity

The generic connection lifecycle in native/connections.*, native/connection_profile.hpp, and src/connection_registry.cpp is based on the design in E:\Repos\enshrouded-server-compat\src\flight_identity.*. The native layout and byte guards are separately bounded to dedicated-server build 26083076, retained executable SHA-256 d7baa768eb00aa895d9f5a1cd8747ccf66858c7b542f50983092b2e3261588ee. The old offsets are not reused. This source profile and offline inspection do not establish live connection or gameplay behavior.

## Settings and file identity

The unchanged shared settings store in shared/esm is source reuse from Nick's private Shroudweave Loader checkout. Exact file hashes and source paths are recorded in shared/esm/PROVENANCE.md. The files are compiled into this companion; no separate Loader runtime is required. The source checkout has no root license file, so this reuse does not establish permission to publish it under another license.

The generic file identity helper in entry/file_identity.hpp retains MIT-licensed material from E:\Repos\enshrouded-server-whitelist. Its original MIT notice is preserved at entry/FILE-IDENTITY-LICENSE.

## Dependencies and legacy loader

MinHook 1.3.4 is copied unchanged from the enshrouded-server-compat vendor directory; its license remains in vendor/minhook-1.3.4/LICENSE.txt. Steam declarations use Shroudtopia's existing third-party/steam headers.

The generic legacy DLL loader in entry/legacy_loader.cpp adapts the loading, configuration, and logging implementation from Entrodor's Modloader Proxy. The MIT license is preserved in MODLOADER-PROXY-LICENSE. Multi-name proxy export generation and the upstream DllMain behavior are not included; this companion supports the dedicated-server forwarding path documented in README.md.

## Project ownership

The generic host services, module ABI, inventory, settings router, and test seams are Shroudtopia project work. Original ownership, notices, source paths, and hashes are retained for adapted or reused components above and alongside the relevant source.
