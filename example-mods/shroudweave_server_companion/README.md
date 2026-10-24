# Shroudweave server companion

Status: experimental, offline source for the Windows x64 dedicated-server profile 26083076. The project builds a standalone Shroudtopia dbghelp.dll proxy and a generic host for versioned server modules.

## SDK boundary

This tree provides generic startup, module loading, connection identity, revisioned settings, authorization, action routing, and transport plumbing. It does not ship game-mod schemas, page IDs, action names, policy defaults, or feature implementations. Each mod repository owns those details and supplies a module DLL that uses the public contracts in include.

The module inventory accepts an empty module list and arbitrary lowercase IDs. No module is bundled by default.

## Startup and module inventory

Place the built dbghelp.dll beside the dedicated executable, together with shroudweave-server.json. The proxy forwards the executable's MiniDumpWriteDump call to the system dbghelp.dll. The startup gate holds entry while the host validates the inventory, pins configured modules, and calls their start exports. A failed required module, or any module whose start export was invoked but did not become ready, prevents entry from resuming. Loaded module DLLs remain resident for the process lifetime.

The inventory is schema 1 and has exactly three top-level fields: schema, requiredModules, and modules. Each module entry has id, file, sha256, and required. The required ID list must match the entries marked required. IDs use lowercase ASCII letters, digits, underscore, and dash, begin with a letter, and are at most 64 bytes. A module file name consists of the prefix shroudweave_, the module ID, and the suffix .dll. The hash is SHA-256 of that file. For example, a required module with ID alpha uses shroudweave_alpha.dll and must appear in both required declarations. An optional module may be omitted from the inventory; if it is listed and its start export fails, startup still fails.

Each module exports ShroudweaveServerModuleStartV1. The host calls it synchronously on the startup thread while the dedicated entry remains held. The module registers its settings pages and action providers through the per-module services table. It must finish registration during its start call. The host freezes registration before starting message routing.

The optional shroudweave-services.json file selects a generic settings state_id; if absent, the host uses server. It has schema 1 and exactly schema and state_id. Malformed configuration fails closed. A changed configuration or native connection epoch stops routing and requires a process restart. This file contains no mod-specific settings.

## Generic services

The C ABI in include/server_services_v1.h lets modules register revisioned JSON settings pages and action providers. A module supplies its own page and route IDs, defaults, validators, migration callback, action validation, authorization policy, capability mask, and execution callback. The host owns storage, current-peer checks, revision conflict handling, capability heartbeats, request sequencing, expiry, receipt handling, and route lifecycle.

A settings page can declare a module-owned legacy page ID and migration callback. Migration is considered only when the canonical page is absent. Existing canonical data is validated and takes precedence; malformed canonical data fails closed. Migrated output is validated and created atomically without replacing another writer's data. The legacy record is left untouched. The SDK does not infer mod-specific schema or revision rules.

Action execution callbacks run synchronously on the thread that calls pump_actions. The host does not claim that this caller is a game thread. Modules must check the current native context and capability gates they require. Queue acceptance is not proof that a game effect succeeded.

The optional observe_creation_context service currently returns unavailable in the standalone host. The startup extension in include/server_startup_v1.h is a bounded, module-owned direct-call registration interface during Start; it is not a general memory writer, runtime patcher, transaction, rollback, or unload API.

## Optional final validation

The independently versioned table in include/server_final_validation_v1.h is an optional HostV1 tail. Older modules can ignore it. A new module checks the host field coverage and table size/version before registering one callback during its synchronous Start:

```cpp
// validate_committed_state is code in this pinned root module. Its context,
// if nonnull, must live for the process. Never retain host or use it as context.
if (host->struct_size < SHROUDWEAVE_SERVER_HOST_V1_FINAL_VALIDATION_SERVICES_SIZE)
    return SHROUDWEAVE_SERVER_FINAL_VALIDATION_UNAVAILABLE_V1;
const auto* final = host->final_validation_services;
if (!final || final->struct_size < sizeof(*final) ||
    final->abi_major != SHROUDWEAVE_SERVER_FINAL_VALIDATION_ABI_MAJOR_V1 ||
    !final->register_validator)
    return SHROUDWEAVE_SERVER_FINAL_VALIDATION_UNAVAILABLE_V1;
return final->register_validator(final->registration_context,
    validate_committed_state, nullptr);
```

The host copies the callback and opaque context, binds them to the pinned registering root module, and invokes validators once in module order on the same held-entry thread after synchronous legacy loading. A root module wrapper handles validation of its children. Settings registration and startup commits are retired during callbacks. Callback execution occurs outside SDK locks, before registration freeze, native CALL ledger validation, transport routing, and entry release. A failed Start invokes no validators. Callback nonzero return, C++ exception, or lifecycle misuse prevents startup without retry, rollback, or unload. Ignored invalid, duplicate, stale, foreign-thread, or reentrant registration errors still poison an open startup. Context slots are never reused, with the existing limit of 64 modules.

Callbacks are read-only by contract. Trusted native code can bypass that contract through process APIs; this service does not enforce callback purity, suspend workers, grant native lifetime authority, or establish gameplay compatibility. A callback checks state only at invocation. Mod owners must still exclude later asynchronous writers from their reviewed legacy composition.

## Optional legacy DLL loading

The generic legacy loader reads chainloader.ini beside the companion. Explicit [Load] entries run in file order, followed by optional non-recursive folder discovery. Relative paths resolve beside the companion; absolute paths are accepted. Optional delayed loading does not delay the startup gate. The configuration is trusted local executable-loading input.

When any module registers a final validator, parsed Deferred=true refuses startup before ForwardTo or configured legacy DLL loading. The host does not silently change the setting. Without validators, deferred behavior remains available; a missing configuration retains synchronous defaults.

The old external proxy-chain inventory field is rejected. ForwardTo may direct the debug-help export to a configured module; missing configured targets fail startup. See CREDITS-AND-PROVENANCE.md for the retained upstream license.

## Build and verification

Configure this directory with Visual Studio 2022 x64 and the pinned nlohmann/json include tree. If the worktree submodule is empty, set SHROUDWEAVE_JSON_INCLUDE to the identical pinned include root. Build the shroudweave_server_companion target and run the focused CTest suite with CTest.

The native hooks and byte guards target only the retained dedicated-server profile. Offline fixtures and static checks do not establish a live server launch, native observer availability, Steam message delivery, module compatibility, or gameplay acceptance. This source work does not install or replace any game or server files. Keep the game, installed mods, saves, and hosted servers unchanged.
